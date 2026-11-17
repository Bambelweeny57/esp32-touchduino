// TouchDuino touch UI (LVGL 8.4, 480x320 landscape), "glass" theme.
//
//  +-----------------------------------------------------------+
//  | [<] /sdcard/Spectrum              REM   [refresh] [gear]  |
//  +-------------------------------------+---------------------+
//  | file list (large touch rows)        | +-----------------+ |
//  |                                     | |##  TV screen  ##| |  <- loading-stripe border
//  |                                     | +-----------------+ |
//  |                                     |  name / type, size  |
//  +-------------------------------------+---------------------+
//  | [====progress====]  PLAY  Blk 3/12  Turbo  45%            |
//  | [|<] [<<blk] [ PLAY ] [ STOP ] [blk>>] [>|]               |
//  +-----------------------------------------------------------+
//
// Directory scanning and cover decoding run on a worker task pinned to core 0
// (same core as LVGL); the tape engine and its ISR stay alone on core 1.
//
// Performance notes: the display driver uses full_refresh, so ANY change
// re-renders the whole frame. The "glass" look is therefore faked with opaque
// gradients plus thin translucent borders (no large alpha-blended areas, no
// shadows), and the loading-border animation only invalidates while it is
// actually animating (~12 fps while loading, ~6 fps for the idle placeholder).

#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "ff.h"

#include "ui.h"
#include "cover.h"
#include "esp_bsp.h"
#include "app/storage.h"
#include "engine/tape_engine.h"
#include "engine/monitor.h"
#include "engine/TimerCounter.h"

static const char *TAG = "ui";

#define PAGE_SIZE   100

// Preview "TV": bezel > striped border frame > screen (cover art / badge)
#define TV_FRAME_W  160
#define TV_FRAME_H  108
#define TV_BORDER   8
#define SCREEN_W    (TV_FRAME_W - 2 * TV_BORDER)   // 144
#define SCREEN_H    (TV_FRAME_H - 2 * TV_BORDER)   // 92

#define ANIM_PERIOD_MS 60          // loading border ~16 fps
#define UI_PERF_LOG    0           // log render time per frame every ~5 s of activity
#define UI_ANIM_SELFTEST 0         // 1 = force loading stripes (for measuring render cost)

// BLOCKID values reported in tape_status_t.block_id (see engine/MaxDuino.h)
#define BLK_C64TAP 0xFB
#define BLK_ORIC   0xFA
#define BLK_JTAP   0xF8

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
static lv_obj_t *scr_main, *scr_settings, *scr_usb;
static lv_obj_t *sw_usb, *lbl_usb_state, *lbl_usb_hint;
static lv_obj_t *lbl_path, *lbl_rem, *list;
static lv_obj_t *tv_frame, *tv_screen, *img_cover, *badge, *badge_lbl, *badge_stripes[4], *placeholder_lbl;
static lv_obj_t *lbl_name, *lbl_info;
static lv_obj_t *bar_prog, *lbl_status, *lbl_play;
static lv_obj_t *sw_reminv;
static lv_obj_t *bm_baud, *sw_motor, *sw_pol, *sw_skip2a, *sw_gaps, *sw_spk, *sl_vol, *lbl_vol;

// Styles
static lv_style_t st_screen, st_glass, st_btn, st_item, st_item_chk, st_item_pr, st_item_dis, st_btn_dis;

// Browsing is locked while a tape is playing or paused (press stop to move on)
static bool s_browse_locked = false;
static lv_obj_t *btn_back, *btn_refresh, *btn_prev, *btn_next;
static lv_style_t st_bar_bg, st_bar_ind, st_bezel;

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
    if (!strcasecmp(e, "tap")) return "TAP";
    if (!strcasecmp(e, "cdt")) return "Amstrad CDT";
    if (!strcasecmp(e, "tsx")) return "MSX TSX";
    if (!strcasecmp(e, "uef")) return "Acorn UEF";
    if (!strcasecmp(e, "cas")) return "MSX/Dragon CAS";
    if (!strcasecmp(e, "p"))   return "ZX81 P";
    if (!strcasecmp(e, "o"))   return "ZX80 O";
    if (!strcasecmp(e, "ay"))  return "AY music";
    if (!strcasecmp(e, "mzf") || !strcasecmp(e, "mzt") || !strcasecmp(e, "m12")) return "Sharp MZ";
    if (!strcasecmp(e, "mtx")) return "Memotech MTX";
    if (!strcasecmp(e, "caq")) return "Aquarius CAQ";
    return "";
}

// ---------------------------------------------------------------------------
// Systems: badges, border colours and loading-stripe styles

enum System { SYS_NONE, SYS_SPECTRUM, SYS_AMSTRAD, SYS_BBC, SYS_MSX, SYS_DRAGON, SYS_C64, SYS_TAPE };

// How a system's loaders coloured the border while loading
enum FrameKind { FK_STATIC, FK_SPECTRUM, FK_CPC, FK_C64 };

static const struct {
    const char *text;
    uint32_t bg, fg, s[4];
    uint32_t border;      // idle border colour
    FrameKind kind;
} SYS[] = {
    /* NONE */     { "",                  0x000000, 0xffffff, { 0, 0, 0, 0 },                                   0x262c38, FK_STATIC },
    /* SPECTRUM */ { "ZX\nSPECTRUM",      0x000000, 0xffffff, { 0xd81e1e, 0xf2c500, 0x2fb84a, 0x1ea7e1 },       0x000000, FK_SPECTRUM },
    /* AMSTRAD */  { "AMSTRAD\nCPC",      0x000080, 0xffff00, { 0xe02020, 0x20b040, 0x2050e0, 0x2050e0 },       0x000080, FK_CPC },
    /* BBC */      { "BBC\nMICRO",        0x000000, 0xf0f0f0, { 0xc00000, 0xc00000, 0xf0f0f0, 0xf0f0f0 },       0x000000, FK_STATIC },
    /* MSX */      { "MSX",               0x10204a, 0xffffff, { 0x6080ff, 0x6080ff, 0x6080ff, 0x6080ff },       0x5955e0, FK_STATIC },
    /* DRAGON */   { "DRAGON",            0x0d2a0d, 0x40ff40, { 0xff7a1a, 0xff7a1a, 0xffb000, 0xffb000 },       0x101010, FK_STATIC },
    /* C64 */      { "COMMODORE\n64",     0x40318d, 0x7869c4, { 0x880000, 0xdd8855, 0xeeee77, 0x0088ff },       0x7869c4, FK_C64 },
    /* TAPE */     { "",                  0x000000, 0xffffff, { 0, 0, 0, 0 },                                   0x262c38, FK_SPECTRUM },
};

static System system_from_name(const std::string &name)
{
    std::string n = name;
    for (auto &c : n) c = (char)tolower((unsigned char)c);
    if (n.find("sinclair") != std::string::npos || n.find("spectrum") != std::string::npos) return SYS_SPECTRUM;
    if (n.find("amstrad") != std::string::npos || n.find("cpc") != std::string::npos) return SYS_AMSTRAD;
    if (n.find("bbc") != std::string::npos) return SYS_BBC;
    if (n.find("msx") != std::string::npos) return SYS_MSX;
    if (n.find("dragon") != std::string::npos) return SYS_DRAGON;
    if (n.find("c64") != std::string::npos || n.find("commodore") != std::string::npos) return SYS_C64;
    return SYS_NONE;
}

