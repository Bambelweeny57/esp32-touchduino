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

#define APP_DISPLAY_ROTATION    LV_DISP_ROT_90   // landscape 480x320

void app_main(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;
    esp_chip_info(&chip_info);
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "TouchDuino on %s rev v%d.%d, %d cores, %" PRIu32 " MB flash, %u KB PSRAM free",
             CONFIG_IDF_TARGET, chip_info.revision / 100, chip_info.revision % 100, chip_info.cores,
             flash_size / (1024 * 1024), heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

    tape_settings_t settings;
    char last_dir[256];
    settings_load(&settings, last_dir, sizeof(last_dir));

    // Tape engine + pulse ISR on core 1, speaker task on core 0
    tape_engine_start(&settings);

    sd_mount();

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
    ui_init(&settings, last_dir);
    bsp_display_unlock();
}
