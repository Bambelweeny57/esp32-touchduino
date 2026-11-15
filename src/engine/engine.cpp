// TouchDuino engine glue: replaces MaxDuino.ino (main loop, buttons, display)
// with a FreeRTOS task pinned to core 1 that owns all engine state, plus a
// block table for touch-driven block skipping.

#include <stdio.h>
#include <string.h>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "configs.h"
#include "Arduino.h"
#include "MaxDuino.h"
#include "MaxProcessing.h"
#include "processing_state.h"
#include "current_settings.h"
#include "file_utils.h"
#include "buffer.h"
#include "isr.h"
#include "pinSetup.h"
#include "TimerCounter.h"
#include "monitor.h"
#include "tape_engine.h"

static const char *TAG = "engine";

extern char tapePath[];

// ---- globals MaxDuino.ino used to provide --------------------------------
portMUX_TYPE g_tape_mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t g_tape_level = 0;
bool pauseOn = false;
byte start = 0;
word block = 0;
byte jblks = 1;
byte oldMinBlock = 0;
byte oldMaxBlock = 19;

static char s_msg[48];

void engine_message(const char *msg)
{
    if (strcmp(s_msg, msg) != 0) ESP_LOGI("engine", "msg: %s (pos %lu)", msg, (unsigned long)bytesRead);
    strncpy(s_msg, msg, sizeof(s_msg) - 1);
    s_msg[sizeof(s_msg) - 1] = 0;
}

void tape_out_init(void)
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << TAPE_OUT_GPIO;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level((gpio_num_t)TAPE_OUT_GPIO, 0);
}

void seekFile() {}   // directory handling lives in the UI

void stopFile()
{
    UniStop();
    start = 0;
    pauseOn = false;
}

void block_mem_oled() {}   // block positions come from the pre-scan table

// ---- block table -----------------------------------------------------------
enum class BlockKind { NONE, TZX, TAP };
struct BlockEntry { uint32_t offset; uint8_t id; };
static std::vector<BlockEntry> s_blocks;
static BlockKind s_kind = BlockKind::NONE;
static bool s_jtap = false;

