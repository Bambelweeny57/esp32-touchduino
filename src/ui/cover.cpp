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

// ---- ZX Spectrum loading screens ------------------------------------------
//
// A SCREEN$ is 6912 bytes of display memory: 6144 bytes of bitmap (in the
// Spectrum's interleaved row order) then 768 attribute bytes (ink, paper,
// bright per 8x8 cell). On tape it's a data block of 6914 bytes (flag 0xFF +
// screen + checksum), usually after a "CODE 16384,6912" header.

#define ZX_SCR_BYTES 6912

struct TapeBlock { uint32_t data; uint32_t len; };   // data = file offset of the block's first byte (flag)

static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd24(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16); }
static uint32_t rd32(const uint8_t *p) { return rd24(p) | ((uint32_t)p[3] << 24); }

// List the data blocks (standard, turbo and pure data) of a TZX or TAP file
static int list_tape_blocks(FILE *f, uint32_t size, bool tzx, TapeBlock *out, int max)
{
    int n = 0;
    uint8_t h[0x14];
    if (!tzx) {
        uint32_t off = 0;
        while (off + 2 <= size && n < max) {
            if (fseek(f, off, SEEK_SET) || fread(h, 1, 2, f) != 2) break;
            uint32_t len = rd16(h);
            if (len == 0 || off + 2 + len > size) break;
            out[n++] = { off + 2, len };
            off += 2 + len;
        }
        return n;
    }
    uint32_t off = 10;
    for (int guard = 0; off < size && n < max && guard < 2000; guard++) {
        memset(h, 0, sizeof(h));
        if (fseek(f, off, SEEK_SET)) break;
        uint8_t id;
        if (fread(&id, 1, 1, f) != 1) break;
        fread(h, 1, sizeof(h), f);
        uint32_t p = off + 1, len;
        switch (id) {
            case 0x10: len = 4 + rd16(h + 2);       out[n++] = { p + 4, rd16(h + 2) };      break;
            case 0x11: len = 0x12 + rd24(h + 0x0F); out[n++] = { p + 0x12, rd24(h + 0x0F) }; break;
            case 0x14: len = 0x0A + rd24(h + 7);    out[n++] = { p + 0x0A, rd24(h + 7) };    break;
            case 0x12: len = 4; break;
            case 0x13: len = 1 + h[0] * 2; break;
            case 0x15: len = 8 + rd24(h + 5); break;
            case 0x18: case 0x19: len = 4 + rd32(h); break;
            case 0x20: case 0x23: case 0x24: len = 2; break;
            case 0x21: len = 1 + h[0]; break;
            case 0x22: case 0x25: case 0x27: len = 0; break;
            case 0x26: len = 2 + rd16(h) * 2; break;
            case 0x28: len = 2 + rd16(h); break;
            case 0x2A: len = 4; break;
            case 0x2B: len = 5; break;
            case 0x30: len = 1 + h[0]; break;
            case 0x31: len = 2 + h[1]; break;
            case 0x32: len = 2 + rd16(h); break;
            case 0x33: len = 1 + h[0] * 3; break;
            case 0x35: len = 0x14 + rd32(h + 0x10); break;
            case 0x4B: len = 4 + rd32(h); break;
            case 0x5A: len = 9; break;
            default:   len = 4 + rd32(h); break;   // TZX extension rule
        }
        off = p + len;
    }
    return n;
}

// Find the file offset of the 6912 screen bytes, or 0 if there's no screen
static uint32_t find_screen(FILE *f, const TapeBlock *b, int n)
{
    // 1. A "CODE 16384,6912" header followed by its data block
    for (int i = 0; i + 1 < n; i++) {
        if (b[i].len != 19) continue;
        uint8_t hdr[19];
        if (fseek(f, b[i].data, SEEK_SET) || fread(hdr, 1, 19, f) != 19) continue;
        if (hdr[0] == 0x00 && hdr[1] == 3 && rd16(hdr + 12) == ZX_SCR_BYTES && rd16(hdr + 14) == 16384 &&
            b[i + 1].len >= ZX_SCR_BYTES + 1) {
            return b[i + 1].data + 1;                     // skip the flag byte
        }
    }
    // 2. Headerless: a data block that is exactly a screen (with flag + checksum)
    for (int i = 0; i < n; i++) {
        uint8_t flag;
        if (b[i].len == ZX_SCR_BYTES + 2) {
            if (fseek(f, b[i].data, SEEK_SET) || fread(&flag, 1, 1, f) != 1) continue;
            if (flag == 0xFF) return b[i].data + 1;
        }
    }
    return 0;
}

