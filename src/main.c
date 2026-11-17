#include <inttypes.h>
#include <esp_log.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include "esp_bsp.h"
#include "display.h"
#include "lv_port.h"
#include "app/storage.h"
#include "engine/tape_engine.h"
#include "ui/ui.h"

static const char *TAG = "main";

// ---- Crash log that survives a reboot ----------------------------------------
// In USB file transfer mode the COM port is gone, so a crash message is lost.
// Every log line is also copied into RTC memory (kept across a software reset)
// and the tail is printed at the next boot along with the reset reason.
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_system.h"
#define CRASHLOG_SIZE 3072
#define CRASHLOG_MAGIC 0x7D0C1A55u
RTC_NOINIT_ATTR static uint32_t s_clog_magic;
RTC_NOINIT_ATTR static uint32_t s_clog_pos;
RTC_NOINIT_ATTR static char s_clog[CRASHLOG_SIZE];
static vprintf_like_t s_prev_vprintf;

// ---- Panic backtrace into the crash log ----------------------------------------
// esp_panic_handler is wrapped (-Wl,--wrap=esp_panic_handler in platformio.ini):
// the panicking core's reason and backtrace go into the RTC log first, so they
// survive the reboot even when the COM port is gone (USB file transfer mode).
#include "esp_private/panic_internal.h"
#include "esp_debug_helpers.h"
#include "esp_cpu.h"
#include "xtensa/xtensa_context.h"

static void IRAM_ATTR clog_putc(char c)
{
    s_clog[s_clog_pos % CRASHLOG_SIZE] = c;
    s_clog_pos++;
}
static void IRAM_ATTR clog_puts(const char *s)
{
    while (s && *s) clog_putc(*s++);
}
static void IRAM_ATTR clog_hex(uint32_t v)
{
    clog_puts(" 0x");
    for (int i = 28; i >= 0; i -= 4) clog_putc("0123456789abcdef"[(v >> i) & 15]);
}

void __real_esp_panic_handler(panic_info_t *info);
void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    clog_puts("\nPANIC on core ");
    clog_putc('0' + info->core);
    clog_puts(": ");
    clog_puts(info->reason);
    clog_puts(" / ");
    clog_puts(info->description);
    clog_puts("\nBacktrace:");
    const XtExcFrame *f = (const XtExcFrame *)info->frame;
    if (f) {
        esp_backtrace_frame_t fr = { .pc = f->pc, .sp = f->a1, .next_pc = f->a0 };
        clog_hex(esp_cpu_get_call_addr(fr.pc));
        for (int i = 0; i < 20 && fr.next_pc; i++) {
            if (!esp_backtrace_get_next_frame(&fr)) break;
            clog_hex(esp_cpu_get_call_addr(fr.pc));
        }
    }
    clog_puts("\n");
    __real_esp_panic_handler(info);
}

static int crashlog_vprintf(const char *fmt, va_list ap)
{
    char line[160];
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(line, sizeof(line), fmt, cp);
    va_end(cp);
    if (n > (int)sizeof(line) - 1) n = sizeof(line) - 1;
    for (int i = 0; i < n; i++) {
        s_clog[s_clog_pos % CRASHLOG_SIZE] = line[i];
        s_clog_pos++;
    }
    return s_prev_vprintf(fmt, ap);
}

static char *s_clog_saved;   // copy of the previous boot's log tail
static int s_clog_reason;

// A few seconds after boot (the COM port takes a moment to come back after a
// reset): a log that ended in a crash or reached USB mode is saved to /sdcard/crashlog.txt, and
// that file is printed line by line (the USB-Serial-JTAG console drops bursts).
static void crashlog_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(6000));
    bool crash = s_clog_reason == ESP_RST_PANIC || s_clog_reason == ESP_RST_INT_WDT ||
                 s_clog_reason == ESP_RST_TASK_WDT || s_clog_reason == ESP_RST_WDT;
    if (s_clog_saved && (crash || strstr(s_clog_saved, "USB mode"))) {
        FILE *f = fopen(SD_MOUNT "/crashlog.txt", "w");
        if (f) {
            fprintf(f, "reset reason %d\n%s\n", s_clog_reason, s_clog_saved);
            fclose(f);
        }
    }
    FILE *f = fopen(SD_MOUNT "/crashlog.txt", "r");
    if (f) {
        char line[200];
        printf("\n---- crashlog.txt ----\n");
        while (fgets(line, sizeof(line), f)) {
            for (char *c = line; *c; c++) if ((unsigned char)*c >= 0x80) *c = '#';   // plain ASCII only
            printf("%s", line);
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        printf("\n---- end ----\n");
        fclose(f);
    }
    vTaskDelete(NULL);
}

static void crashlog_init(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    ESP_LOGI(TAG, "reset reason %d", (int)r);
    if (s_clog_magic == CRASHLOG_MAGIC && r != ESP_RST_POWERON && s_clog_pos > 0) {
        uint32_t len = s_clog_pos < CRASHLOG_SIZE ? s_clog_pos : CRASHLOG_SIZE;
        s_clog_saved = (char *)malloc(len + 1);
        if (s_clog_saved) {
            uint32_t k = 0;
            for (uint32_t i = s_clog_pos - len; i < s_clog_pos; i++) s_clog_saved[k++] = s_clog[i % CRASHLOG_SIZE];
            s_clog_saved[k] = 0;
            s_clog_reason = (int)r;
        }
    }
    xTaskCreate(crashlog_task, "crashlog", 4096, NULL, 1, NULL);
    s_clog_magic = CRASHLOG_MAGIC;
    s_clog_pos = 0;
    s_prev_vprintf = esp_log_set_vprintf(crashlog_vprintf);
}

#define APP_DISPLAY_ROTATION    LV_DISP_ROT_90   // landscape 480x320

void app_main(void)
{
    crashlog_init();
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;
    esp_chip_info(&chip_info);
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "TouchDuino on %s rev v%d.%d, %d cores, %" PRIu32 " MB flash, %u KB PSRAM free",
             CONFIG_IDF_TARGET, chip_info.revision / 100, chip_info.revision % 100, chip_info.cores,
             flash_size / (1024 * 1024), heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

    // USB file transfer mode is entered with a reboot (one-shot flag). On a
    // normal boot, make sure the USB port belongs to the COM port again.
    bool usb_boot = sd_usb_boot_pending();
    if (!usb_boot) sd_usb_boot_init();

    tape_settings_t settings;
    char last_dir[256];
    settings_load(&settings, last_dir, sizeof(last_dir));

    // Tape engine + pulse ISR on core 1, speaker task on core 0
    tape_engine_start(&settings);

    if (!usb_boot) sd_mount();

    // LVGL rendering pinned to core 0, away from the tape ISR
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = EXAMPLE_LCD_QSPI_H_RES * EXAMPLE_LCD_QSPI_V_RES,
        .rotate = APP_DISPLAY_ROTATION,
    };
    cfg.lvgl_port_cfg.task_affinity = 0;
    cfg.lvgl_port_cfg.task_stack = 8192;
    if (bsp_display_start_with_config(&cfg) == NULL) {
        ESP_LOGE(TAG, "Display init failed");
        return;
    }
    bsp_display_backlight_on();

    bsp_display_lock(0);
    ui_init(&settings, last_dir, usb_boot);
    bsp_display_unlock();
}
