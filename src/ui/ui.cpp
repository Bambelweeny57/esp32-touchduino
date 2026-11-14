// TouchDuino touch UI (LVGL 8.4, 480x320 landscape).
//
//  +-----------------------------------------------------------+
//  | [<] /sdcard/Spectrum                     REM:off   [gear] |
//  +-------------------------------------+---------------------+
//  | file list (large touch rows)        |  cover art / badge  |
//  |                                     |  name, type, size   |
//  +-------------------------------------+---------------------+
//  | [========progress========]  Blk 3/12  Turbo  45%          |
//  | [|<] [<<blk] [ PLAY ] [ STOP ] [blk>>] [>|]               |
//  +-----------------------------------------------------------+
//
// Directory scanning and cover decoding run on a worker task pinned to core 0
// (same core as LVGL); the tape engine and its ISR stay alone on core 1.

#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "ui.h"
#include "cover.h"
#include "esp_bsp.h"
#include "app/storage.h"
#include "engine/tape_engine.h"
#include "engine/monitor.h"

static const char *TAG = "ui";

#define PAGE_SIZE   100
#define COVER_W     176
#define COVER_H     120

// ---------------------------------------------------------------------------
// State (touched only with the LVGL lock held)

struct Entry {
    std::string name;
    bool dir;
    uint32_t size;
};

static std::vector<Entry> s_entries;
static std::string s_dir = SD_MOUNT;
static int s_page = 0;
static int s_sel = -1;                 // index into s_entries
static std::string s_playing;          // full path of the file in the engine
static tape_settings_t s_settings;
static tape_state_t s_last_state = TAPE_STOPPED;
static lv_img_dsc_t *s_cover_img = NULL;

// Widgets
static lv_obj_t *scr_main, *scr_settings;
static lv_obj_t *lbl_path, *lbl_rem, *list;
static lv_obj_t *cover_box, *img_cover, *badge, *badge_lbl, *badge_stripes[4];
static lv_obj_t *lbl_name, *lbl_info;
static lv_obj_t *bar_prog, *lbl_status, *lbl_play;
static lv_obj_t *bm_baud, *sw_motor, *sw_pol, *sw_skip2a, *sw_spk, *sl_vol, *lbl_vol;

// ---------------------------------------------------------------------------
// Helpers

static const char *ext_of(const std::string &name)
{
    size_t dot = name.rfind('.');
    return dot == std::string::npos ? "" : name.c_str() + dot + 1;
}

static bool is_tape_ext(const char *e)
{
    static const char *exts[] = { "tzx", "tap", "cdt", "tsx", "uef", "cas", "p", "o", "ay",
                                  "mzf", "mzt", "m12", "mtx", "caq", NULL };
    for (int i = 0; exts[i]; i++) if (!strcasecmp(e, exts[i])) return true;
    return false;
}

