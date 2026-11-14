#pragma once
#include <stdbool.h>
#include "engine/tape_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_MOUNT "/sdcard"

bool sd_mount(void);          // safe to call repeatedly; returns true when mounted
bool sd_is_mounted(void);

void settings_load(tape_settings_t *s, char *last_dir, int last_dir_len);
void settings_save(const tape_settings_t *s);
void settings_save_dir(const char *dir);

#ifdef __cplusplus
}
#endif
