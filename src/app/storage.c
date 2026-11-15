#include <string.h>
#include "storage.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "storage";

#define SD_PIN_CS   10
#define SD_PIN_CMD  11
#define SD_PIN_CLK  12
#define SD_PIN_D0   13

static sdmmc_card_t *s_card = NULL;

bool sd_is_mounted(void) { return s_card != NULL; }

// There is no card-detect line on this board, so after a card swap the old
// mount is stale (the new card is uninitialised). Unmount and start again.
bool sd_remount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT, s_card);
        s_card = NULL;
        ESP_LOGI(TAG, "SD unmounted");
    }
    return sd_mount();
}

static const esp_vfs_fat_mount_config_t s_mount_cfg = {
    .format_if_mount_failed = false,
    .max_files = 8,
    .allocation_unit_size = 16 * 1024,
};

// SDMMC 1-bit mode on CLK 12, CMD 11, D0 13 at 20 MHz, exactly as the vendor
// demos do. DAT3/CS is held high so the card powers up in SD (not SPI) mode.
static bool sd_mount_sdmmc(void)
{
    gpio_set_direction(SD_PIN_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(SD_PIN_CS, 1);
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;   // 20 MHz
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_PIN_CLK;
    slot.cmd = SD_PIN_CMD;
    slot.d0 = SD_PIN_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &s_mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount (SDMMC 1-bit) failed: %s", esp_err_to_name(err));
        s_card = NULL;
        return false;
    }
    ESP_LOGI(TAG, "SD mounted in SDMMC 1-bit mode");
    sdmmc_card_print_info(stdout, s_card);
    return true;
}


// SDMMC only. SPI mode doesn't work on this board, and an SPI attempt can
// latch a freshly inserted card into SPI mode until it is power-cycled.
bool sd_mount(void)
{
    if (s_card) return true;
    return sd_mount_sdmmc();
}

static nvs_handle_t open_nvs(void)
{
    static bool inited = false;
    if (!inited) {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_flash_init();
        }
        inited = true;
    }
    nvs_handle_t h = 0;
    if (nvs_open("touchduino", NVS_READWRITE, &h) != ESP_OK) return 0;
    return h;
}

void settings_load(tape_settings_t *s, char *last_dir, int last_dir_len)
{
    s->baud = 3850;
    s->motor_auto = false;
    s->polarity = false;
    s->skip2a = false;
    s->speaker = true;
    s->volume = 40;
    if (last_dir) strncpy(last_dir, SD_MOUNT, last_dir_len);

    nvs_handle_t h = open_nvs();
    if (!h) return;
    uint16_t u16;
    uint8_t u8;
    if (nvs_get_u16(h, "baud", &u16) == ESP_OK) s->baud = u16;
    if (nvs_get_u8(h, "motor", &u8) == ESP_OK) s->motor_auto = u8;
    if (nvs_get_u8(h, "pol", &u8) == ESP_OK) s->polarity = u8;
    if (nvs_get_u8(h, "skip2a", &u8) == ESP_OK) s->skip2a = u8;
    if (nvs_get_u8(h, "spk", &u8) == ESP_OK) s->speaker = u8;
    if (nvs_get_u8(h, "vol", &u8) == ESP_OK) s->volume = u8;
    if (last_dir) {
        size_t len = last_dir_len;
        if (nvs_get_str(h, "dir", last_dir, &len) != ESP_OK) strncpy(last_dir, SD_MOUNT, last_dir_len);
    }
    nvs_close(h);
}

void settings_save(const tape_settings_t *s)
{
    nvs_handle_t h = open_nvs();
    if (!h) return;
    nvs_set_u16(h, "baud", s->baud);
    nvs_set_u8(h, "motor", s->motor_auto);
    nvs_set_u8(h, "pol", s->polarity);
    nvs_set_u8(h, "skip2a", s->skip2a);
    nvs_set_u8(h, "spk", s->speaker);
    nvs_set_u8(h, "vol", (uint8_t)s->volume);
    nvs_commit(h);
    nvs_close(h);
}

void settings_save_dir(const char *dir)
{
    nvs_handle_t h = open_nvs();
    if (!h) return;
    nvs_set_str(h, "dir", dir);
    nvs_commit(h);
    nvs_close(h);
}
