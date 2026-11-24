// Speaker output via the onboard I2S class-D amplifier.
//
// The tape ISR (core 1) records every level change with its timestamp on the
// tape timeline. This task (core 0) replays those edges as a square wave into
// I2S at 44.1 kHz, running a fixed ~40 ms behind the live signal. It never
// touches the tape timing itself, so a slow or stalled speaker task can't
// disturb loading.

#include "monitor.h"
#include "TimerCounter.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdlib.h>

static const char *TAG = "monitor";

#define MON_I2S_BCK   42
#define MON_I2S_LRCK  2
#define MON_I2S_DOUT  41
#define MON_RATE      44100
#define MON_FRAMES    256
#define MON_LATENCY_US 40000
#define MON_RESYNC_US  30000

mon_edge_t g_mon_ring[MON_RING_SIZE];
volatile uint32_t g_mon_head = 0;
volatile uint32_t g_mon_tail = 0;
volatile uint32_t g_mon_drops = 0;
volatile uint8_t g_mon_last_level = 0;
volatile bool g_mon_enabled = true;

static volatile int s_volume = 40;
static i2s_chan_handle_t s_tx = NULL;

void monitor_set_enabled(bool on) { g_mon_enabled = on; }
void monitor_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
}

static volatile int s_beep_frames = 0;

void monitor_test_beep(int ms) { s_beep_frames = ms * (MON_RATE / 1000); }

// ---- Self-check: decode the generated signal like the Spectrum ROM loader ----
// Runs on the same edge stream the ISR records (scheduled edge times), so it
// checks the bytes and pulse lengths TouchDuino produces, independent of the
// analog side. Logs each standard-speed block: length, flag, XOR parity and
// the min/max half-pulse lengths seen for 0 and 1 bits.
static struct {
    int state;            // 0 idle/pilot, 1 sync1 seen, 2 data
    int pilot;
    uint32_t last_t;
    uint32_t half;        // first half of the current bit (0 = none)
    uint8_t cur, nbits, flag, parity;
    uint32_t bytes, bad_halves;
    uint32_t min0, max0, min1, max1;
    int block;
} sc;

static void sc_reset_block(void)
{
    sc.state = 0; sc.pilot = 0; sc.half = 0;
    sc.cur = 0; sc.nbits = 0; sc.parity = 0; sc.bytes = 0; sc.bad_halves = 0;
    sc.min0 = sc.min1 = 0xFFFFFFFF; sc.max0 = sc.max1 = 0;
}

static void sc_finish(const char *why)
{
    if (sc.state == 2 && sc.bytes > 0) {
        ESP_LOGI(TAG, "selfcheck block %d: %lu bytes, flag 0x%02X, parity %s, %u stray bits, "
                 "0-half %lu..%lu us, 1-half %lu..%lu us, odd halves %lu (%s)",
                 sc.block, (unsigned long)sc.bytes, sc.flag, sc.parity == 0 ? "OK" : "BAD",
                 sc.nbits, (unsigned long)sc.min0, (unsigned long)sc.max0,
                 (unsigned long)sc.min1, (unsigned long)sc.max1, (unsigned long)sc.bad_halves, why);
        sc.block++;
    }
    sc_reset_block();
}

