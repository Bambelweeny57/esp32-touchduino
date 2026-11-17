#include <string.h>
#include <stdio.h>
#include "storage.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "tusb_tasks.h"
#include "esp_private/usb_phy.h"
#include "soc/rtc_cntl_reg.h"

static const char *TAG = "storage";

#define SD_PIN_CS   10
#define SD_PIN_CMD  11
#define SD_PIN_CLK  12
#define SD_PIN_D0   13

static sdmmc_card_t *s_card = NULL;

bool sd_is_mounted(void) { return s_card != NULL; }

const char *sd_fatfs_drive(void)
{
    static char drv[8];
    if (!s_card) return NULL;
    snprintf(drv, sizeof(drv), "%d:", (int)ff_diskio_get_pdrv_card(s_card));
    return drv;
}

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
static void sd_host_slot(sdmmc_host_t *host, sdmmc_slot_config_t *slot)
{
    gpio_set_direction(SD_PIN_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(SD_PIN_CS, 1);
    *host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    host->max_freq_khz = SDMMC_FREQ_DEFAULT;   // 20 MHz
    *slot = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    slot->width = 1;
    slot->clk = SD_PIN_CLK;
    slot->cmd = SD_PIN_CMD;
    slot->d0 = SD_PIN_D0;
    slot->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
}

static bool sd_mount_sdmmc(void)
{
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    sd_host_slot(&host, &slot);
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

// ---- USB file transfer mode --------------------------------------------------
// The card is handed to the PC as a USB drive (TinyUSB mass storage on the
// native USB port, shared with the USB-Serial-JTAG that gives the COM port).
// Switching the port at runtime fought with the USB-Serial-JTAG (chip resets,
// no pull-up, watchdog), so the mode is entered and left with a reboot:
//   switch on  -> sd_usb_request_boot() + restart -> boot straight into USB mode
//   Exit       -> sd_usb_exit(): restart -> normal boot reclaims the COM port
// The boot flag is one-shot (cleared when read), so a crash in USB mode always
// ends in a normal boot.
static sdmmc_card_t *s_usb_card = NULL;
static bool s_usb_active = false;

bool sd_usb_active(void) { return s_usb_active; }
bool sd_usb_host_connected(void) { return s_usb_active && tud_mounted(); }

static nvs_handle_t open_nvs(void);

void sd_usb_request_boot(void)
{
    nvs_handle_t h = open_nvs();
    if (!h) return;
    nvs_set_u8(h, "usbboot", 1);
    nvs_commit(h);
    nvs_close(h);
}

bool sd_usb_boot_pending(void)
{
    nvs_handle_t h = open_nvs();
    if (!h) return false;
    uint8_t v = 0;
    bool pending = nvs_get_u8(h, "usbboot", &v) == ESP_OK && v;
    if (pending) {
        nvs_erase_key(h, "usbboot");
        nvs_commit(h);
    }
    nvs_close(h);
    return pending;
}

void sd_usb_boot_init(void)
{
    // Normal boot: after USB mode (a software reset keeps the RTC USB routing)
    // the port may still be routed to the OTG controller. Route it back to
    // the USB-Serial-JTAG and re-enable its reset (esptool's auto-reset).
    static usb_phy_handle_t jtag_phy = NULL;
    if (!jtag_phy && (REG_READ(RTC_CNTL_USB_CONF_REG) & RTC_CNTL_SW_USB_PHY_SEL)) {
        const usb_phy_config_t jtag = {
            .controller = USB_PHY_CTRL_SERIAL_JTAG,
            .target = USB_PHY_TARGET_INT,
        };
        if (usb_new_phy(&jtag, &jtag_phy) != ESP_OK) jtag_phy = NULL;
        ESP_LOGI(TAG, "USB port handed back to the COM port");
    }
    REG_CLR_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_IO_MUX_RESET_DISABLE | RTC_CNTL_USB_RESET_DISABLE);
}

bool sd_usb_start(void)
{
    if (s_usb_active) return true;
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT, s_card);
        s_card = NULL;
    }
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    sd_host_slot(&host, &slot);
    if (host.init() != ESP_OK) return false;
    if (sdmmc_host_init_slot(host.slot, &slot) != ESP_OK) { host.deinit(); return false; }
    s_usb_card = (sdmmc_card_t *)calloc(1, sizeof(sdmmc_card_t));
    if (!s_usb_card || sdmmc_card_init(&host, s_usb_card) != ESP_OK) {
        ESP_LOGW(TAG, "USB mode: no SD card");
        free(s_usb_card);
        s_usb_card = NULL;
        host.deinit();
        return false;
    }
    const tinyusb_msc_sdmmc_config_t msc = {
        .card = s_usb_card,
        .mount_config = { .format_if_mount_failed = false, .max_files = 8, .allocation_unit_size = 16 * 1024 },
    };
    ESP_LOGI(TAG, "USB mode: card ready, %lu sectors", (unsigned long)s_usb_card->csd.capacity);
    ESP_ERROR_CHECK(tinyusb_msc_storage_init_sdmmc(&msc));
    // Taking the port from the USB-Serial-JTAG looks like a "reset me" line
    // change to it, and it resets the whole chip (ESP_RST_USB): block that.
    REG_SET_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_IO_MUX_RESET_DISABLE | RTC_CNTL_USB_RESET_DISABLE);
    const tinyusb_config_t tusb = { 0 };      // default descriptors (Kconfig strings)
    if (tinyusb_driver_install(&tusb) != ESP_OK) {
        ESP_LOGE(TAG, "USB mode: TinyUSB install failed");
        return false;
    }
    s_usb_active = true;
    ESP_LOGI(TAG, "USB mode: TinyUSB running");
    return true;
}

