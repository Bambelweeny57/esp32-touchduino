#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

// Decodes a JPG or PNG file into an LVGL true-colour image scaled to fit
// max_w x max_h (aspect preserved). Pixel data is allocated in PSRAM.
// Returns NULL on failure. Free with cover_free().
lv_img_dsc_t *cover_decode(const char *path, int max_w, int max_h);

// ZX Spectrum: finds the loading screen (SCREEN$, 6912 bytes) inside a .tzx or
// .tap file and renders it, scaled to fit. Returns NULL if there isn't one.
lv_img_dsc_t *cover_from_spectrum_tape(const char *path, int max_w, int max_h);

void cover_free(lv_img_dsc_t *img);