static void sc_edge(uint32_t t)
{
    uint32_t d = t - sc.last_t;
    sc.last_t = t;
    if (d > 1500) {                       // pause / silence ends a block
        sc_finish("gap");
        return;
    }
    switch (sc.state) {
    case 0:
        if (d >= 500 && d <= 750) sc.pilot++;
        else if (d < 300 && sc.pilot >= 200) sc.state = 1;   // sync 1
        else sc.pilot = 0;
        break;
    case 1:
        if (d < 300) { sc.state = 2; sc.half = 0; }         // sync 2
        else sc_reset_block();
        break;
    case 2: {
        if (sc.half == 0) { sc.half = d; break; }
        uint32_t a = sc.half, sum = a + d;
        sc.half = 0;
        int bit = sum > 733 ? 1 : 0;
        if (d >= 500 && d <= 750 && a >= 500 && a <= 750) { sc_finish("pilot"); sc.pilot = 2; break; }
        // both halves of a bit should be the same length
        if ((a > d ? a - d : d - a) > 60) sc.bad_halves++;
        uint32_t lo = a < d ? a : d, hi = a > d ? a : d;
        if (bit) { if (lo < sc.min1) sc.min1 = lo; if (hi > sc.max1) sc.max1 = hi; }
        else     { if (lo < sc.min0) sc.min0 = lo; if (hi > sc.max0) sc.max0 = hi; }
        sc.cur = (uint8_t)((sc.cur << 1) | bit);
        if (++sc.nbits == 8) {
            if (sc.bytes == 0) sc.flag = sc.cur;
            sc.parity ^= sc.cur;
            sc.bytes++;
            sc.nbits = 0; sc.cur = 0;
        }
        break;
    }
    }
}

// ---- Self-check for C64 tapes -------------------------------------------------
static uint16_t *volatile s_c64_next = NULL;
static volatile uint32_t s_c64_next_n = 0;
static volatile bool s_c64_stop = false;
struct C64Check {
    uint16_t *exp;
    uint32_t n, idx, good, bad, paused, slips, first_bad, first_got, drops0;
    uint32_t last_fall;
    bool have, reported;
    int worst;
    int badrun;
    uint32_t hist[16];
    uint32_t nh;
    int64_t stop_at;
};
static C64Check c6;

void c64check_start(uint16_t *pulses, uint32_t count)
{
    s_c64_stop = false;
    s_c64_next_n = count;
    __sync_synchronize();
    s_c64_next = pulses;
}

void c64check_stop(void) { s_c64_stop = true; }

static void c6_report(const char *why)
{
    if (!c6.exp || c6.reported) return;
    c6.reported = true;
    uint32_t drops = g_mon_drops - c6.drops0;
    ESP_LOGI(TAG, "c64check %s: %lu of %lu pulses, %lu ok (worst %d us), %lu BAD, %lu paused, %lu slips, %lu edges not recorded -> %s",
             why, (unsigned long)c6.idx, (unsigned long)c6.n, (unsigned long)c6.good, c6.worst,
             (unsigned long)c6.bad, (unsigned long)c6.paused, (unsigned long)c6.slips, (unsigned long)drops,
             c6.idx < 2 ? "NOTHING CHECKED" : (c6.bad || c6.slips) ? "BAD" : "OK");
    if (c6.bad) ESP_LOGW(TAG, "c64check first bad: pulse %lu sent %lu us, file %u us", (unsigned long)c6.first_bad,
                         (unsigned long)c6.first_got, (unsigned)c6.exp[c6.first_bad]);
}

static void c6_fall(uint32_t t)
{
    if (c6.reported) return;
    uint32_t d = t - c6.last_fall;
    c6.last_fall = t;
    if (!c6.have) { c6.have = true; return; }   // start of pulse 1 (pulse 0 starts from an already low line)
    int diff = (int)d - (int)c6.exp[c6.idx];
    c6.hist[c6.nh++ & 15] = d;
    if (diff >= 40000) {                        // paused (by hand or REM) in this pulse
        c6.paused++;
        c6.nh = 0;
        c6.badrun = 0;
    } else if (diff > 20 || diff < -20) {
        if (!c6.bad) { c6.first_bad = c6.idx; c6.first_got = d; }
        c6.bad++;
        if (++c6.badrun >= 16 && c6.nh >= 16) { // lost step? look 8 pulses either side
            for (int s = -8; s <= 8; s++) {
                int64_t base = (int64_t)c6.idx - 15 + s;
                if (!s || base < 0 || base + 16 > (int64_t)c6.n) continue;
                int k = 0;
                while (k < 16) {
                    int e = (int)c6.hist[(c6.nh - 16 + k) & 15] - (int)c6.exp[base + k];
                    if (e > 20 || e < -20) break;
                    k++;
                }
                if (k == 16) { c6.idx += s; c6.slips++; break; }
            }
            c6.badrun = 0;
        }
    } else {
        c6.good++;
        c6.badrun = 0;
        if (diff < 0) diff = -diff;
        if (diff > c6.worst) c6.worst = diff;
    }
    if (++c6.idx >= c6.n) c6_report("complete");
}

