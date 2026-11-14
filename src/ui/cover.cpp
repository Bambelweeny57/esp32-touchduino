// Cover art decoding: JPG via LVGL's bundled TJpgDec, PNG via LVGL's lodepng.
// Output is scaled (nearest neighbour, aspect preserved) to fit the preview
// box and stored as an LVGL true-colour image in PSRAM.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cover.h"

#include "src/extra/libs/sjpg/tjpgd.h"   // has its own extern "C"
// lodepng.h exposes a C++ API when included from C++, but LVGL compiles
// lodepng.c as C, so declare the C entry point directly.
extern "C" unsigned lodepng_decode32(unsigned char **out, unsigned *w, unsigned *h,
                                     const unsigned char *in, size_t insize);

static const char *TAG = "cover";

#define MAX_FILE_BYTES (3 * 1024 * 1024)

static void *ps_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > MAX_FILE_BYTES) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)ps_alloc(sz);
    if (buf && fread(buf, 1, sz, f) != (size_t)sz) { free(buf); buf = NULL; }
    fclose(f);
    *len = (size_t)sz;
    return buf;
}

// Scale an RGB888 or RGBA8888 source into a new lv_img_dsc_t
static lv_img_dsc_t *make_scaled(const uint8_t *src, int sw, int sh, int bpp, int max_w, int max_h)
{
    if (sw <= 0 || sh <= 0) return NULL;
    int dw = max_w, dh = (int)((int64_t)sh * max_w / sw);
    if (dh > max_h) { dh = max_h; dw = (int)((int64_t)sw * max_h / sh); }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    lv_img_dsc_t *img = (lv_img_dsc_t *)calloc(1, sizeof(lv_img_dsc_t));
    lv_color_t *px = (lv_color_t *)ps_alloc((size_t)dw * dh * sizeof(lv_color_t));
    if (!img || !px) { free(img); free(px); return NULL; }
    for (int y = 0; y < dh; y++) {
        const uint8_t *row = src + (size_t)((int64_t)y * sh / dh) * sw * bpp;
        for (int x = 0; x < dw; x++) {
            const uint8_t *p = row + (size_t)((int64_t)x * sw / dw) * bpp;
            uint8_t r = p[0], g = p[1], b = p[2];
            if (bpp == 4 && p[3] < 255) {        // blend transparency onto black
                r = (uint8_t)(r * p[3] / 255);
                g = (uint8_t)(g * p[3] / 255);
                b = (uint8_t)(b * p[3] / 255);
            }
            px[(size_t)y * dw + x] = lv_color_make(r, g, b);
        }
    }
    img->header.always_zero = 0;
    img->header.cf = LV_IMG_CF_TRUE_COLOR;
    img->header.w = dw;
    img->header.h = dh;
    img->data_size = (uint32_t)dw * dh * sizeof(lv_color_t);
    img->data = (const uint8_t *)px;
    return img;
}

// ---- JPEG -------------------------------------------------------------------
struct JpgCtx {
    const uint8_t *data;
    size_t len, pos;
    uint8_t *out;     // RGB888
    int ow, oh;
};

static size_t jpg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    JpgCtx *c = (JpgCtx *)jd->device;
    if (c->pos + n > c->len) n = c->len - c->pos;
    if (buf) memcpy(buf, c->data + c->pos, n);
    c->pos += n;
    return n;
}

static int jpg_out(JDEC *jd, void *bitmap, JRECT *r)
{
    JpgCtx *c = (JpgCtx *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;
    int w = r->right - r->left + 1;
    for (int y = r->top; y <= r->bottom; y++) {
        for (int x = r->left; x <= r->right; x++, src += 3) {
            if (x < c->ow && y < c->oh) memcpy(c->out + ((size_t)y * c->ow + x) * 3, src, 3);
        }
    }
    (void)w;
    return 1;
}

static lv_img_dsc_t *decode_jpg(const uint8_t *data, size_t len, int max_w, int max_h)
{
    const size_t pool_sz = 32 * 1024;
    void *pool = malloc(pool_sz);
    if (!pool) return NULL;
    JpgCtx c = { data, len, 0, NULL, 0, 0 };
    JDEC jd;
    lv_img_dsc_t *img = NULL;
    JRESULT res = jd_prepare(&jd, jpg_in, pool, pool_sz, &c);
    if (res == JDR_OK) {
        uint8_t scale = 0;
        while (scale < 3 && (jd.width >> (scale + 1)) >= max_w && (jd.height >> (scale + 1)) >= max_h) scale++;
        c.ow = jd.width >> scale;
        c.oh = jd.height >> scale;
        c.out = (uint8_t *)ps_alloc((size_t)c.ow * c.oh * 3);
        if (c.out) {
            memset(c.out, 0, (size_t)c.ow * c.oh * 3);
            res = jd_decomp(&jd, jpg_out, scale);
            if (res == JDR_OK) img = make_scaled(c.out, c.ow, c.oh, 3, max_w, max_h);
            free(c.out);
        }
    }
    if (res != JDR_OK) ESP_LOGW(TAG, "jpeg decode failed (%d) - progressive JPEGs are not supported", (int)res);
    free(pool);
    return img;
}

// ---- PNG ----------------------------------------------------------------------
static lv_img_dsc_t *decode_png(const uint8_t *data, size_t len, int max_w, int max_h)
{
    unsigned char *rgba = NULL;
    unsigned w = 0, h = 0;
    unsigned err = lodepng_decode32(&rgba, &w, &h, data, len);
    if (err) {
        ESP_LOGW(TAG, "png decode failed: %u", err);
        if (rgba) lv_mem_free(rgba);
        return NULL;
    }
    lv_img_dsc_t *img = make_scaled(rgba, (int)w, (int)h, 4, max_w, max_h);
    lv_mem_free(rgba);
    return img;
}

lv_img_dsc_t *cover_decode(const char *path, int max_w, int max_h)
{
    size_t len = 0;
    uint8_t *data = read_file(path, &len);
    if (!data) return NULL;
    lv_img_dsc_t *img = NULL;
    if (len > 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        img = decode_png(data, len, max_w, max_h);
    else if (len > 3 && data[0] == 0xFF && data[1] == 0xD8)
        img = decode_jpg(data, len, max_w, max_h);
    free(data);
    if (img) ESP_LOGI(TAG, "%s -> %dx%d", path, img->header.w, img->header.h);
    return img;
}

void cover_free(lv_img_dsc_t *img)
{
    if (!img) return;
    free((void *)img->data);
    free(img);
}