static bool rd(FILE *f, uint32_t off, void *buf, size_t n)
{
    return fseek(f, (long)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}
static uint32_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le24(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// Walk a TZX/TSX/CDT file. Navigable entries are data blocks; a run of
// pure tone / pulse sequence blocks (0x12/0x13) is grouped with the data
// block that follows it so turbo loaders keep their pilot and sync.
static void scan_tzx(FILE *f, uint32_t size)
{
    uint32_t off = 10;
    int64_t pending = -1;
    uint8_t h[0x14];
    while (off < size && s_blocks.size() < 4096) {
        uint8_t id;
        if (!rd(f, off, &id, 1)) break;
        uint32_t p = off + 1, len;
        memset(h, 0, sizeof(h));
        rd(f, p, h, (size - p) < sizeof(h) ? (size - p) : sizeof(h));
        switch (id) {
            case 0x10: len = 4 + le16(h + 2); break;
            case 0x11: len = 0x12 + le24(h + 0x0F); break;
            case 0x12: len = 4; break;
            case 0x13: len = 1 + h[0] * 2; break;
            case 0x14: len = 0x0A + le24(h + 7); break;
            case 0x15: len = 8 + le24(h + 5); break;
            case 0x18: case 0x19: len = 4 + le32(h); break;
            case 0x20: case 0x23: case 0x24: len = 2; break;
            case 0x21: len = 1 + h[0]; break;
            case 0x22: case 0x25: case 0x27: len = 0; break;
            case 0x26: len = 2 + le16(h) * 2; break;
            case 0x28: len = 2 + le16(h); break;
            case 0x2A: len = 4; break;
            case 0x2B: len = 5; break;
            case 0x30: len = 1 + h[0]; break;
            case 0x31: len = 2 + h[1]; break;
            case 0x32: len = 2 + le16(h); break;
            case 0x33: len = 1 + h[0] * 3; break;
            case 0x35: len = 0x14 + le32(h + 0x10); break;
            case 0x4B: len = 4 + le32(h); break;
            case 0x5A: len = 9; break;
            default:   len = 4 + le32(h); break;   // TZX extension rule
        }
        switch (id) {
            case 0x12: case 0x13:
                if (pending < 0) pending = off;
                break;
            case 0x10: case 0x11: case 0x14: case 0x15: case 0x18: case 0x19: case 0x4B:
                s_blocks.push_back({ (uint32_t)(pending >= 0 ? pending : off), id });
                pending = -1;
                break;
            case 0x20: case 0x21: case 0x22: case 0x30: case 0x31: case 0x32: case 0x33: case 0x35: case 0x5A:
                break;   // informational / pauses don't break a pilot run
            default:
                pending = -1;
                break;
        }
        off = p + len;
    }
}

static void scan_tap(FILE *f, uint32_t size)
{
    uint32_t off = 0;
    uint8_t h[2];
    while (off + 2 <= size && s_blocks.size() < 4096) {
        if (!rd(f, off, h, 2)) break;
        uint32_t len = le16(h);
        s_blocks.push_back({ off, (uint8_t)BLOCKID::TAP });
        off += 2 + len;
    }
}

static void build_block_table(const char *path, const char *ext)
{
    s_blocks.clear();
    s_kind = BlockKind::NONE;
    bool tzx = !strcasecmp(ext, "tzx") || !strcasecmp(ext, "tsx") || !strcasecmp(ext, "cdt");
    bool tap = !strcasecmp(ext, "tap") && (currentID == BLOCKID::TAP || currentID == BLOCKID::JTAP);
    if (!tzx && !tap) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (tzx) { s_kind = BlockKind::TZX; scan_tzx(f, filesize); }
    else     { s_kind = BlockKind::TAP; s_jtap = currentID == BLOCKID::JTAP; scan_tap(f, filesize); }
    fclose(f);
    ESP_LOGI(TAG, "%u navigable blocks (file %lu bytes)", (unsigned)s_blocks.size(), (unsigned long)filesize);
    for (size_t i = 0; i < s_blocks.size(); i++)
        ESP_LOGI(TAG, "  blk %u: id 0x%02x @ %lu", (unsigned)(i + 1), s_blocks[i].id, (unsigned long)s_blocks[i].offset);
}

static int current_block_index(void)
{
    if (s_blocks.empty()) return -1;
    uint32_t pos = bytesRead;
    int lo = 0, hi = (int)s_blocks.size() - 1, ans = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_blocks[mid].offset < pos) { ans = mid; lo = mid + 1; } else hi = mid - 1;
    }
    return ans;
}

static void jump_to_block(int idx)
{
    if (s_blocks.empty() || start != 1) return;
    if (idx < 0) idx = 0;
    if (idx >= (int)s_blocks.size()) idx = (int)s_blocks.size() - 1;
    clearBuffer();
    bytesRead = s_blocks[idx].offset;
    currentBit = 0;
    pass = 0;
    EndOfFile = false;
    count_r = 255;
    temppause = 0;
    currentBlockTask = BLOCKTASK::READPARAM;
    if (s_kind == BlockKind::TZX) {
        currentTask = TASK::GETID;
    } else {
        currentTask = TASK::PROCESSID;
        currentID = s_jtap ? BLOCKID::JTAP : BLOCKID::TAP;
        block = idx;
        jtapflag = (idx % 2 == 0) ? 255 : 0;
    }
    Timer.setPeriod(1000);
    ESP_LOGI(TAG, "jump to block %d @%lu", idx, (unsigned long)bytesRead);
}

// ---- command queue -----------------------------------------------------------
enum CmdType { CMD_PLAY, CMD_PAUSE, CMD_STOP, CMD_SKIP, CMD_SETTINGS };
struct Cmd {
    CmdType type;
    int arg;
    tape_settings_t settings;
    char path[256];
};