static System system_from_ext(const char *e)
{
    if (!strcasecmp(e, "tzx") || !strcasecmp(e, "tap")) return SYS_SPECTRUM;
    if (!strcasecmp(e, "cdt")) return SYS_AMSTRAD;
    if (!strcasecmp(e, "uef")) return SYS_BBC;
    if (!strcasecmp(e, "tsx") || !strcasecmp(e, "cas")) return SYS_MSX;
    return SYS_NONE;
}

// System that a tape (or folder) belongs to, for its border: a known folder
// name on the way up wins, then the file extension. No file I/O.
static System frame_system_for(const std::string &path, bool is_dir)
{
    std::string dir = is_dir ? path : parent_of(path);
    while (dir.size() > strlen(SD_MOUNT)) {
        size_t slash = dir.rfind('/');
        System s = system_from_name(dir.substr(slash + 1));
        if (s != SYS_NONE) return s;
        dir = parent_of(dir);
    }
    if (!is_dir) {
        System s = system_from_ext(ext_of(path));
        if (s != SYS_NONE) return s;
    }
    return SYS_TAPE;
}

// ---------------------------------------------------------------------------
// Loading-stripe border

static FrameKind s_play_kind = FK_STATIC;   // stripe style of the tape being played
static bool s_placeholder = false;          // preview shows the default "no image" screen
static uint8_t s_anim_mode = 0;             // 0 static, 1 pilot, 2 data, 3 idle placeholder
static FrameKind s_anim_kind = FK_SPECTRUM;
static uint32_t s_anim_frame = 0;

static const uint32_t CPC_PAL[] = { 0x000080, 0x0000ff, 0xff0000, 0xff00ff, 0x00ff00, 0x00ffff,
                                    0xffff00, 0xffffff, 0xff8000, 0x8000ff, 0x80ff00, 0xff0080 };
static const uint32_t C64_PAL[] = { 0x000000, 0xffffff, 0x880000, 0xaaffee, 0xcc44cc, 0x00cc55,
                                    0x0000aa, 0xeeee77, 0xdd8855, 0x664400, 0xff7777, 0x333333,
                                    0x777777, 0xaaff66, 0x0088ff, 0xbbbbbb };