static std::string join(const std::string &dir, const std::string &name)
{
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

static std::string parent_of(const std::string &path)
{
    size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return path.substr(0, slash);
}

static bool file_exists(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static const char *format_name(const char *e)
{
    if (!strcasecmp(e, "tzx")) return "ZX Spectrum TZX";
    if (!strcasecmp(e, "tap")) return "TAP (Spectrum/Oric/C64/Jupiter)";
    if (!strcasecmp(e, "cdt")) return "Amstrad CPC CDT";
    if (!strcasecmp(e, "tsx")) return "MSX TSX";
    if (!strcasecmp(e, "uef")) return "Acorn UEF";
    if (!strcasecmp(e, "cas")) return "MSX / Dragon CAS";
    if (!strcasecmp(e, "p"))   return "ZX81 P";
    if (!strcasecmp(e, "o"))   return "ZX80 O";
    if (!strcasecmp(e, "ay"))  return "AY music";
    if (!strcasecmp(e, "mzf") || !strcasecmp(e, "mzt") || !strcasecmp(e, "m12")) return "Sharp MZ";
    if (!strcasecmp(e, "mtx")) return "Memotech MTX";
    if (!strcasecmp(e, "caq")) return "Mattel Aquarius";
    return "";
}

// ---------------------------------------------------------------------------
// System badges (drawn with LVGL primitives, no bitmaps)

enum System { SYS_NONE, SYS_SPECTRUM, SYS_AMSTRAD, SYS_BBC, SYS_MSX, SYS_DRAGON, SYS_TAPE };

static System system_from_name(const std::string &name)
{
    std::string n = name;
    for (auto &c : n) c = (char)tolower((unsigned char)c);
    if (n.find("sinclair") != std::string::npos || n.find("spectrum") != std::string::npos) return SYS_SPECTRUM;
    if (n.find("amstrad") != std::string::npos || n.find("cpc") != std::string::npos) return SYS_AMSTRAD;
    if (n.find("bbc") != std::string::npos) return SYS_BBC;
    if (n.find("msx") != std::string::npos) return SYS_MSX;
    if (n.find("dragon") != std::string::npos) return SYS_DRAGON;
    return SYS_NONE;
}

static System system_from_ext(const char *e)
{
    if (!strcasecmp(e, "tzx")) return SYS_SPECTRUM;
    if (!strcasecmp(e, "cdt")) return SYS_AMSTRAD;
    if (!strcasecmp(e, "uef")) return SYS_BBC;
    if (!strcasecmp(e, "tsx")) return SYS_MSX;
    return SYS_TAPE;
}

static void show_badge(System sys)
{
    static const struct { const char *text; uint32_t bg, fg, s[4]; } B[] = {
        /* NONE */     { "",              0x202020, 0xffffff, { 0, 0, 0, 0 } },
        /* SPECTRUM */ { "ZX\nSPECTRUM",  0x101010, 0xffffff, { 0xd81e1e, 0xf2c500, 0x2fb84a, 0x1ea7e1 } },
        /* AMSTRAD */  { "AMSTRAD\nCPC",  0x1a1a2e, 0xffffff, { 0xe02020, 0x20b040, 0x2050e0, 0x2050e0 } },
        /* BBC */      { "BBC\nMICRO",    0x2a2a2a, 0xf0f0f0, { 0xc00000, 0xc00000, 0xf0f0f0, 0xf0f0f0 } },
        /* MSX */      { "MSX",           0x10204a, 0xffffff, { 0x6080ff, 0x6080ff, 0x6080ff, 0x6080ff } },
        /* DRAGON */   { "DRAGON",        0x3a0d0d, 0xffd0a0, { 0xff7a1a, 0xff7a1a, 0xffb000, 0xffb000 } },
        /* TAPE */     { LV_SYMBOL_AUDIO "\nTAPE", 0x303846, 0xd0d8e8, { 0x8090a8, 0x8090a8, 0x8090a8, 0x8090a8 } },
    };
    if (sys == SYS_NONE) { lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_add_flag(img_cover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(badge, lv_color_hex(B[sys].bg), 0);
    lv_obj_set_style_text_color(badge_lbl, lv_color_hex(B[sys].fg), 0);
    lv_label_set_text(badge_lbl, B[sys].text);
    for (int i = 0; i < 4; i++) lv_obj_set_style_bg_color(badge_stripes[i], lv_color_hex(B[sys].s[i]), 0);
}

static void show_cover_image(lv_img_dsc_t *img)
{
    lv_img_set_src(img_cover, img);
    lv_obj_clear_flag(img_cover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
    if (s_cover_img) {
        lv_img_cache_invalidate_src(s_cover_img);
        cover_free(s_cover_img);
    }
    s_cover_img = img;
}

// ---------------------------------------------------------------------------
// Worker task (core 0): directory scans and cover decoding

enum JobType { JOB_SCAN, JOB_COVER };
struct Job {
    JobType type;
    char path[256];      // directory for SCAN, file (or dir) for COVER
    int select;          // SCAN: entry name to select afterwards is in 'name'
    char name[128];
};

static QueueHandle_t s_jobs;
static volatile uint32_t s_cover_seq = 0;   // newest cover request wins

static void rebuild_list(void);
static void update_preview(void);

static void do_scan(const Job &j)
{
    std::vector<Entry> v;
    DIR *d = opendir(j.path);
    if (!d && sd_is_mounted() && strcmp(j.path, SD_MOUNT) != 0) {
        // remembered folder has gone: fall back to the card root
        Job root = {};
        root.type = JOB_SCAN;
        strcpy(root.path, SD_MOUNT);
        do_scan(root);
        return;
    }
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            std::string name = de->d_name;
            bool dir = de->d_type == DT_DIR;
            if (!dir && !is_tape_ext(ext_of(name))) continue;
            if (!strcasecmp(name.c_str(), "System Volume Information")) continue;
            uint32_t size = 0;
            if (!dir) {
                struct stat st;
                if (stat(join(j.path, name).c_str(), &st) == 0) size = (uint32_t)st.st_size;
            }
            v.push_back({ name, dir, size });
        }
        closedir(d);
    }
    std::sort(v.begin(), v.end(), [](const Entry &a, const Entry &b) {
        if (a.dir != b.dir) return a.dir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    ESP_LOGI(TAG, "scan %s: %u entries%s", j.path, (unsigned)v.size(), d ? "" : " (open failed)");

    bsp_display_lock(0);
    s_entries.swap(v);
    s_dir = j.path;
    s_sel = -1;
    s_page = 0;
    if (j.name[0]) {
        for (size_t i = 0; i < s_entries.size(); i++)
            if (s_entries[i].name == j.name) { s_sel = (int)i; s_page = (int)i / PAGE_SIZE; break; }
    }
    if (!d) {
        lv_label_set_text(lbl_path, sd_is_mounted() ? "Cannot open folder" : "No SD card - tap " LV_SYMBOL_REFRESH);
    } else {
        lv_label_set_text(lbl_path, s_dir.c_str());
    }
    rebuild_list();
    update_preview();
    bsp_display_unlock();
    if (d) settings_save_dir(j.path);
}

static void do_cover(const Job &j, uint32_t seq)
{
    std::string path = j.path;
    lv_img_dsc_t *img = NULL;
    System sys = SYS_NONE;
    std::string dir;

    struct stat st;
    bool is_dir = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    if (!is_dir) {
        // 1. game.jpg / game.png next to game.tzx
        size_t dot = path.rfind('.');
        std::string stem = dot == std::string::npos ? path : path.substr(0, dot);
        static const char *img_ext[] = { ".jpg", ".png", ".jpeg", NULL };
        for (int i = 0; img_ext[i] && !img; i++) {
            std::string p = stem + img_ext[i];
            if (file_exists(p)) img = cover_decode(p.c_str(), COVER_W, COVER_H);
        }
        dir = parent_of(path);
    } else {
        dir = path;
    }

    // 2. folder.jpg / folder.png or a system name, walking up to /sdcard
    while (!img && sys == SYS_NONE && dir.size() >= strlen(SD_MOUNT)) {
        if (seq != s_cover_seq) return;
        for (const char *f : { "folder.jpg", "folder.png" }) {
            std::string p = join(dir, f);
            if (file_exists(p)) { img = cover_decode(p.c_str(), COVER_W, COVER_H); if (img) break; }
        }
        if (img) break;
        size_t slash = dir.rfind('/');
        sys = system_from_name(slash == std::string::npos ? dir : dir.substr(slash + 1));
        if (dir == SD_MOUNT) break;
        dir = parent_of(dir);
    }
    // 3. fall back to a badge from the file type
    if (!img && sys == SYS_NONE) sys = is_dir ? SYS_TAPE : system_from_ext(ext_of(path));

    bsp_display_lock(0);
    if (seq == s_cover_seq) {
        if (img) show_cover_image(img);
        else show_badge(sys);
        img = NULL;
    }
    bsp_display_unlock();
    if (img) cover_free(img);   // superseded by a newer request
}

static void worker_task(void *arg)
{
    Job j;
    for (;;) {
        if (xQueueReceive(s_jobs, &j, portMAX_DELAY) != pdTRUE) continue;
        if (j.type == JOB_SCAN) {
            if (!sd_is_mounted()) sd_mount();
            do_scan(j);
        } else {
            uint32_t seq = (uint32_t)j.select;
            // Drop stale cover jobs if newer ones are queued
            if (seq != s_cover_seq) continue;
            do_cover(j, seq);
        }
    }
}

static void request_scan(const std::string &dir, const std::string &select_name = "")
{
    Job j = {};
    j.type = JOB_SCAN;
    strncpy(j.path, dir.c_str(), sizeof(j.path) - 1);
    strncpy(j.name, select_name.c_str(), sizeof(j.name) - 1);
    xQueueSend(s_jobs, &j, 0);
}

static void request_cover(const std::string &path)
{
    Job j = {};
    j.type = JOB_COVER;
    strncpy(j.path, path.c_str(), sizeof(j.path) - 1);
    s_cover_seq = s_cover_seq + 1;
    j.select = (int)s_cover_seq;
    xQueueSend(s_jobs, &j, 0);
}

// ---------------------------------------------------------------------------
// File list

static void list_item_cb(lv_event_t *e);

static void add_item(const char *icon, const char *text, intptr_t tag, bool selected)
{
    lv_obj_t *b = lv_list_add_btn(list, icon, text);
    lv_obj_set_height(b, 44);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_16, 0);
    lv_obj_add_event_cb(b, list_item_cb, LV_EVENT_CLICKED, (void *)tag);
    lv_obj_set_user_data(b, (void *)tag);
    lv_obj_t *lbl = lv_obj_get_child(b, 1);
    if (lbl) lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    if (selected) lv_obj_add_state(b, LV_STATE_CHECKED);
}

#define TAG_PREV_PAGE (-2)
#define TAG_NEXT_PAGE (-3)
#define TAG_PARENT    (-4)

static void rebuild_list(void)
{
    lv_obj_clean(list);
    if (s_dir != SD_MOUNT) add_item(LV_SYMBOL_UP, "..", TAG_PARENT, false);
    if (s_entries.empty()) {
        lv_obj_t *l = lv_list_add_text(list, "No tape files here");
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    }
    int pages = ((int)s_entries.size() + PAGE_SIZE - 1) / PAGE_SIZE;
    if (s_page > 0) add_item(LV_SYMBOL_UP, "Previous page", TAG_PREV_PAGE, false);
    int start = s_page * PAGE_SIZE;
    int end = std::min((int)s_entries.size(), start + PAGE_SIZE);
    lv_obj_t *sel_btn = NULL;
    for (int i = start; i < end; i++) {
        const Entry &en = s_entries[i];
        add_item(en.dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE, en.name.c_str(), i, i == s_sel);
        if (i == s_sel) sel_btn = lv_obj_get_child(list, lv_obj_get_child_cnt(list) - 1);
    }
    if (s_page < pages - 1) add_item(LV_SYMBOL_DOWN, "Next page", TAG_NEXT_PAGE, false);
    if (sel_btn) lv_obj_scroll_to_view(sel_btn, LV_ANIM_OFF);
}

static void mark_selection(void)
{
    uint32_t n = lv_obj_get_child_cnt(list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(list, i);
        intptr_t tag = (intptr_t)lv_obj_get_user_data(c);
        if (tag == s_sel && tag >= 0) {
            lv_obj_add_state(c, LV_STATE_CHECKED);
            lv_obj_scroll_to_view(c, LV_ANIM_ON);
        } else {
            lv_obj_clear_state(c, LV_STATE_CHECKED);
        }
    }
}

static void update_preview(void)
{
    if (s_sel < 0 || s_sel >= (int)s_entries.size()) {
        lv_label_set_text(lbl_name, s_dir == SD_MOUNT ? "TouchDuino" : s_dir.substr(s_dir.rfind('/') + 1).c_str());
        lv_label_set_text_fmt(lbl_info, "%u items", (unsigned)s_entries.size());
        request_cover(s_dir);
        return;
    }
    const Entry &en = s_entries[s_sel];
    lv_label_set_text(lbl_name, en.name.c_str());
    if (en.dir) {
        lv_label_set_text(lbl_info, "Folder");
    } else {
        const char *fmt = format_name(ext_of(en.name));
        if (en.size >= 10240) lv_label_set_text_fmt(lbl_info, "%s\n%u KB", fmt, (unsigned)(en.size / 1024));
        else lv_label_set_text_fmt(lbl_info, "%s\n%u bytes", fmt, (unsigned)en.size);
    }
    request_cover(join(s_dir, en.name));
}

static void select_index(int idx)
{
    s_sel = idx;
    int page = idx >= 0 ? idx / PAGE_SIZE : s_page;
    if (page != s_page) { s_page = page; rebuild_list(); }
    else mark_selection();
    update_preview();
}

static void enter_dir(const std::string &dir, const std::string &select_name = "")
{
    lv_label_set_text(lbl_path, "Loading...");
    request_scan(dir, select_name);
}

static void go_parent(void)
{
    if (s_dir == SD_MOUNT || s_dir.size() <= strlen(SD_MOUNT)) return;
    std::string leaving = s_dir.substr(s_dir.rfind('/') + 1);
    enter_dir(parent_of(s_dir), leaving);
}

static void play_selected(void)
{
    if (s_sel < 0 || s_sel >= (int)s_entries.size() || s_entries[s_sel].dir) {
        // nothing selected: pick the first file
        for (size_t i = 0; i < s_entries.size(); i++)
            if (!s_entries[i].dir) { select_index((int)i); break; }
        if (s_sel < 0 || s_entries[s_sel].dir) return;
    }
    s_playing = join(s_dir, s_entries[s_sel].name);
    tape_play(s_playing.c_str());
}

static void list_item_cb(lv_event_t *e)
{
    intptr_t tag = (intptr_t)lv_event_get_user_data(e);
    if (tag == TAG_PARENT) { go_parent(); return; }
    if (tag == TAG_PREV_PAGE) { s_page--; rebuild_list(); return; }
    if (tag == TAG_NEXT_PAGE) { s_page++; rebuild_list(); return; }
    if (tag < 0 || tag >= (intptr_t)s_entries.size()) return;
    const Entry &en = s_entries[tag];
    if (en.dir) { enter_dir(join(s_dir, en.name)); return; }
    if (tag == s_sel && s_last_state == TAPE_STOPPED) { play_selected(); return; }   // second tap plays
    select_index((int)tag);
}

static void step_file(int dir)
{
    if (s_entries.empty()) return;
    int n = (int)s_entries.size();
    int i = s_sel;
    for (int k = 0; k < n; k++) {
        i = (i + dir + n) % n;
        if (i < 0) i = 0;
        if (!s_entries[i].dir) break;
    }
    if (s_entries[i].dir) return;
    if (s_last_state != TAPE_STOPPED) tape_stop();
    select_index(i);
}

// ---------------------------------------------------------------------------
// Transport buttons

enum Btn { B_PREV, B_BLK_PREV, B_PLAY, B_STOP, B_BLK_NEXT, B_NEXT };

static void transport_cb(lv_event_t *e)
{
    switch ((intptr_t)lv_event_get_user_data(e)) {
        case B_PREV:     step_file(-1); break;
        case B_NEXT:     step_file(+1); break;
        case B_BLK_PREV: tape_block_skip(-1); break;
        case B_BLK_NEXT: tape_block_skip(+1); break;
        case B_STOP:     tape_stop(); break;
        case B_PLAY:
            if (s_last_state == TAPE_STOPPED) play_selected();
            else tape_pause_toggle();
            break;
    }
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, intptr_t id, lv_color_t color, lv_obj_t **lbl_out = NULL)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 74, 54);
    lv_obj_set_style_bg_color(b, color, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_add_event_cb(b, transport_cb, LV_EVENT_CLICKED, (void *)id);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_center(l);
    if (lbl_out) *lbl_out = l;
    return b;
}

// ---------------------------------------------------------------------------
// Status polling

static void status_timer_cb(lv_timer_t *t)
{
    tape_status_t st;
    tape_get_status(&st);

    if (st.state != s_last_state) {
        lv_label_set_text(lbl_play, st.state == TAPE_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        s_last_state = st.state;
    }

    int pct = st.size ? (int)((uint64_t)st.pos * 100 / st.size) : 0;
    if (st.state == TAPE_STOPPED) pct = 0;
    lv_bar_set_value(bar_prog, pct, LV_ANIM_OFF);

    char buf[96];
    if (st.state == TAPE_STOPPED) {
        snprintf(buf, sizeof(buf), "%s", st.msg[0] ? st.msg : "Ready");
    } else if (st.block_count > 0 && st.block >= 0) {
        snprintf(buf, sizeof(buf), "%s  Blk %d/%d  %s  %d%%", st.state == TAPE_PAUSED ? "PAUSED" : "PLAY",
                 st.block + 1, st.block_count, tape_block_name(st.block_id), pct);
    } else {
        snprintf(buf, sizeof(buf), "%s  %s  %d%%", st.state == TAPE_PAUSED ? "PAUSED" : "PLAY",
                 tape_block_name(st.block_id), pct);
    }
    lv_label_set_text(lbl_status, buf);

    if (s_settings.motor_auto) {
        lv_label_set_text(lbl_rem, st.motor_on ? "REM " LV_SYMBOL_PLAY : "REM " LV_SYMBOL_PAUSE);
        lv_obj_set_style_text_color(lbl_rem, st.motor_on ? lv_color_hex(0x40e070) : lv_color_hex(0xe0a040), 0);
    } else {
        lv_label_set_text(lbl_rem, "FORCE RUN");
        lv_obj_set_style_text_color(lbl_rem, lv_color_hex(0x8090a0), 0);
    }
}

// ---------------------------------------------------------------------------
// Settings screen

static const char *baud_map[] = { "1200", "2400", "3150", "3600", "3850", "" };
static const uint16_t baud_vals[] = { 1200, 2400, 3150, 3600, 3850 };

static void settings_from_widgets(void)
{
    uint16_t b = lv_btnmatrix_get_selected_btn(bm_baud);
    if (b < 5) s_settings.baud = baud_vals[b];
    s_settings.motor_auto = lv_obj_has_state(sw_motor, LV_STATE_CHECKED);
    s_settings.polarity = lv_obj_has_state(sw_pol, LV_STATE_CHECKED);
    s_settings.skip2a = lv_obj_has_state(sw_skip2a, LV_STATE_CHECKED);
    s_settings.speaker = lv_obj_has_state(sw_spk, LV_STATE_CHECKED);
    s_settings.volume = lv_slider_get_value(sl_vol);
}

static void settings_to_widgets(void)
{
    for (int i = 0; i < 5; i++) {
        if (baud_vals[i] == s_settings.baud) {
            lv_btnmatrix_set_btn_ctrl(bm_baud, i, LV_BTNMATRIX_CTRL_CHECKED);
            lv_btnmatrix_set_selected_btn(bm_baud, i);
        } else {
            lv_btnmatrix_clear_btn_ctrl(bm_baud, i, LV_BTNMATRIX_CTRL_CHECKED);
        }
    }
    if (s_settings.motor_auto) lv_obj_add_state(sw_motor, LV_STATE_CHECKED); else lv_obj_clear_state(sw_motor, LV_STATE_CHECKED);
    if (s_settings.polarity) lv_obj_add_state(sw_pol, LV_STATE_CHECKED); else lv_obj_clear_state(sw_pol, LV_STATE_CHECKED);
    if (s_settings.skip2a) lv_obj_add_state(sw_skip2a, LV_STATE_CHECKED); else lv_obj_clear_state(sw_skip2a, LV_STATE_CHECKED);
    if (s_settings.speaker) lv_obj_add_state(sw_spk, LV_STATE_CHECKED); else lv_obj_clear_state(sw_spk, LV_STATE_CHECKED);
    lv_slider_set_value(sl_vol, s_settings.volume, LV_ANIM_OFF);
    lv_label_set_text_fmt(lbl_vol, "%d%%", s_settings.volume);
}

static void setting_changed_cb(lv_event_t *e)
{
    settings_from_widgets();
    lv_label_set_text_fmt(lbl_vol, "%d%%", s_settings.volume);
    tape_apply_settings(&s_settings);        // live (e.g. volume while listening)
}

static void settings_done_cb(lv_event_t *e)
{
    settings_from_widgets();
    tape_apply_settings(&s_settings);
    settings_save(&s_settings);
    lv_scr_load(scr_main);
}

static void open_settings_cb(lv_event_t *e)
{
    settings_to_widgets();
    lv_scr_load(scr_settings);
}

static lv_obj_t *settings_row(lv_obj_t *parent, const char *title, const char *hint)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_20, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    if (hint) {
        lv_obj_t *h = lv_label_create(row);
        lv_label_set_text(h, hint);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(h, lv_color_hex(0x9aa4b4), 0);
        lv_obj_align(h, LV_ALIGN_TOP_LEFT, 0, 22);
    }
    return row;
}

static lv_obj_t *settings_switch(lv_obj_t *parent, const char *title, const char *hint)
{
    lv_obj_t *row = settings_row(parent, title, hint);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 64, 34);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(sw, setting_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static void build_settings(void)
{
    scr_settings = lv_obj_create(NULL);
    lv_obj_t *hdr = lv_obj_create(scr_settings);
    lv_obj_set_size(hdr, 480, 48);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lv_label_create(hdr);
    lv_label_set_text(t, LV_SYMBOL_SETTINGS "  Options");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *done = lv_btn_create(hdr);
    lv_obj_set_size(done, 100, 40);
    lv_obj_align(done, LV_ALIGN_RIGHT_MID, 6, 0);
    lv_obj_add_event_cb(done, settings_done_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(done);
    lv_label_set_text(dl, LV_SYMBOL_OK " Done");
    lv_obj_center(dl);

    lv_obj_t *body = lv_obj_create(scr_settings);
    lv_obj_set_size(body, 480, 272);
    lv_obj_set_pos(body, 0, 48);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 6, 0);
    lv_obj_set_style_pad_all(body, 8, 0);

    lv_obj_t *r = settings_row(body, "Baud (TSX / CAS / CDT speed-up)", NULL);
    bm_baud = lv_btnmatrix_create(r);
    lv_btnmatrix_set_map(bm_baud, baud_map);
    lv_btnmatrix_set_btn_ctrl_all(bm_baud, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(bm_baud, true);
    lv_obj_set_size(bm_baud, lv_pct(100), 52);
    lv_obj_align(bm_baud, LV_ALIGN_TOP_LEFT, 0, 26);
    lv_obj_add_event_cb(bm_baud, setting_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    sw_motor = settings_switch(body, "Motor: Auto REM", "On: REM jack pauses/plays. Off: force run");
    sw_spk = settings_switch(body, "Onboard speaker", "Hear the tape signal on the speaker");

    r = settings_row(body, "Speaker volume", NULL);
    sl_vol = lv_slider_create(r);
    lv_slider_set_range(sl_vol, 0, 100);
    lv_obj_set_size(sl_vol, 300, 18);
    lv_obj_align(sl_vol, LV_ALIGN_TOP_LEFT, 8, 32);
    lv_obj_add_event_cb(sl_vol, setting_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lbl_vol = lv_label_create(r);
    lv_obj_align(lbl_vol, LV_ALIGN_TOP_LEFT, 320, 30);
    lv_obj_t *tb = lv_btn_create(r);
    lv_obj_set_size(tb, 76, 40);
    lv_obj_align(tb, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(tb, [](lv_event_t *e) { monitor_test_beep(700); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tl = lv_label_create(tb);
    lv_label_set_text(tl, LV_SYMBOL_VOLUME_MAX " Test");
    lv_obj_center(tl);
    lv_obj_set_height(r, 70);

    sw_pol = settings_switch(body, "Polarity / TSX accel / UEF parity", "MaxDuino multi-purpose flag");
    sw_skip2a = settings_switch(body, "Skip TZX block 2A", "Ignore 'stop tape if 48K' blocks");
}

// ---------------------------------------------------------------------------
// Main screen

static void back_cb(lv_event_t *e) { go_parent(); }
static void refresh_cb(lv_event_t *e) { enter_dir(sd_is_mounted() ? s_dir : std::string(SD_MOUNT)); }

static void build_main(void)
{
    scr_main = lv_obj_create(NULL);
    lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);

    // Header
    lv_obj_t *hdr = lv_obj_create(scr_main);
    lv_obj_set_size(hdr, 480, 44);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 4, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x182030), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_btn_create(hdr);
    lv_obj_set_size(back, 48, 36);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);

    lbl_path = lv_label_create(hdr);
    lv_obj_set_width(lbl_path, 220);
    lv_label_set_long_mode(lbl_path, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(lbl_path, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_path, LV_ALIGN_LEFT_MID, 56, 0);
    lv_label_set_text(lbl_path, "");

    lbl_rem = lv_label_create(hdr);
    lv_obj_set_style_text_font(lbl_rem, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_rem, LV_ALIGN_RIGHT_MID, -108, 0);
    lv_label_set_text(lbl_rem, "");

    lv_obj_t *refresh = lv_btn_create(hdr);
    lv_obj_set_size(refresh, 48, 36);
    lv_obj_align(refresh, LV_ALIGN_RIGHT_MID, -52, 0);
    lv_obj_add_event_cb(refresh, refresh_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(refresh);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_center(rl);

    lv_obj_t *gear = lv_btn_create(hdr);
    lv_obj_set_size(gear, 48, 36);
    lv_obj_align(gear, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(gear, open_settings_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gl = lv_label_create(gear);
    lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
    lv_obj_center(gl);

    // File list
    list = lv_list_create(scr_main);
    lv_obj_set_size(list, 292, 190);
    lv_obj_set_pos(list, 0, 44);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_set_style_border_width(list, 0, 0);

    // Preview pane
    lv_obj_t *pv = lv_obj_create(scr_main);
    lv_obj_set_size(pv, 188, 190);
    lv_obj_set_pos(pv, 292, 44);
    lv_obj_set_style_radius(pv, 0, 0);
    lv_obj_set_style_pad_all(pv, 6, 0);
    lv_obj_set_style_border_width(pv, 0, 0);
    lv_obj_set_style_bg_color(pv, lv_color_hex(0x10141c), 0);
    lv_obj_clear_flag(pv, LV_OBJ_FLAG_SCROLLABLE);

    cover_box = lv_obj_create(pv);
    lv_obj_set_size(cover_box, COVER_W, COVER_H);
    lv_obj_align(cover_box, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(cover_box, 0, 0);
    lv_obj_set_style_border_width(cover_box, 0, 0);
    lv_obj_set_style_bg_color(cover_box, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(cover_box, LV_OBJ_FLAG_SCROLLABLE);

    img_cover = lv_img_create(cover_box);
    lv_obj_center(img_cover);
    lv_obj_add_flag(img_cover, LV_OBJ_FLAG_HIDDEN);

    badge = lv_obj_create(cover_box);
    lv_obj_set_size(badge, COVER_W, COVER_H);
    lv_obj_center(badge);
    lv_obj_set_style_radius(badge, 8, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 4; i++) {
        badge_stripes[i] = lv_obj_create(badge);
        lv_obj_set_size(badge_stripes[i], 10, 70);
        lv_obj_set_style_radius(badge_stripes[i], 0, 0);
        lv_obj_set_style_border_width(badge_stripes[i], 0, 0);
        lv_obj_align(badge_stripes[i], LV_ALIGN_RIGHT_MID, -12 - (3 - i) * 12, 0);
        lv_obj_set_style_transform_angle(badge_stripes[i], 0, 0);
    }
    badge_lbl = lv_label_create(badge);
    lv_obj_set_style_text_font(badge_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(badge_lbl, LV_ALIGN_LEFT_MID, 12, 0);
    show_badge(SYS_TAPE);

    lbl_name = lv_label_create(pv);
    lv_obj_set_width(lbl_name, 176);
    lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_DOT);
    lv_obj_set_height(lbl_name, 20);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_name, LV_ALIGN_TOP_LEFT, 0, COVER_H + 4);
    lv_label_set_text(lbl_name, "TouchDuino");

    lbl_info = lv_label_create(pv);
    lv_obj_set_width(lbl_info, 176);
    lv_obj_set_style_text_font(lbl_info, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_info, lv_color_hex(0x9aa4b4), 0);
    lv_obj_align(lbl_info, LV_ALIGN_TOP_LEFT, 0, COVER_H + 26);
    lv_label_set_text(lbl_info, "");

    // Progress row
    bar_prog = lv_bar_create(scr_main);
    lv_obj_set_size(bar_prog, 180, 14);
    lv_obj_set_pos(bar_prog, 8, 242);
    lv_bar_set_range(bar_prog, 0, 100);

    lbl_status = lv_label_create(scr_main);
    lv_obj_set_width(lbl_status, 280);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lbl_status, 196, 239);
    lv_label_set_text(lbl_status, "Ready");

    // Transport
    lv_obj_t *row = lv_obj_create(scr_main);
    lv_obj_set_size(row, 480, 62);
    lv_obj_set_pos(row, 0, 258);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_color_t grey = lv_color_hex(0x3a4456), blue = lv_color_hex(0x2a5fa8);
    make_btn(row, LV_SYMBOL_PREV, B_PREV, grey);
    make_btn(row, LV_SYMBOL_LEFT LV_SYMBOL_LEFT, B_BLK_PREV, grey);
    make_btn(row, LV_SYMBOL_PLAY, B_PLAY, lv_color_hex(0x1f8a4c), &lbl_play);
    make_btn(row, LV_SYMBOL_STOP, B_STOP, lv_color_hex(0xa83232));
    make_btn(row, LV_SYMBOL_RIGHT LV_SYMBOL_RIGHT, B_BLK_NEXT, grey);
    make_btn(row, LV_SYMBOL_NEXT, B_NEXT, grey);
    (void)blue;
}

extern "C" void ui_init(const tape_settings_t *settings, const char *start_dir)
{
    s_settings = *settings;
    lv_disp_t *disp = lv_disp_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_TEAL),
                                           true, &lv_font_montserrat_14);
    lv_disp_set_theme(disp, th);

    build_main();
    build_settings();
    lv_scr_load(scr_main);

    s_jobs = xQueueCreate(8, sizeof(Job));
    xTaskCreatePinnedToCore(worker_task, "ui_worker", 8192, NULL, 3, NULL, 0);

    lv_timer_create(status_timer_cb, 100, NULL);

    std::string dir = start_dir && strncmp(start_dir, SD_MOUNT, strlen(SD_MOUNT)) == 0 ? start_dir : SD_MOUNT;
    request_scan(dir);
}