static QueueHandle_t s_queue;
static tape_settings_t s_settings;
static volatile tape_status_t s_status;
static bool s_last_motor = true;

static bool motor_line_on(void) { return gpio_get_level((gpio_num_t)TAPE_MOTOR_GPIO) == 0; }

static void apply_settings(const tape_settings_t *s)
{
    s_settings = *s;
    BAUDRATE = s->baud;
    mselectMask = s->motor_auto;
    TSXCONTROLzxpolarityUEFSWITCHPARITY = s->polarity;
    skip2A = s->skip2a;
    monitor_set_enabled(s->speaker);
    monitor_set_volume(s->volume);
}

static void do_play(const char *path)
{
    if (start == 1) stopFile();
    strncpy(tapePath, path, 299);
    tapePath[299] = 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strncpy(fileName, base, 255);
    fileName[255] = 0;
    const char *dot = strrchr(fileName, '.');
    if (!dot) { engine_message("No file extension"); return; }

    FILE *f = fopen(path, "rb");
    if (!f) { engine_message("Cannot open file"); return; }
    fseek(f, 0, SEEK_END);
    filesize = (unsigned long)ftell(f);
    fclose(f);

    engine_message("Playing");
    pauseOn = false;
    UniPlay();                       // opens the file, detects format, starts the ISR
    start = 1;
    build_block_table(path, dot + 1);

    s_last_motor = true;             // as MaxDuino: start paused if REM says stop
    ESP_LOGI(TAG, "play %s (%lu bytes)", path, (unsigned long)filesize);
}

static void handle(const Cmd &c)
{
    switch (c.type) {
        case CMD_PLAY: do_play(c.path); break;
        case CMD_PAUSE:
            if (start == 1) {
                pauseOn = !pauseOn;
                engine_message(pauseOn ? "Paused" : "Playing");
            }
            break;
        case CMD_STOP:
            if (start == 1) { stopFile(); engine_message("Stopped"); }
            break;
        case CMD_SKIP: {
            int cur = current_block_index();
            jump_to_block(cur + c.arg);
            break;
        }
        case CMD_SETTINGS: apply_settings(&c.settings); break;
    }
}

static void update_status(void)
{
    // Diagnostics: log block ID / state transitions (low rate, engine task only)
    static uint8_t last_id = 0xFF, last_state = 0xFF;
    uint8_t state_now = start != 1 ? 0 : (pauseOn ? 2 : 1);
    if (currentID != last_id || state_now != last_state) {
        ESP_LOGI(TAG, "%s id 0x%02x task %d blk %d @ %lu/%lu",
                 state_now == 0 ? "STOP" : state_now == 2 ? "PAUSE" : "PLAY", currentID, (int)currentBlockTask,
                 start == 1 ? current_block_index() + 1 : 0, (unsigned long)bytesRead, (unsigned long)filesize);
        last_id = currentID;
        last_state = state_now;
    }

    tape_status_t st = {};
    st.state = start != 1 ? TAPE_STOPPED : (pauseOn ? TAPE_PAUSED : TAPE_PLAYING);
    st.pos = bytesRead;
    st.size = filesize;
    st.block = start == 1 ? current_block_index() : -1;
    st.block_count = (int)s_blocks.size();
    st.block_id = currentID;
    st.motor_on = motor_line_on();
    st.phase = TAPE_PHASE_IDLE;
    if (start == 1 && !pauseOn) {
        if (currentID == BLOCKID::IDPAUSE || currentBlockTask == BLOCKTASK::PAUSE) {
            st.phase = TAPE_PHASE_GAP;
        } else {
            switch (currentBlockTask) {
                case BLOCKTASK::PILOT:
                case BLOCKTASK::SYNC1:
                case BLOCKTASK::SYNC2:
                    st.phase = TAPE_PHASE_PILOT;
                    break;
                default:
                    st.phase = TAPE_PHASE_DATA;
                    break;
            }
        }
    }
    snprintf(st.msg, sizeof(st.msg), "%s", s_msg);
    snprintf(st.file, sizeof(st.file), "%s", fileName);
    noInterrupts();
    memcpy((void *)&s_status, &st, sizeof(st));
    interrupts();
}

