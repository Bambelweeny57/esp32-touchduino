#pragma once

#include "lvgl.h"
#include "engine/tape_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Build the TouchDuino UI. Called once from app_main() with the LVGL lock held.
 *  usb_mode: this boot is for USB file transfer (show that screen, hand the card to the PC). */
void ui_init(const tape_settings_t *settings, const char *start_dir, bool usb_mode);

#ifdef __cplusplus
}
#endif