// Box-filter downscale of an RGB888 image (keeps 1-pixel detail visible)
static lv_img_dsc_t *make_scaled_avg(const uint8_t *src, int sw, int sh, int max_w, int max_h)
{
    int dw = max_w, dh = (int)((int64_t)sh * max_w / sw);
    if (dh > max_h) { dh = max_h; dw = (int)((int64_t)sw * max_h / sh); }
    lv_img_dsc_t *img = (lv_img_dsc_t *)calloc(1, sizeof(lv_img_dsc_t));
    lv_color_t *px = (lv_color_t *)ps_alloc((size_t)dw * dh * sizeof(lv_color_t));
    if (!img || !px) { free(img); free(px); return NULL; }
    for (int y = 0; y < dh; y++) {
        int y0 = y * sh / dh, y1 = LV_MAX(y0 + 1, (y + 1) * sh / dh);
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw, x1 = LV_MAX(x0 + 1, (x + 1) * sw / dw);
            uint32_t r = 0, g = 0, bl = 0, c = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t *p = src + ((size_t)yy * sw + x0) * 3;
                for (int xx = x0; xx < x1; xx++, p += 3) { r += p[0]; g += p[1]; bl += p[2]; c++; }
            }
            px[(size_t)y * dw + x] = lv_color_make(r / c, g / c, bl / c);
        }
    }
    img->header.cf = LV_IMG_CF_TRUE_COLOR;
    img->header.w = dw;
    img->header.h = dh;
    img->data_size = (uint32_t)dw * dh * sizeof(lv_color_t);
    img->data = (const uint8_t *)px;
    return img;
}

lv_img_dsc_t *cover_from_spectrum_tape(const char *path, int max_w, int max_h)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return NULL;
    bool tzx = !strcasecmp(dot, ".tzx");
    if (!tzx && strcasecmp(dot, ".tap")) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    uint32_t size = (uint32_t)ftell(f);
    uint8_t magic[12] = {};
    fseek(f, 0, SEEK_SET);
    fread(magic, 1, sizeof(magic), f);
    lv_img_dsc_t *img = NULL;
    if ((tzx && memcmp(magic, "ZXTape!\x1A", 8) != 0) || (!tzx && memcmp(magic, "C64-TAPE-RAW", 12) == 0)) {
        fclose(f);
        return NULL;
    }

    static TapeBlock blocks[256];
    int n = list_tape_blocks(f, size, tzx, blocks, 256);
    uint32_t scr_off = find_screen(f, blocks, n);
    uint8_t *scr = scr_off ? (uint8_t *)ps_alloc(ZX_SCR_BYTES) : NULL;
    if (scr && fseek(f, scr_off, SEEK_SET) == 0 && fread(scr, 1, ZX_SCR_BYTES, f) == ZX_SCR_BYTES) {
        uint8_t *rgb = (uint8_t *)ps_alloc(256 * 192 * 3);
        if (rgb) {
            for (int y = 0; y < 192; y++) {
                // Spectrum bitmap order: thirds, then character row, then pixel row
                const uint8_t *row = scr + (((y & 0xC0) << 5) | ((y & 0x07) << 8) | ((y & 0x38) << 2));
                const uint8_t *attr = scr + 6144 + (y >> 3) * 32;
                for (int cx = 0; cx < 32; cx++) {
                    uint8_t a = attr[cx], bits = row[cx];
                    uint8_t level = (a & 0x40) ? 0xFF : 0xD7;            // BRIGHT
                    for (int b = 0; b < 8; b++) {
                        uint8_t c = (bits & (0x80 >> b)) ? (a & 7) : ((a >> 3) & 7);   // ink : paper
                        uint8_t *p = rgb + ((size_t)y * 256 + cx * 8 + b) * 3;
                        p[0] = (c & 2) ? level : 0;   // red
                        p[1] = (c & 4) ? level : 0;   // green
                        p[2] = (c & 1) ? level : 0;   // blue
                    }
                }
            }
            img = make_scaled_avg(rgb, 256, 192, max_w, max_h);
            free(rgb);
        }
    }
    free(scr);
    fclose(f);
    if (img) ESP_LOGI(TAG, "%s: loading screen from tape -> %dx%d", path, img->header.w, img->header.h);
    return img;
}

void cover_free(lv_img_dsc_t *img)
{
    if (!img) return;
    free((void *)img->data);
    free(img);
}