static void engine_task(void *arg)
{
    tape_timer_create();             // ISR allocated on this core (1)
    UniSetup();
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << TAPE_MOTOR_GPIO;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);

    Cmd c;
    uint32_t last_status = 0;
    for (;;) {
        while (xQueueReceive(s_queue, &c, start == 1 ? 0 : pdMS_TO_TICKS(20)) == pdTRUE) {
            handle(c);
        }

        if (start == 1) {
            if (mselectMask) {
                bool m = motor_line_on();
                if (m != s_last_motor) {
                    if (!m && !pauseOn) { pauseOn = true; engine_message("REM: paused"); }
                    if (m && pauseOn)   { pauseOn = false; engine_message("REM: playing"); }
                    s_last_motor = m;
                }
            }
            // Fill the buffer; once full, sleep a tick (a page lasts >= 85 ms)
            for (int i = 0; i < 256 && start == 1; i++) {
                UniLoop();
                if (writepos >= buffsize && !morebuff) break;
            }
            if (start == 1 && writepos >= buffsize && !morebuff) vTaskDelay(1);
        } else {
            WRITE_LOW;
        }

        uint32_t now = millis();
        if (now - last_status > 50) {
            update_status();
            last_status = now;
        }
    }
}

// ---- public API ---------------------------------------------------------------
static void post(const Cmd &c) { if (s_queue) xQueueSend(s_queue, &c, pdMS_TO_TICKS(100)); }

extern "C" void tape_engine_start(const tape_settings_t *initial)
{
    s_queue = xQueueCreate(8, sizeof(Cmd));
    apply_settings(initial);
    monitor_start();
    xTaskCreatePinnedToCore(engine_task, "tape", 6144, NULL, configMAX_PRIORITIES - 2, NULL, 1);
}

extern "C" void tape_play(const char *path)
{
    Cmd c = {};
    c.type = CMD_PLAY;
    strncpy(c.path, path, sizeof(c.path) - 1);
    post(c);
}

extern "C" void tape_pause_toggle(void) { Cmd c = {}; c.type = CMD_PAUSE; post(c); }
extern "C" void tape_stop(void) { Cmd c = {}; c.type = CMD_STOP; post(c); }
extern "C" void tape_block_skip(int delta) { Cmd c = {}; c.type = CMD_SKIP; c.arg = delta; post(c); }

extern "C" void tape_apply_settings(const tape_settings_t *s)
{
    Cmd c = {};
    c.type = CMD_SETTINGS;
    c.settings = *s;
    post(c);
}

extern "C" void tape_get_status(tape_status_t *out)
{
    noInterrupts();
    memcpy(out, (const void *)&s_status, sizeof(*out));
    interrupts();
}

extern "C" const char *tape_block_name(uint8_t id)
{
    switch (id) {
        case 0x10: return "Standard";
        case 0x11: return "Turbo";
        case 0x12: return "Tone";
        case 0x13: return "Pulses";
        case 0x14: return "Pure data";
        case 0x15: return "Direct rec";
        case 0x19: return "Generalized";
        case 0x20: return "Pause";
        case 0x4B: return "Kansas City";
        case BLOCKID::TAP: return "TAP";
        case BLOCKID::JTAP: return "Jupiter TAP";
        case BLOCKID::ORIC: return "Oric";
        case BLOCKID::UEF: return "UEF";
        case BLOCKID::ZXP: return "ZX81 P";
        case BLOCKID::ZXO: return "ZX80 O";
        case BLOCKID::AYO: return "AY";
        case BLOCKID::C64TAP: return "C64 TAP";
        case BLOCKID::MZF: return "Sharp MZ";
        case BLOCKID::MTX: return "MTX";
        case BLOCKID::CAQ: return "Aquarius";
        case BLOCKID::IDPAUSE: return "Pause";
        case BLOCKID::IDEOF: return "End";
        default: return "";
    }
}
