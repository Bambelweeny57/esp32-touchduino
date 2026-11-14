// Minimal Arduino compatibility layer so the MaxDuino engine sources compile
// (nearly) unchanged on ESP-IDF.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_timer.h"

typedef uint8_t byte;
typedef uint16_t word;
typedef bool boolean;

#define HIGH 1
#define LOW 0

// AVR flash-string helpers are no-ops on ESP32 (flash is memory mapped)
#define PROGMEM
#define PSTR(s) (s)
#define F(s) (s)
#define pgm_read_byte(addr) (*(const uint8_t *)(addr))
#define pgm_read_word(addr) (*(const uint16_t *)(addr))
#define memcmp_P memcmp
#define memcpy_P memcpy
#define strcasecmp_P strcasecmp
#define strcmp_P strcmp
#define strcpy_P strcpy
#define strncpy_P strncpy
#define strcat_P strcat
#define strlen_P strlen

#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define bitWrite(value, bit, bitvalue) ((bitvalue) ? bitSet(value, bit) : bitClear(value, bit))
#define lowByte(w) ((uint8_t)((w) & 0xff))
#define highByte(w) ((uint8_t)((w) >> 8))

// Must be always_inline: used from the IRAM tape ISR
__attribute__((always_inline)) static inline uint16_t makeWord(uint16_t w) { return w; }
__attribute__((always_inline)) static inline uint16_t makeWord(uint8_t h, uint8_t l) { return (uint16_t)((h << 8) | l); }
#define word(...) makeWord(__VA_ARGS__)

// The tape ISR runs on core 1, as does all engine code, so a critical section
// on that core is enough to keep the ISR out while the buffer is written.
extern portMUX_TYPE g_tape_mux;
#define noInterrupts() portENTER_CRITICAL(&g_tape_mux)
#define interrupts() portEXIT_CRITICAL(&g_tape_mux)

static inline void delay(uint32_t ms) { vTaskDelay(ms < portTICK_PERIOD_MS ? 1 : pdMS_TO_TICKS(ms)); }
static inline unsigned long millis(void) { return (unsigned long)(esp_timer_get_time() / 1000); }
static inline unsigned long micros(void) { return (unsigned long)esp_timer_get_time(); }
