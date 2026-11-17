#include "configs.h"
#include "file_utils.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "miniz.h"   // the ROM's inflater

#define TAPE_CACHE_SIZE (4 * 1024)   // small reads: one slow SD read can never outlast a buffer page

TapeFile entry;
unsigned long bytesRead = 0;
unsigned long filesize = 0;
byte lastByte;
_readout_type readout;
char fileName[256];

byte filebuffer[20];

// Most archive UEFs (Stairway to Hell etc.) are gzip-compressed, and MaxDuino
// only reads plain files. A gzip file (1f 8b) is unpacked into PSRAM with the
// ROM's inflater and then served from memory; nothing is written to the card.
#define GZIP_MAX_UNPACKED (4u * 1024 * 1024)

bool TapeFile::open_gzip(FILE *src)
{
    fseek(src, 0, SEEK_END);
    long n = ftell(src);
    fseek(src, 0, SEEK_SET);
    if (n < 18) return false;
    uint8_t *in = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!in) return false;
    bool ok = fread(in, 1, n, src) == (size_t)n;
    // gzip header: 1f 8b, method 8 (deflate), flags, mtime(4), xfl, os
    uint32_t out_len = in[n - 4] | (in[n - 3] << 8) | (in[n - 2] << 16) | ((uint32_t)in[n - 1] << 24);
    size_t p = 10;
    uint8_t flg = in[3];
    ok = ok && in[2] == 8 && out_len > 0 && out_len <= GZIP_MAX_UNPACKED;
    if (ok && (flg & 4)) p += 2 + (in[p] | (in[p + 1] << 8));      // FEXTRA
    if (ok && (flg & 8))  { while (p < (size_t)n && in[p]) p++; p++; }   // FNAME
    if (ok && (flg & 16)) { while (p < (size_t)n && in[p]) p++; p++; }   // FCOMMENT
    if (ok && (flg & 2)) p += 2;                                       // FHCRC
    ok = ok && p < (size_t)n - 8;
    if (ok) {
        // The decompressor state is ~11 KB: keep it on the heap, never on the
        // tape task's 6 KB stack (tinfl_decompress_mem_to_mem puts it there and
        // overflowed it: a reboot on pressing play).
        mem = (uint8_t *)heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM);
        tinfl_decompressor *d = (tinfl_decompressor *)heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
        ok = mem && d;
        if (ok) {
            tinfl_init(d);
            size_t in_len = n - 8 - p, got = out_len;
            tinfl_status st = tinfl_decompress(d, in + p, &in_len, mem, mem, &got, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            ok = st == TINFL_STATUS_DONE && got == out_len;
        }
        free(d);
        if (!ok) { free(mem); mem = NULL; }
    }
    free(in);
    if (ok) {
        size = out_len;
        ESP_LOGI("tapefile", "gzip: %ld bytes unpacked to %lu", n, (unsigned long)out_len);
    } else {
        ESP_LOGW("tapefile", "gzip: could not unpack");
    }
    return ok;
}

bool TapeFile::open(const char *path)
{
    close();
    f = fopen(path, "rb");
    if (!f) return false;
    uint8_t magic[2] = { 0, 0 };
    if (fread(magic, 1, 2, f) == 2 && magic[0] == 0x1f && magic[1] == 0x8b) {
        bool ok = open_gzip(f);
        fclose(f);
        f = NULL;
        pos = 0;
        return ok;
    }
    fseek(f, 0, SEEK_SET);
    setvbuf(f, NULL, _IONBF, 0);   // we do our own caching
    if (!cache) {
        cache = (uint8_t *)heap_caps_malloc(TAPE_CACHE_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!cache) cache = (uint8_t *)malloc(TAPE_CACHE_SIZE);
    }
    fseek(f, 0, SEEK_END);
    size = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    pos = 0;
    cache_pos = 0;
    cache_len = 0;
    return cache != NULL;
}

void TapeFile::close()
{
    if (f) fclose(f);
    f = NULL;
    free(mem);
    mem = NULL;
    cache_len = 0;
    pos = 0;
}

bool TapeFile::seekSet(uint32_t p)
{
    if (!isOpen() || p > size) return false;
    pos = p;
    return true;
}

bool TapeFile::fill(uint32_t p)
{
    uint32_t start = p & ~511u;             // sector aligned reads are fastest
    if (fseek(f, (long)start, SEEK_SET) != 0) return false;
    size_t n = fread(cache, 1, TAPE_CACHE_SIZE, f);
    cache_pos = start;
    cache_len = (uint32_t)n;
    return n > 0;
}

int TapeFile::read(void *buf, size_t n)
{
    if (mem) {
        if (pos >= size) return 0;
        if (n > size - pos) n = size - pos;
        memcpy(buf, mem + pos, n);
        pos += n;
        return (int)n;
    }
    if (!f) return -1;
    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;
    while (done < n && pos < size) {
        if (pos < cache_pos || pos >= cache_pos + cache_len) {
            if (!fill(pos)) break;
            if (pos >= cache_pos + cache_len) break;
        }
        uint32_t avail = cache_pos + cache_len - pos;
        size_t chunk = n - done;
        if (chunk > avail) chunk = avail;
        memcpy(out + done, cache + (pos - cache_pos), chunk);
        done += chunk;
        pos += chunk;
    }
    return (int)done;
}

byte readfile(byte nbytes, unsigned long p)
{
  byte i=0;
  if(entry.seekSet(p)) {
    int r = entry.read(filebuffer, nbytes);
    i = r > 0 ? (byte)r : 0;
  }
  return i;
}

byte ReadByte() {
  if(readfile(1, bytesRead)==1)
  {
    bytesRead += 1;
    outByte = filebuffer[0];
    return true;
  }
  return false;
}

byte ReadWord() {
  if(readfile(2, bytesRead)==2)
  {
    bytesRead += 2;
    outWord = word(filebuffer[1], filebuffer[0]);
    return true;
  }
  return false;
}

byte ReadLong() {
  if(readfile(3, bytesRead)==3)
  {
    bytesRead += 3;
    outLong = ((unsigned long) word(filebuffer[2], filebuffer[1]) << 8) | filebuffer[0];
    return true;
  }
  return false;
}

byte ReadDword() {
  if(readfile(4, bytesRead)==4)
  {
    bytesRead += 4;
    outLong = ((unsigned long)word(filebuffer[3], filebuffer[2]) << 16) | word(filebuffer[1], filebuffer[0]);
    return true;
  }
  return false;
}
