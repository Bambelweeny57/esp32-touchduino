#pragma once

#include "lvgl.h"
#include "engine/tape_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Build the TouchDuino UI. Called once from app_main() with the LVGL lock held. */
void ui_init(const tape_settings_t *settings, const char *start_dir);

#ifdef __cplusplus
}
#endif