static inline uint32_t xorshift(uint32_t &s)
{
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

// Draw one horizontal band, but only where it falls on the border (never
// under the screen, which is drawn on top anyway).
static void draw_band(lv_draw_ctx_t *dc, lv_draw_rect_dsc_t *d, const lv_area_t *a, int y0, int y1, uint32_t col)
{
    if (y0 < a->y1) y0 = a->y1;
    if (y1 > a->y2) y1 = a->y2;
    if (y0 > y1) return;
    d->bg_color = lv_color_hex(col);
    const int top_end = a->y1 + TV_BORDER - 1;
    const int bot_start = a->y2 - TV_BORDER + 1;
    lv_area_t r;
    if (y0 <= top_end) {                                   // top border: full width
        r = { a->x1, (lv_coord_t)y0, a->x2, (lv_coord_t)LV_MIN(y1, top_end) };
        lv_draw_rect(dc, d, &r);
    }
    if (y1 >= bot_start) {                                 // bottom border: full width
        r = { a->x1, (lv_coord_t)LV_MAX(y0, bot_start), a->x2, (lv_coord_t)y1 };
        lv_draw_rect(dc, d, &r);
    }
    int m0 = LV_MAX(y0, top_end + 1), m1 = LV_MIN(y1, bot_start - 1);
    if (m0 <= m1) {                                        // sides only
        r = { a->x1, (lv_coord_t)m0, (lv_coord_t)(a->x1 + TV_BORDER - 1), (lv_coord_t)m1 };
        lv_draw_rect(dc, d, &r);
        r = { (lv_coord_t)(a->x2 - TV_BORDER + 1), (lv_coord_t)m0, a->x2, (lv_coord_t)m1 };
        lv_draw_rect(dc, d, &r);
    }
}

// ---- Live border: the real tape signal, like a real machine's loader ----
//
// The tape ISR records every level change (with its time on the tape
// timeline) into g_mon_ring for the speaker. Each frame we look at a 20 ms
// window of that history (one PAL TV frame, as on a real Spectrum) ending
// slightly in the past so it lines up with what the speaker is playing, and
// map each border row to the signal level at that moment. Read-only, no
// effect on the ISR.

#define LIVE_WINDOW_US 20000    // one TV frame
#define LIVE_DELAY_US  35000    // speaker plays ~40 ms behind; minus display latency
#define SILENCE_US     3000     // no edge for this long = no sound
#define PILOT_MIN_US   540      // Spectrum pilot half-cycle 619 us; data 244/489 us

struct LiveEdge { uint32_t t; uint8_t lvl; uint32_t idx; };

static uint32_t s_silent_col = 0x000000;    // border colour when there's no sound

static uint32_t live_colour(FrameKind kind, const LiveEdge &e, uint32_t dur)
{
    switch (kind) {
        case FK_CPC: return CPC_PAL[(e.idx * 7u) % (sizeof(CPC_PAL) / 4)];   // a colour per pulse
        case FK_C64: return C64_PAL[(e.idx * 11u) % 16];
        default:     // ZX Spectrum ROM loader: red/cyan pilot, blue/yellow data
            if (dur >= PILOT_MIN_US) return e.lvl ? 0x00c8c8 : 0xc80000;
            return e.lvl ? 0xc8c800 : 0x0000c8;
    }
}

static void draw_live(lv_draw_ctx_t *dc, lv_draw_rect_dsc_t *d, const lv_area_t *a, FrameKind kind)
{
    static LiveEdge rev[384];       // newest first
    static LiveEdge asc[386];       // oldest first
    const int rows = a->y2 - a->y1 + 1;
    const uint32_t t1 = (uint32_t)(tape_timer_now_us() - LIVE_DELAY_US);
    const uint32_t t0 = t1 - LIVE_WINDOW_US;

    // Walk back from the newest edge: skip edges after the window, collect the
    // window, and keep the last edge before it (its level is in force at t0).
    uint32_t head = g_mon_head;
    int nr = 0;
    bool have_before = false, have_after = false;
    LiveEdge before = {};
    uint32_t after_t = 0;
    for (uint32_t k = 1; k <= 4096 && k <= head; k++) {
        uint32_t idx = head - k;
        const mon_edge_t &e = g_mon_ring[idx & (MON_RING_SIZE - 1)];
        if ((int32_t)(e.t - t1) > 0) { have_after = true; after_t = e.t; continue; }
        if ((int32_t)(e.t - t0) <= 0 || nr == (int)(sizeof(rev) / sizeof(rev[0]))) {
            before = { e.t, e.level, idx };
            have_before = true;
            break;
        }
        rev[nr++] = { e.t, e.level, idx };
    }
    int m = 0;
    if (have_before) asc[m++] = before;
    for (int i = nr - 1; i >= 0; i--) asc[m++] = rev[i];

    if (m == 0) {   // nothing recorded: silence
        draw_band(dc, d, a, a->y1, a->y2, s_silent_col);
        return;
    }

    int j = 0, run_start = a->y1;
    uint32_t run_col = 0;
    for (int r = 0; r < rows; r++) {
        uint32_t t = t0 + (uint32_t)((uint64_t)LIVE_WINDOW_US * r / rows);
        while (j + 1 < m && (int32_t)(asc[j + 1].t - t) <= 0) j++;
        uint32_t col;
        if ((int32_t)(asc[0].t - t) > 0) {
            col = s_silent_col;                       // before any known edge
        } else {
            uint32_t start = asc[j].t, dur;
            if (j + 1 < m) dur = asc[j + 1].t - start;
            else if (have_after) dur = after_t - start;
            else dur = t - start;                     // still in progress
            col = dur > SILENCE_US ? s_silent_col : live_colour(kind, asc[j], dur);
        }
        if (r == 0) {
            run_col = col;
        } else if (col != run_col) {
            draw_band(dc, d, a, run_start, a->y1 + r - 1, run_col);
            run_start = a->y1 + r;
            run_col = col;
        }
    }
    draw_band(dc, d, a, run_start, a->y2, run_col);
}

static void frame_draw_cb(lv_event_t *e)
{
    if (!s_anim_mode) return;   // static border = the object's own bg colour
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.radius = 0;
    d.border_width = 0;

    if (s_anim_mode == 4) {     // live tape signal
        draw_live(dc, &d, &a, s_anim_kind);
        return;
    }

    uint32_t rng = 0x9E3779B9u ^ (s_anim_frame * 2654435761u);
    uint8_t mode = s_anim_mode;
    FrameKind kind = s_anim_kind;
    if (mode == 3) {
        // idle placeholder: a Spectrum-style load, cycling pilot -> data
        kind = FK_SPECTRUM;
        mode = ((s_anim_frame / 20) % 3 == 2) ? 2 : 1;
    }

    int y = a.y1;
    bool alt = false;
    if (kind == FK_SPECTRUM && mode == 1) {
        // pilot tone: even red/cyan bands rolling slowly
        const int h = 5;
        y -= (int)((s_anim_frame * 3) % (2 * h));
        while (y <= a.y2) {
            draw_band(dc, &d, &a, y, y + h - 1, alt ? 0x00c8c8 : 0xc80000);
            alt = !alt;
            y += h;
        }
    } else if (kind == FK_SPECTRUM) {
        // data: thin blue/yellow bands of random height (0 and 1 bits)
        while (y <= a.y2) {
            int h = 1 + (int)(xorshift(rng) % 3);
            draw_band(dc, &d, &a, y, y + h - 1, alt ? 0xc8c800 : 0x0000c8);
            alt = !alt;
            y += h;
        }
    } else if (kind == FK_CPC) {
        if (mode == 1) {
            const int h = 4;
            y -= (int)((s_anim_frame * 2) % (2 * h));
            while (y <= a.y2) {
                draw_band(dc, &d, &a, y, y + h - 1, alt ? 0xffff00 : 0x0000ff);
                alt = !alt;
                y += h;
            }
        } else {
            while (y <= a.y2) {
                int h = 1 + (int)(xorshift(rng) % 4);
                draw_band(dc, &d, &a, y, y + h - 1, CPC_PAL[xorshift(rng) % (sizeof(CPC_PAL) / 4)]);
                y += h;
            }
        }
    } else {   // FK_C64: raster bars
        int maxh = mode == 1 ? 3 : 6;
        while (y <= a.y2) {
            int h = 2 + (int)(xorshift(rng) % maxh);
            draw_band(dc, &d, &a, y, y + h - 1, C64_PAL[xorshift(rng) % 16]);
            y += h;
        }
    }
}

static void anim_timer_cb(lv_timer_t *t)
{
    tape_status_t st;
    tape_get_status(&st);

    uint8_t mode = 0;
    FrameKind kind = s_play_kind;
    if (st.state == TAPE_PLAYING) {
        if (st.block_id == BLK_C64TAP) kind = FK_C64;
        else if (st.block_id == BLK_ORIC || st.block_id == BLK_JTAP) kind = FK_STATIC;
        if (kind == FK_STATIC && s_placeholder) kind = FK_SPECTRUM;   // placeholder always "loads"
    }
    if (st.state == TAPE_PLAYING && kind != FK_STATIC) {
        mode = 4;                   // live: follow the real signal (silence draws the silent colour)
    } else if (s_placeholder && st.state == TAPE_STOPPED) {
        mode = 3;                   // idle placeholder: gentle fake loading loop
    }

#if UI_ANIM_SELFTEST
    mode = 2; kind = FK_SPECTRUM;
#endif
    bool changed = mode != s_anim_mode || kind != s_anim_kind;
    s_anim_mode = mode;
    s_anim_kind = kind;
    if (mode == 3 && !changed && (++s_anim_frame & 1)) return;   // placeholder at half rate
    if (mode || changed) {
        s_anim_frame++;
        lv_obj_invalidate(tv_frame);
    }
}

// ---------------------------------------------------------------------------
// Preview content

static void set_border_for(System frame_sys)
{
    lv_obj_set_style_bg_color(tv_frame, lv_color_hex(SYS[frame_sys].border), 0);
}

static void show_badge(System sys, System frame_sys)
{
    lv_obj_add_flag(img_cover, LV_OBJ_FLAG_HIDDEN);
    set_border_for(frame_sys);
    s_placeholder = (sys == SYS_TAPE || sys == SYS_NONE);
    if (s_placeholder) {
        lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(placeholder_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(placeholder_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(badge, lv_color_hex(SYS[sys].bg), 0);
    lv_obj_set_style_text_color(badge_lbl, lv_color_hex(SYS[sys].fg), 0);
    lv_label_set_text(badge_lbl, SYS[sys].text);
    for (int i = 0; i < 4; i++) lv_obj_set_style_bg_color(badge_stripes[i], lv_color_hex(SYS[sys].s[i]), 0);
}

static void show_cover_image(lv_img_dsc_t *img, System frame_sys)
{
    lv_img_set_src(img_cover, img);
    lv_obj_clear_flag(img_cover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(placeholder_lbl, LV_OBJ_FLAG_HIDDEN);
    s_placeholder = false;
    set_border_for(frame_sys);
    if (s_cover_img) {
        lv_img_cache_invalidate_src(s_cover_img);
        cover_free(s_cover_img);
    }
    s_cover_img = img;
}

// ---------------------------------------------------------------------------
// Worker task (core 0): directory scans and cover decoding

enum JobType { JOB_SCAN, JOB_COVER, JOB_REMOUNT, JOB_USB_REBOOT, JOB_USB_ON, JOB_USB_OFF };
struct Job {
    JobType type;
    char path[256];      // directory for SCAN, file (or dir) for COVER
    int select;          // COVER: request sequence number
    char name[128];      // SCAN: entry name to select afterwards
};

static QueueHandle_t s_jobs;
static volatile uint32_t s_cover_seq = 0;   // newest cover request wins

static void rebuild_list(void);
static void update_preview(void);

// Read a folder with FatFs directly: f_readdir returns name, size and type in
// one pass. (readdir + stat per file made big folders O(n^2): each stat
// searches the whole directory again.)
static bool read_dir(const char *vfs_path, std::vector<Entry> &v)
{
    const char *drv = sd_fatfs_drive();
    if (!drv || strncmp(vfs_path, SD_MOUNT, strlen(SD_MOUNT)) != 0) return false;
    std::string ff_path = std::string(drv) + (vfs_path[strlen(SD_MOUNT)] ? vfs_path + strlen(SD_MOUNT) : "/");
    static FF_DIR dir;
    static FILINFO fno;
    if (f_opendir(&dir, ff_path.c_str()) != FR_OK) return false;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fname[0] == '.' || (fno.fattrib & (AM_HID | AM_SYS))) continue;
        bool is_dir = (fno.fattrib & AM_DIR) != 0;
        if (!is_dir && !is_tape_ext(ext_of(fno.fname))) continue;
        v.push_back({ fno.fname, is_dir, (uint32_t)fno.fsize });
    }
    f_closedir(&dir);
    return true;
}

static void do_scan(const Job &j)
{
    std::vector<Entry> v;
    int64_t t0 = esp_timer_get_time();
    bool d = read_dir(j.path, v);
    if (!d && sd_is_mounted() && strcmp(j.path, SD_MOUNT) != 0) {
        // remembered folder has gone: fall back to the card root
        Job root = {};
        root.type = JOB_SCAN;
        strcpy(root.path, SD_MOUNT);
        do_scan(root);
        return;
    }
    std::sort(v.begin(), v.end(), [](const Entry &a, const Entry &b) {
        if (a.dir != b.dir) return a.dir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    int64_t t1 = esp_timer_get_time();

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
    ESP_LOGI(TAG, "scan %s: %u entries%s, read %d ms, list %d ms", j.path, (unsigned)s_entries.size(),
             d ? "" : " (open failed)", (int)((t1 - t0) / 1000), (int)((esp_timer_get_time() - t1) / 1000));
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
    System frame_sys = frame_system_for(path, is_dir);
    if (!is_dir) {
        // 1. game.jpg / game.png next to game.tzx
        size_t dot = path.rfind('.');
        std::string stem = dot == std::string::npos ? path : path.substr(0, dot);
        static const char *img_ext[] = { ".jpg", ".png", ".jpeg", NULL };
        for (int i = 0; img_ext[i] && !img; i++) {
            std::string p = stem + img_ext[i];
            if (file_exists(p)) img = cover_decode(p.c_str(), SCREEN_W, SCREEN_H);
        }
        // 1b. Spectrum .tzx / .tap: the loading screen stored in the tape itself
        if (!img && seq == s_cover_seq) img = cover_from_spectrum_tape(path.c_str(), SCREEN_W, SCREEN_H);
        dir = parent_of(path);
    } else {
        dir = path;
    }

    // 2. folder.jpg / folder.png or a system name, walking up to /sdcard
    while (!img && sys == SYS_NONE && dir.size() >= strlen(SD_MOUNT)) {
        if (seq != s_cover_seq) return;
        for (const char *f : { "folder.jpg", "folder.png" }) {
            std::string p = join(dir, f);
            if (file_exists(p)) { img = cover_decode(p.c_str(), SCREEN_W, SCREEN_H); if (img) break; }
        }
        if (img) break;
        size_t slash = dir.rfind('/');
        sys = system_from_name(slash == std::string::npos ? dir : dir.substr(slash + 1));
        if (dir == SD_MOUNT) break;
        dir = parent_of(dir);
    }
    // 3. nothing found: the default "loading screen" placeholder
    if (!img && sys == SYS_NONE) sys = SYS_TAPE;

    bsp_display_lock(0);
    if (seq == s_cover_seq) {
        if (img) show_cover_image(img, frame_sys);
        else show_badge(sys, frame_sys);
        img = NULL;
    }
    bsp_display_unlock();
    if (img) cover_free(img);   // superseded by a newer request
}

// Stop playback (if any) so no file is open on the card
static void stop_tape_and_wait(void)
{
    tape_status_t st;
    tape_get_status(&st);
    if (st.state == TAPE_STOPPED) return;
    tape_stop();
    for (int i = 0; i < 40; i++) {      // up to ~1 s
        vTaskDelay(pdMS_TO_TICKS(25));
        tape_get_status(&st);
        if (st.state == TAPE_STOPPED) break;
    }
}

static void worker_task(void *arg)
{
    Job j;
    for (;;) {
        if (xQueueReceive(s_jobs, &j, portMAX_DELAY) != pdTRUE) continue;
        if (j.type == JOB_SCAN) {
            if (!sd_is_mounted()) sd_mount();
            do_scan(j);
        } else if (j.type == JOB_REMOUNT) {
            // Stop playback so no file is open on the card, then remount
            stop_tape_and_wait();
            sd_remount();
            do_scan(j);    // falls back to the card root if the folder has gone
        } else if (j.type == JOB_USB_REBOOT) {
            stop_tape_and_wait();
            sd_usb_request_boot();
            vTaskDelay(pdMS_TO_TICKS(300));   // let "Restarting..." show
            esp_restart();
        } else if (j.type == JOB_USB_ON) {
            // This boot is for USB mode: hand the card to the PC
            bool ok = sd_usb_start();
            bsp_display_lock(0);
            lv_label_set_text(lbl_usb_state, ok ? "Waiting for the PC..."
                                                : "No SD card found: press Exit, insert a card and try again");
            bsp_display_unlock();
        } else if (j.type == JOB_USB_OFF) {
            sd_usb_exit();     // reboots into normal TouchDuino (card mounted, COM port back)
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
    lv_obj_add_style(b, &st_item, 0);
    lv_obj_add_style(b, &st_item_chk, LV_STATE_CHECKED);
    lv_obj_add_style(b, &st_item_pr, LV_STATE_PRESSED);
    lv_obj_add_style(b, &st_item_dis, LV_STATE_DISABLED);
    if (s_browse_locked) lv_obj_add_state(b, LV_STATE_DISABLED);
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
        lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0x9aa8c0), 0);
        lv_obj_set_user_data(l, (void *)(intptr_t)-100);
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

// Lock or unlock browsing (list, back, refresh, prev/next file) while a tape plays
static void set_browse_locked(bool lock)
{
    if (lock == s_browse_locked) return;
    s_browse_locked = lock;
    uint32_t n = lv_obj_get_child_cnt(list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(list, i);
        if (lock) lv_obj_add_state(c, LV_STATE_DISABLED);
        else lv_obj_clear_state(c, LV_STATE_DISABLED);
    }
    if (lock) lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    else lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    for (lv_obj_t *b : { btn_back, btn_refresh, btn_prev, btn_next }) {
        if (lock) lv_obj_add_state(b, LV_STATE_DISABLED);
        else lv_obj_clear_state(b, LV_STATE_DISABLED);
    }
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

static uint32_t s_playing_size = 0;
static std::string s_preview_path;     // what the preview currently shows

static void set_preview_file(const std::string &name, uint32_t size)
{
    lv_label_set_text(lbl_name, name.c_str());
    const char *fmt = format_name(ext_of(name));
    if (size >= 10240) lv_label_set_text_fmt(lbl_info, "%s  " LV_SYMBOL_BULLET "  %u KB", fmt, (unsigned)(size / 1024));
    else lv_label_set_text_fmt(lbl_info, "%s  " LV_SYMBOL_BULLET "  %u bytes", fmt, (unsigned)size);
}

static void preview_cover(const std::string &path)
{
    if (path == s_preview_path) return;    // already showing it: no flicker
    s_preview_path = path;
    request_cover(path);
}

static void update_preview(void)
{
    // While a tape is playing or paused the preview stays on that tape, so
    // browsing other files doesn't show their details as if they were playing.
    if (s_last_state != TAPE_STOPPED && !s_playing.empty()) {
        set_preview_file(s_playing.substr(s_playing.rfind('/') + 1), s_playing_size);
        preview_cover(s_playing);
        return;
    }
    if (s_sel < 0 || s_sel >= (int)s_entries.size()) {
        lv_label_set_text(lbl_name, s_dir == SD_MOUNT ? "TouchDuino" : s_dir.substr(s_dir.rfind('/') + 1).c_str());
        lv_label_set_text_fmt(lbl_info, "%u items", (unsigned)s_entries.size());
        preview_cover(s_dir);
        return;
    }
    const Entry &en = s_entries[s_sel];
    if (en.dir) {
        lv_label_set_text(lbl_name, en.name.c_str());
        lv_label_set_text(lbl_info, "Folder");
    } else {
        set_preview_file(en.name, en.size);
    }
    preview_cover(join(s_dir, en.name));
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
    s_playing_size = s_entries[s_sel].size;
    System psys = frame_system_for(s_playing, false);
    s_play_kind = SYS[psys].kind;
    s_silent_col = SYS[psys].border;
    tape_play(s_playing.c_str());
}

static void list_item_cb(lv_event_t *e)
{
    if (s_browse_locked) return;     // stop the tape first
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
    if (s_browse_locked || s_entries.empty()) return;
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
// Styles

static void init_styles(void)
{
    lv_style_init(&st_screen);
    lv_style_set_bg_color(&st_screen, lv_color_hex(0x060a14));
    lv_style_set_bg_grad_color(&st_screen, lv_color_hex(0x1a2740));
    lv_style_set_bg_grad_dir(&st_screen, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&st_screen, LV_OPA_COVER);

    // Glass panel: opaque gradient (cheap) + thin light edge
    lv_style_init(&st_glass);
    lv_style_set_bg_color(&st_glass, lv_color_hex(0x223352));
    lv_style_set_bg_grad_color(&st_glass, lv_color_hex(0x0e1628));
    lv_style_set_bg_grad_dir(&st_glass, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&st_glass, LV_OPA_COVER);
    lv_style_set_border_color(&st_glass, lv_color_hex(0xa8c4ff));
    lv_style_set_border_opa(&st_glass, LV_OPA_40);
    lv_style_set_border_width(&st_glass, 1);
    lv_style_set_radius(&st_glass, 12);
    lv_style_set_shadow_width(&st_glass, 0);
    lv_style_set_pad_all(&st_glass, 6);
    lv_style_set_text_color(&st_glass, lv_color_hex(0xe6ecf8));

    // Glass button: gradient set per button, bright rim, no shadow
    lv_style_init(&st_btn);
    lv_style_set_radius(&st_btn, 14);
    lv_style_set_bg_grad_dir(&st_btn, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&st_btn, LV_OPA_COVER);
    lv_style_set_border_color(&st_btn, lv_color_hex(0xffffff));
    lv_style_set_border_opa(&st_btn, LV_OPA_40);
    lv_style_set_border_width(&st_btn, 1);
    lv_style_set_shadow_width(&st_btn, 0);
    lv_style_set_text_color(&st_btn, lv_color_hex(0xffffff));

    // List rows
    lv_style_init(&st_item);
    lv_style_set_bg_opa(&st_item, LV_OPA_TRANSP);
    lv_style_set_text_color(&st_item, lv_color_hex(0xe2e8f4));
    lv_style_set_border_side(&st_item, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&st_item, lv_color_hex(0xffffff));
    lv_style_set_border_opa(&st_item, LV_OPA_10);
    lv_style_set_border_width(&st_item, 1);
    lv_style_set_radius(&st_item, 8);

    lv_style_init(&st_item_chk);
    lv_style_set_bg_opa(&st_item_chk, LV_OPA_COVER);
    lv_style_set_bg_color(&st_item_chk, lv_color_hex(0x3a78e0));
    lv_style_set_bg_grad_color(&st_item_chk, lv_color_hex(0x1c3f86));
    lv_style_set_bg_grad_dir(&st_item_chk, LV_GRAD_DIR_VER);
    lv_style_set_border_side(&st_item_chk, LV_BORDER_SIDE_FULL);
    lv_style_set_border_color(&st_item_chk, lv_color_hex(0x9cc2ff));
    lv_style_set_border_opa(&st_item_chk, LV_OPA_60);
    lv_style_set_text_color(&st_item_chk, lv_color_hex(0xffffff));

    // Locked (tape playing): dim text, and greyed-out buttons
    lv_style_init(&st_item_dis);
    lv_style_set_text_color(&st_item_dis, lv_color_hex(0x5a6475));

    lv_style_init(&st_btn_dis);
    lv_style_set_bg_color(&st_btn_dis, lv_color_hex(0x2a3040));
    lv_style_set_bg_grad_color(&st_btn_dis, lv_color_hex(0x1b202b));
    lv_style_set_text_color(&st_btn_dis, lv_color_hex(0x5a6475));
    lv_style_set_border_opa(&st_btn_dis, LV_OPA_10);

    lv_style_init(&st_item_pr);
    lv_style_set_bg_opa(&st_item_pr, LV_OPA_COVER);
    lv_style_set_bg_color(&st_item_pr, lv_color_hex(0x2c3d5e));

    // Progress bar
    lv_style_init(&st_bar_bg);
    lv_style_set_bg_color(&st_bar_bg, lv_color_hex(0x0a1120));
    lv_style_set_bg_opa(&st_bar_bg, LV_OPA_COVER);
    lv_style_set_border_color(&st_bar_bg, lv_color_hex(0xa8c4ff));
    lv_style_set_border_opa(&st_bar_bg, LV_OPA_30);
    lv_style_set_border_width(&st_bar_bg, 1);
    lv_style_set_radius(&st_bar_bg, 6);

    lv_style_init(&st_bar_ind);
    lv_style_set_bg_color(&st_bar_ind, lv_color_hex(0x46d8ff));
    lv_style_set_bg_grad_color(&st_bar_ind, lv_color_hex(0x2a62e8));
    lv_style_set_bg_grad_dir(&st_bar_ind, LV_GRAD_DIR_HOR);
    lv_style_set_radius(&st_bar_ind, 6);

    // TV bezel around the preview
    lv_style_init(&st_bezel);
    lv_style_set_bg_color(&st_bezel, lv_color_hex(0x2a2f3a));
    lv_style_set_bg_grad_color(&st_bezel, lv_color_hex(0x0b0d12));
    lv_style_set_bg_grad_dir(&st_bezel, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&st_bezel, LV_OPA_COVER);
    lv_style_set_border_color(&st_bezel, lv_color_hex(0xffffff));
    lv_style_set_border_opa(&st_bezel, LV_OPA_30);
    lv_style_set_border_width(&st_bezel, 1);
    lv_style_set_radius(&st_bezel, 10);
    lv_style_set_pad_all(&st_bezel, 0);
    lv_style_set_shadow_width(&st_bezel, 0);
}

static lv_obj_t *glass_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_add_style(p, &st_glass, 0);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static void style_btn(lv_obj_t *b, uint32_t top, uint32_t bottom)
{
    lv_obj_add_style(b, &st_btn, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(bottom), 0);
    lv_obj_add_style(b, &st_btn_dis, LV_STATE_DISABLED);
}

static lv_obj_t *small_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 48, 34);
    style_btn(b, 0x3a4c6e, 0x1a253c);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
    return b;
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

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, intptr_t id, uint32_t top, uint32_t bottom,
                          lv_obj_t **lbl_out = NULL)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 72, 52);
    style_btn(b, top, bottom);
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
        bool stop_changed = (st.state == TAPE_STOPPED) != (s_last_state == TAPE_STOPPED);
        s_last_state = st.state;
        if (stop_changed) {
            set_browse_locked(st.state != TAPE_STOPPED);
            update_preview();   // lock to the playing tape / back to the selection
        }
    }

    int pct = st.size ? (int)((uint64_t)st.pos * 100 / st.size) : 0;
    if (st.state == TAPE_STOPPED) pct = 0;
    if (lv_bar_get_value(bar_prog) != pct) lv_bar_set_value(bar_prog, pct, LV_ANIM_OFF);

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
    if (strcmp(buf, lv_label_get_text(lbl_status)) != 0) lv_label_set_text(lbl_status, buf);

    const char *rem;
    uint32_t col;
    if (s_settings.motor_auto) {
        rem = st.motor_on ? "REM " LV_SYMBOL_PLAY : "REM " LV_SYMBOL_PAUSE;
        col = st.motor_on ? 0x40e070 : 0xe0a040;
    } else {
        rem = "RUN";
        col = 0x8090a0;
    }
    if (strcmp(rem, lv_label_get_text(lbl_rem)) != 0) {
        lv_label_set_text(lbl_rem, rem);
        lv_obj_set_style_text_color(lbl_rem, lv_color_hex(col), 0);
    }
}

#if UI_PERF_LOG
static void perf_monitor_cb(lv_disp_drv_t *drv, uint32_t time_ms, uint32_t px)
{
    // Every ~5 s of activity: frames drawn and time per frame (core 0 load ~= frames * avg / 5000)
    static uint32_t n = 0, sum = 0, mx = 0, t0 = 0;
    uint32_t now = lv_tick_get();
    if (n == 0) t0 = now;
    n++;
    sum += time_ms;
    if (time_ms > mx) mx = time_ms;
    if (now - t0 >= 5000) {
        ESP_LOGI(TAG, "render: %u frames in %u ms, avg %u ms, max %u ms", (unsigned)n, (unsigned)(now - t0),
                 (unsigned)(sum / n), (unsigned)mx);
        n = sum = mx = 0;
    }
}
#endif

// ---------------------------------------------------------------------------
// Settings screen

static const char *baud_map[] = { "1200", "2400", "3150", "3600", "3850", "" };
static const uint16_t baud_vals[] = { 1200, 2400, 3150, 3600, 3850 };

static void settings_from_widgets(void)
{
    uint16_t b = lv_btnmatrix_get_selected_btn(bm_baud);
    if (b < 5) s_settings.baud = baud_vals[b];
    s_settings.motor_auto = lv_obj_has_state(sw_motor, LV_STATE_CHECKED);
    s_settings.rem_invert = lv_obj_has_state(sw_reminv, LV_STATE_CHECKED);
    s_settings.polarity = lv_obj_has_state(sw_pol, LV_STATE_CHECKED);
    s_settings.skip2a = lv_obj_has_state(sw_skip2a, LV_STATE_CHECKED);
    s_settings.short_gaps = lv_obj_has_state(sw_gaps, LV_STATE_CHECKED);
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
    if (s_settings.rem_invert) lv_obj_add_state(sw_reminv, LV_STATE_CHECKED); else lv_obj_clear_state(sw_reminv, LV_STATE_CHECKED);
    if (s_settings.polarity) lv_obj_add_state(sw_pol, LV_STATE_CHECKED); else lv_obj_clear_state(sw_pol, LV_STATE_CHECKED);
    if (s_settings.skip2a) lv_obj_add_state(sw_skip2a, LV_STATE_CHECKED); else lv_obj_clear_state(sw_skip2a, LV_STATE_CHECKED);
    if (s_settings.short_gaps) lv_obj_add_state(sw_gaps, LV_STATE_CHECKED); else lv_obj_clear_state(sw_gaps, LV_STATE_CHECKED);
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
    lv_obj_add_style(row, &st_glass, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    if (hint) {
        lv_obj_t *h = lv_label_create(row);
        lv_label_set_text(h, hint);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(h, lv_color_hex(0x9aa8c0), 0);
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

static void usb_switch_cb(lv_event_t *e)
{
    if (!lv_obj_has_state(sw_usb, LV_STATE_CHECKED)) return;   // leaving is done with Exit
    settings_from_widgets();
    settings_save(&s_settings);
    lv_label_set_text(lbl_usb_hint, "Restarting into USB mode...");
    Job j = {};
    j.type = JOB_USB_REBOOT;
    strncpy(j.path, s_dir.c_str(), sizeof(j.path) - 1);
    xQueueSend(s_jobs, &j, 0);
}

static void build_settings(void)
{
    scr_settings = lv_obj_create(NULL);
    lv_obj_add_style(scr_settings, &st_screen, 0);
    lv_obj_clear_flag(scr_settings, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = glass_panel(scr_settings, 4, 4, 472, 46);
    lv_obj_t *t = lv_label_create(hdr);
    lv_label_set_text(t, LV_SYMBOL_SETTINGS "  Options");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *done = lv_btn_create(hdr);
    lv_obj_set_size(done, 100, 34);
    style_btn(done, 0x3a8ef0, 0x1d4fa8);
    lv_obj_set_style_radius(done, 10, 0);
    lv_obj_align(done, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(done, settings_done_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(done);
    lv_label_set_text(dl, LV_SYMBOL_OK " Done");
    lv_obj_center(dl);

    lv_obj_t *body = lv_obj_create(scr_settings);
    lv_obj_set_size(body, 480, 266);
    lv_obj_set_pos(body, 0, 54);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 6, 0);
    lv_obj_set_style_pad_hor(body, 4, 0);
    lv_obj_set_style_pad_ver(body, 2, 0);

    lv_obj_t *r = settings_row(body, "Baud (TSX / CAS / CDT speed-up)", NULL);
    bm_baud = lv_btnmatrix_create(r);
    lv_btnmatrix_set_map(bm_baud, baud_map);
    lv_btnmatrix_set_btn_ctrl_all(bm_baud, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(bm_baud, true);
    lv_obj_set_size(bm_baud, lv_pct(100), 52);
    lv_obj_align(bm_baud, LV_ALIGN_TOP_LEFT, 0, 26);
    lv_obj_set_style_bg_opa(bm_baud, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bm_baud, 0, 0);
    lv_obj_add_event_cb(bm_baud, setting_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    sw_motor = settings_switch(body, "Motor: Auto REM", "On: REM jack pauses/plays. Off: force run");
    sw_reminv = settings_switch(body, "REM invert", "Off: contact closed = play. On: open = play");
    sw_spk = settings_switch(body, "Onboard speaker", "Hear the tape signal on the speaker");

    r = settings_row(body, "Speaker volume", NULL);
    sl_vol = lv_slider_create(r);
    lv_slider_set_range(sl_vol, 0, 100);
    lv_obj_set_size(sl_vol, 290, 16);
    lv_obj_align(sl_vol, LV_ALIGN_TOP_LEFT, 8, 34);
    lv_obj_add_style(sl_vol, &st_bar_ind, LV_PART_INDICATOR);
    lv_obj_add_event_cb(sl_vol, setting_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lbl_vol = lv_label_create(r);
    lv_obj_align(lbl_vol, LV_ALIGN_TOP_LEFT, 314, 32);
    lv_obj_t *tb = lv_btn_create(r);
    lv_obj_set_size(tb, 80, 40);
    style_btn(tb, 0x3a4c6e, 0x1a253c);
    lv_obj_set_style_radius(tb, 10, 0);
    lv_obj_align(tb, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(tb, [](lv_event_t *e) { monitor_test_beep(700); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tl = lv_label_create(tb);
    lv_label_set_text(tl, LV_SYMBOL_VOLUME_MAX " Test");
    lv_obj_center(tl);
    lv_obj_set_height(r, 70);

    sw_pol = settings_switch(body, "Polarity / TSX accel / UEF parity", "MaxDuino multi-purpose flag");
    sw_gaps = settings_switch(body, "Shorten long gaps", "Gaps over 5 s play as 5 s. Turn off if a game fails");
    sw_skip2a = settings_switch(body, "Skip TZX block 2A", "Ignore 'stop tape if 48K' blocks");

    // USB file transfer: not a saved setting, it switches straight into the mode
    lv_obj_t *ur = settings_row(body, LV_SYMBOL_USB "  USB file transfer", NULL);
    lbl_usb_hint = lv_label_create(ur);
    lv_label_set_text(lbl_usb_hint, "Use the SD card as a USB drive on the PC");
    lv_obj_set_style_text_font(lbl_usb_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_usb_hint, lv_color_hex(0x9aa8c0), 0);
    lv_obj_align(lbl_usb_hint, LV_ALIGN_TOP_LEFT, 0, 22);
    sw_usb = lv_switch_create(ur);
    lv_obj_set_size(sw_usb, 64, 34);
    lv_obj_align(sw_usb, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(sw_usb, usb_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

// ---------------------------------------------------------------------------
// USB file transfer screen

static void usb_exit_cb(lv_event_t *e)
{
    lv_label_set_text(lbl_usb_state, "Restarting TouchDuino...");
    Job j = {};
    j.type = JOB_USB_OFF;
    strncpy(j.path, s_dir.c_str(), sizeof(j.path) - 1);
    xQueueSend(s_jobs, &j, 0);
}

static void usb_timer_cb(lv_timer_t *t)
{
    static int last = -1;
    if (lv_scr_act() != scr_usb || !sd_usb_active()) { last = -1; return; }
    int now = sd_usb_host_connected() ? 1 : 0;
    if (now == last) return;
    last = now;
    lv_label_set_text(lbl_usb_state, now ? LV_SYMBOL_OK "  Connected: the card is a drive on the PC"
                                         : "Waiting for the PC...");
}

static void build_usb(void)
{
    scr_usb = lv_obj_create(NULL);
    lv_obj_add_style(scr_usb, &st_screen, 0);
    lv_obj_clear_flag(scr_usb, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = glass_panel(scr_usb, 4, 4, 472, 46);
    lv_obj_t *t = lv_label_create(hdr);
    lv_label_set_text(t, LV_SYMBOL_USB "  USB file transfer");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *p = glass_panel(scr_usb, 4, 56, 472, 180);
    lbl_usb_state = lv_label_create(p);
    lv_obj_set_style_text_font(lbl_usb_state, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_usb_state, lv_color_hex(0x46d8ff), 0);
    lv_label_set_text(lbl_usb_state, "Waiting for the PC...");
    lv_obj_align(lbl_usb_state, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_t *info = lv_label_create(p);
    lv_obj_set_width(info, 448);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_label_set_text(info,
        "The SD card shows up on the PC as a USB drive: copy, delete and "
        "rename files and folders as usual.\n\n"
        "Tape playback is off in this mode, and the COM port is gone "
        "until you exit.\n\n"
        "Eject the drive on the PC, then press Exit: TouchDuino restarts "
        "and reads the card again.");
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 8, 40);

    lv_obj_t *ex = lv_btn_create(scr_usb);
    lv_obj_set_size(ex, 220, 64);
    style_btn(ex, 0x3a8ef0, 0x1d4fa8);
    lv_obj_set_style_radius(ex, 14, 0);
    lv_obj_align(ex, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(ex, usb_exit_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *el = lv_label_create(ex);
    lv_label_set_text(el, LV_SYMBOL_CLOSE "  Exit");
    lv_obj_set_style_text_font(el, &lv_font_montserrat_20, 0);
    lv_obj_center(el);

    lv_timer_create(usb_timer_cb, 300, NULL);
}

// ---------------------------------------------------------------------------
// Main screen

static void back_cb(lv_event_t *e) { if (!s_browse_locked) go_parent(); }
static void refresh_cb(lv_event_t *e)
{
    if (s_browse_locked) return;
    // Remount the card (handles a card swap), then rescan the current folder
    lv_label_set_text(lbl_path, "Reading SD card...");
    Job j = {};
    j.type = JOB_REMOUNT;
    strncpy(j.path, s_dir.c_str(), sizeof(j.path) - 1);
    xQueueSend(s_jobs, &j, 0);
}

static void build_main(void)
{
    scr_main = lv_obj_create(NULL);
    lv_obj_add_style(scr_main, &st_screen, 0);
    lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);

    // Header
    lv_obj_t *hdr = glass_panel(scr_main, 4, 4, 472, 42);
    lv_obj_set_style_pad_all(hdr, 3, 0);
    lv_obj_t *back = btn_back = small_btn(hdr, LV_SYMBOL_LEFT, back_cb);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);

    lbl_path = lv_label_create(hdr);
    lv_obj_set_width(lbl_path, 246);      // ends before the REM tag
    // Scrolls when too long; with the shadow frame buffer only this label redraws
    lv_label_set_long_mode(lbl_path, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_speed(lbl_path, 18, 0);
    lv_obj_set_style_text_font(lbl_path, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_path, LV_ALIGN_LEFT_MID, 56, 0);
    lv_label_set_text(lbl_path, "");
    // Hidden: long-press the path to save a screenshot to /sdcard/screenshot.bmp
    lv_obj_add_flag(lbl_path, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lbl_path, [](lv_event_t *e) {
        bool ok = lvgl_port_save_screenshot(SD_MOUNT "/screenshot.bmp");
        lv_label_set_text(lbl_path, ok ? LV_SYMBOL_OK " Saved screenshot.bmp" : LV_SYMBOL_WARNING " Screenshot failed");
    }, LV_EVENT_LONG_PRESSED, NULL);

    lbl_rem = lv_label_create(hdr);
    lv_obj_set_style_text_font(lbl_rem, &lv_font_montserrat_12, 0);
    lv_obj_set_width(lbl_rem, 52);
    lv_obj_set_style_text_align(lbl_rem, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_rem, LV_ALIGN_RIGHT_MID, -104, 0);
    lv_label_set_text(lbl_rem, "");

    lv_obj_t *refresh = btn_refresh = small_btn(hdr, LV_SYMBOL_REFRESH, refresh_cb);
    lv_obj_align(refresh, LV_ALIGN_RIGHT_MID, -52, 0);
    lv_obj_t *gear = small_btn(hdr, LV_SYMBOL_SETTINGS, open_settings_cb);
    lv_obj_align(gear, LV_ALIGN_RIGHT_MID, 0, 0);

    // File list (the list itself is a glass panel)
    list = lv_list_create(scr_main);
    lv_obj_add_style(list, &st_glass, 0);
    lv_obj_set_pos(list, 4, 50);
    lv_obj_set_size(list, 286, 184);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_style_pad_row(list, 2, 0);

    // Preview pane
    lv_obj_t *pv = glass_panel(scr_main, 294, 50, 182, 184);

    lv_obj_t *bezel = lv_obj_create(pv);
    lv_obj_add_style(bezel, &st_bezel, 0);
    lv_obj_set_size(bezel, TV_FRAME_W + 10, TV_FRAME_H + 10);
    lv_obj_align(bezel, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(bezel, LV_OBJ_FLAG_SCROLLABLE);

    tv_frame = lv_obj_create(bezel);
    lv_obj_remove_style_all(tv_frame);
    lv_obj_set_size(tv_frame, TV_FRAME_W, TV_FRAME_H);
    lv_obj_center(tv_frame);
    lv_obj_set_style_bg_opa(tv_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tv_frame, lv_color_hex(SYS[SYS_TAPE].border), 0);
    lv_obj_clear_flag(tv_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tv_frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tv_frame, frame_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);

    tv_screen = lv_obj_create(tv_frame);
    lv_obj_remove_style_all(tv_screen);
    lv_obj_set_size(tv_screen, SCREEN_W, SCREEN_H);
    lv_obj_center(tv_screen);
    lv_obj_set_style_bg_opa(tv_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tv_screen, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(tv_screen, LV_OBJ_FLAG_SCROLLABLE);

    img_cover = lv_img_create(tv_screen);
    lv_obj_center(img_cover);
    lv_obj_add_flag(img_cover, LV_OBJ_FLAG_HIDDEN);

    placeholder_lbl = lv_label_create(tv_screen);
    lv_label_set_text(placeholder_lbl, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(placeholder_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(placeholder_lbl, lv_color_hex(0x2a3344), 0);
    lv_obj_center(placeholder_lbl);

    badge = lv_obj_create(tv_screen);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, SCREEN_W, SCREEN_H);
    lv_obj_center(badge);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 4; i++) {
        badge_stripes[i] = lv_obj_create(badge);
        lv_obj_remove_style_all(badge_stripes[i]);
        lv_obj_set_style_bg_opa(badge_stripes[i], LV_OPA_COVER, 0);
        lv_obj_set_size(badge_stripes[i], 7, 56);
        lv_obj_align(badge_stripes[i], LV_ALIGN_RIGHT_MID, -8 - (3 - i) * 9, 0);
    }
    badge_lbl = lv_label_create(badge);
    lv_obj_set_style_text_font(badge_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(badge_lbl, LV_ALIGN_LEFT_MID, 8, 0);
    show_badge(SYS_TAPE, SYS_TAPE);

    lbl_name = lv_label_create(pv);
    lv_obj_set_width(lbl_name, 168);
    lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_speed(lbl_name, 18, 0);
    lv_obj_set_height(lbl_name, 20);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_name, LV_ALIGN_TOP_LEFT, 0, TV_FRAME_H + 16);
    lv_label_set_text(lbl_name, "TouchDuino");

    lbl_info = lv_label_create(pv);
    lv_obj_set_width(lbl_info, 168);
    lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_DOT);
    lv_obj_set_height(lbl_info, 16);
    lv_obj_set_style_text_font(lbl_info, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_info, lv_color_hex(0x9aa8c0), 0);
    lv_obj_align(lbl_info, LV_ALIGN_TOP_LEFT, 0, TV_FRAME_H + 38);
    lv_label_set_text(lbl_info, "");

    // Progress row
    bar_prog = lv_bar_create(scr_main);
    lv_obj_add_style(bar_prog, &st_bar_bg, LV_PART_MAIN);
    lv_obj_add_style(bar_prog, &st_bar_ind, LV_PART_INDICATOR);
    lv_obj_set_size(bar_prog, 168, 12);
    lv_obj_set_pos(bar_prog, 10, 242);
    lv_bar_set_range(bar_prog, 0, 100);

    lbl_status = lv_label_create(scr_main);
    lv_obj_set_width(lbl_status, 284);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xd6e2f6), 0);
    lv_obj_set_pos(lbl_status, 188, 239);
    lv_label_set_text(lbl_status, "Ready");

    // Transport
    lv_obj_t *row = lv_obj_create(scr_main);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 480, 62);
    lv_obj_set_pos(row, 0, 258);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const uint32_t g_top = 0x46597e, g_bot = 0x1a2438;
    btn_prev = make_btn(row, LV_SYMBOL_PREV, B_PREV, g_top, g_bot);
    make_btn(row, LV_SYMBOL_LEFT LV_SYMBOL_LEFT, B_BLK_PREV, g_top, g_bot);
    make_btn(row, LV_SYMBOL_PLAY, B_PLAY, 0x3fd27f, 0x14693a, &lbl_play);
    make_btn(row, LV_SYMBOL_STOP, B_STOP, 0xf0605a, 0x8a1f1f);
    make_btn(row, LV_SYMBOL_RIGHT LV_SYMBOL_RIGHT, B_BLK_NEXT, g_top, g_bot);
    btn_next = make_btn(row, LV_SYMBOL_NEXT, B_NEXT, g_top, g_bot);
}

extern "C" void ui_init(const tape_settings_t *settings, const char *start_dir, bool usb_mode)
{
    s_settings = *settings;
    lv_disp_t *disp = lv_disp_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, lv_color_hex(0x3a78e0), lv_color_hex(0x46d8ff),
                                           true, &lv_font_montserrat_14);
    lv_disp_set_theme(disp, th);
#if UI_PERF_LOG
    disp->driver->monitor_cb = perf_monitor_cb;
#endif

    init_styles();
    build_main();
    build_settings();
    build_usb();
    lv_scr_load(scr_main);

    s_jobs = xQueueCreate(8, sizeof(Job));
    xTaskCreatePinnedToCore(worker_task, "ui_worker", 8192, NULL, 3, NULL, 0);

    lv_timer_create(status_timer_cb, 100, NULL);
    lv_timer_create(anim_timer_cb, ANIM_PERIOD_MS, NULL);

    std::string dir = start_dir && strncmp(start_dir, SD_MOUNT, strlen(SD_MOUNT)) == 0 ? start_dir : SD_MOUNT;
    if (usb_mode) {
        s_dir = dir;
        lv_label_set_text(lbl_usb_state, "Starting USB drive...");
        lv_scr_load(scr_usb);
        Job j = {};
        j.type = JOB_USB_ON;
        xQueueSend(s_jobs, &j, 0);
        return;
    }
    request_scan(dir);
}
