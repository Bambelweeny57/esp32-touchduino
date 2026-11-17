#ifndef FILE_UTILS_H_INCLUDED
#define FILE_UTILS_H_INCLUDED

// Replacement for MaxDuino's SdFat-based file_utils: same globals and helpers,
// backed by a C FILE* with a read cache (the engine does many 1-4 byte reads at
// arbitrary offsets, which would be slow as raw fseek/fread calls).

#include "Arduino.h"
#include <stdio.h>

class TapeFile
{
  public:
    bool open(const char *path);
    void close();
    bool isOpen() const { return f != NULL || mem != NULL; }
    bool seekSet(uint32_t pos);
    int read(void *buf, size_t n);
    uint32_t fileSize() const { return size; }

  private:
    bool fill(uint32_t pos);
    bool open_gzip(FILE *src);
    uint8_t *mem = NULL;      // whole unpacked file (gzip input), else NULL
    FILE *f = NULL;
    uint8_t *cache = NULL;
    uint32_t cache_pos = 0;   // file offset of cache[0]
    uint32_t cache_len = 0;   // valid bytes in cache
    uint32_t pos = 0;
    uint32_t size = 0;
};

extern TapeFile entry;               // the file being played
extern unsigned long filesize;
extern unsigned long bytesRead;
extern char fileName[];              // basename of the file being played

extern byte filebuffer[]; // used for small reads (readfile, ReadByte, ...), sized for the largest header read
extern byte lastByte;

byte readfile(byte nbytes, unsigned long p);
byte ReadByte();
byte ReadWord();
byte ReadLong();
byte ReadDword();

typedef union {
  byte outbyte;
  word outword;
  unsigned long outlong=0;
} _readout_type;

extern _readout_type readout;

#define outByte readout.outbyte
#define outWord readout.outword
#define outLong readout.outlong

#endif // FILE_UTILS_H_INCLUDED