static void c6_poll(void)
{
    uint16_t *nx = s_c64_next;
    if (nx) {
        uint32_t n = s_c64_next_n;
        s_c64_next = NULL;
        free(c6.exp);
        c6 = C64Check();
        c6.exp = nx;
        c6.n = n;
        c6.idx = 1;
        c6.drops0 = g_mon_drops;
        ESP_LOGI(TAG, "c64check: %lu pulses to check", (unsigned long)n);
    }
    if (c6.exp && s_c64_stop) {
        int64_t now = esp_timer_get_time();
        if (!c6.stop_at) {
            c6.stop_at = now + 200000;          // let the last edges through (~40 ms behind)
        } else if (now > c6.stop_at) {
            c6_report("stopped");
            free(c6.exp);
            c6.exp = NULL;
        }
    }
}

static void monitor_task(void *arg)
{
    sc_reset_block();
    static int16_t buf[MON_FRAMES * 2];
    const int64_t step_ns = 1000000000LL / MON_RATE;
    int64_t cursor_ns = 0;
    uint8_t level = 0;
    float x_prev = 0, y_prev = 0;

    for (;;) {
        c6_poll();
        bool on = g_mon_enabled;
        int64_t target_ns = ((int64_t)tape_timer_now_us() - MON_LATENCY_US) * 1000;
        if (llabs(cursor_ns - target_ns) > (int64_t)MON_RESYNC_US * 1000) {
            cursor_ns = target_ns;
        }
        float amp = on ? (float)s_volume * 160.0f : 0.0f;   // 100% -> 16000
        bool beep = s_beep_frames > 0;
        if (beep) amp = (float)(s_volume < 20 ? 20 : s_volume) * 160.0f;

        for (int i = 0; i < MON_FRAMES; i++) {
            cursor_ns += step_ns;
            uint32_t t = (uint32_t)(cursor_ns / 1000);
            uint32_t tail = g_mon_tail;
            while (tail != g_mon_head) {
                const mon_edge_t *e = &g_mon_ring[tail & (MON_RING_SIZE - 1)];
                if ((int32_t)(e->t - t) > 0) break;
                level = e->level;
                if (!c6.exp) sc_edge(e->t);
                else if (!e->level) c6_fall(e->t);
                tail++;
            }
            g_mon_tail = tail;
            if (!c6.exp && sc.state == 2 && (int32_t)(t - sc.last_t) > 200000) sc_finish("end");
            if (beep) {
                level = (s_beep_frames / 22) & 1;   // ~1 kHz square
                if (s_beep_frames > 0) s_beep_frames = s_beep_frames - 1;
            }

            // Square wave through a DC blocker, so long pauses at one level
            // settle to silence instead of holding DC on the speaker
            float x = level ? amp : -amp;
            float y = x - x_prev + 0.995f * y_prev;
            x_prev = x;
            y_prev = y;
            if (y > 32000) y = 32000;
            if (y < -32000) y = -32000;
            buf[i * 2] = buf[i * 2 + 1] = (int16_t)y;
        }
        size_t written = 0;
        i2s_channel_write(s_tx, buf, sizeof(buf), &written, portMAX_DELAY);
    }
}

void monitor_start(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = MON_FRAMES;
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &s_tx, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed");
        return;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MON_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)MON_I2S_BCK,
            .ws = (gpio_num_t)MON_I2S_LRCK,
            .dout = (gpio_num_t)MON_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {},
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    xTaskCreatePinnedToCore(monitor_task, "spk", 4096, NULL, 6, NULL, 0);
    ESP_LOGI(TAG, "speaker monitor on I2S (BCK %d, WS %d, DOUT %d)", MON_I2S_BCK, MON_I2S_LRCK, MON_I2S_DOUT);
}
