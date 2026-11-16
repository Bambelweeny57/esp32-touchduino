#pragma once
#include <stdbool.h>
#include "engine/tape_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_MOUNT "/sdcard"

bool sd_mount(void);          // safe to call repeatedly; returns true when mounted
bool sd_is_mounted(void);
bool sd_remount(void);        // unmount (if mounted) and mount again, e.g. after a card swap
const char *sd_fatfs_drive(void);  // FatFs drive prefix for the card, e.g. "0:" (NULL if not mounted)
// USB file transfer mode: the card appears on the PC as a USB drive.
// Entered and left with a reboot (see storage.c).
void sd_usb_request_boot(void);    // next boot goes straight into USB mode
bool sd_usb_boot_pending(void);    // at boot: true once (the flag is cleared)
void sd_usb_boot_init(void);       // normal boot: USB port back to the COM port
bool sd_usb_start(void);           // USB-mode boot: hand the card to the PC
void sd_usb_exit(void);            // reboot into normal TouchDuino
bool sd_usb_active(void);
bool sd_usb_host_connected(void);   // the PC has enumerated the drive

void settings_load(tape_settings_t *s, char *last_dir, int last_dir_len);
void settings_save(const tape_settings_t *s);
void settings_save_dir(const char *dir);

#ifdef __cplusplus
}
#endif