void sd_usb_exit(void)
{
    if (s_usb_active) {
        tud_disconnect();                    // the PC sees the drive removed
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    esp_restart();                           // normal boot: COM port, card remounted
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
    s->rem_invert = false;  // contact closed = play
    s->polarity = false;
    s->skip2a = true;      // ignore TZX ID2A "stop if 48K": wrong for 128K games
    s->short_gaps = true;  // gaps over 5 s play as 5 s (TZX transfers often contain long blank tape)
    s->speaker = true;
    s->volume = 40;
    if (last_dir) strncpy(last_dir, SD_MOUNT, last_dir_len);

    nvs_handle_t h = open_nvs();
    if (!h) return;
    uint16_t u16;
    uint8_t u8;
    if (nvs_get_u16(h, "baud", &u16) == ESP_OK) s->baud = u16;
    if (nvs_get_u8(h, "motor", &u8) == ESP_OK) s->motor_auto = u8;
    if (nvs_get_u8(h, "reminv", &u8) == ESP_OK) s->rem_invert = u8;
    if (nvs_get_u8(h, "pol", &u8) == ESP_OK) s->polarity = u8;
    if (nvs_get_u8(h, "skip2a", &u8) == ESP_OK) s->skip2a = u8;
    if (nvs_get_u8(h, "gaps", &u8) == ESP_OK) s->short_gaps = u8;
    // Settings version 2: "Skip TZX block 2A" became the default. Turn it on once
    // for settings saved by older firmware (the user can still switch it off).
    uint8_t ver = 0;
    nvs_get_u8(h, "cfgver", &ver);
    if (ver < 2) {
        s->skip2a = true;
        nvs_set_u8(h, "skip2a", 1);
        nvs_set_u8(h, "cfgver", 2);
        nvs_commit(h);
    }
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
    nvs_set_u8(h, "reminv", s->rem_invert);
    nvs_set_u8(h, "pol", s->polarity);
    nvs_set_u8(h, "skip2a", s->skip2a);
    nvs_set_u8(h, "gaps", s->short_gaps);
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
