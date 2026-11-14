#include "configs.h"
#include "file_utils.h"
#include "esp_heap_caps.h"

#define TAPE_CACHE_SIZE (16 * 1024)

TapeFile entry;
unsigned long bytesRead = 0;
unsigned long filesize = 0;
byte lastByte;
_readout_type readout;
char fileName[256];

byte filebuffer[20];

bool TapeFile::open(const char *path)
{
    close();
    f = fopen(path, "rb");
    if (!f) return false;
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
    cache_len = 0;
    pos = 0;
}

bool TapeFile::seekSet(uint32_t p)
{
    if (!f || p > size) return false;
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
