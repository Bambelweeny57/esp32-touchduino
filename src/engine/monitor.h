// Speaker output: mirrors the tape signal to the onboard I2S class-D amp.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_attr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MON_RING_SIZE 8192   // power of two

typedef struct {
    uint32_t t;      // tape timeline, microseconds
    uint8_t level;
} mon_edge_t;

extern mon_edge_t g_mon_ring[MON_RING_SIZE];
extern volatile uint32_t g_mon_head;
extern volatile uint32_t g_mon_tail;
extern volatile uint8_t g_mon_last_level;
extern volatile bool g_mon_enabled;
extern volatile uint32_t g_mon_drops;   // edges not recorded (ring full)

// Called from the tape ISR on every timer tick: records level changes
__attribute__((always_inline)) static inline void monitor_isr_record(uint32_t t, uint8_t level)
{
    // Always recorded (the UI border reads it too); the speaker task drains it
    if (level == g_mon_last_level) return;
    g_mon_last_level = level;
    uint32_t h = g_mon_head;
    if (h - g_mon_tail >= MON_RING_SIZE) { g_mon_drops = g_mon_drops + 1; return; }   // full: drop
    g_mon_ring[h & (MON_RING_SIZE - 1)].t = t;
    g_mon_ring[h & (MON_RING_SIZE - 1)].level = level;
    __sync_synchronize();
    g_mon_head = h + 1;
}

// C64 self-check: compares every pulse sent (falling edge to falling edge, what
// the C64 times) with the .tap file. Takes ownership of pulses (us, free()).
void c64check_start(uint16_t *pulses, uint32_t count);
void c64check_stop(void);              // tape stopped: report what was checked

void monitor_start(void);              // starts the I2S task (core 0)
void monitor_set_enabled(bool on);
void monitor_set_volume(int percent);  // 0..100
void monitor_test_beep(int ms);         // 1 kHz test tone, independent of the tape

#ifdef __cplusplus
}
#endif
