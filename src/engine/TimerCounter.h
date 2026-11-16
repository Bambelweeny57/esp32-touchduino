#ifndef TIMER_H_INCLUDED
#define TIMER_H_INCLUDED
#include <stdint.h>

typedef void (*timerCallback)();

// Drop-in for MaxDuino's TimerCounter, backed by an ESP-IDF gptimer (1 MHz).
// Each alarm is armed relative to the previous alarm time (not "now"), so ISR
// latency never accumulates into the pulse timing.
class TimerCounter
{
  public:
    TimerCounter();
    void initialize(unsigned long microseconds);
    void setPeriod(unsigned long microseconds);
    void stop();
    void attachInterrupt(timerCallback isr);
};
extern TimerCounter Timer;   // TouchDuino: object (not reference) so the ISR never reads flash

// Must be called once from the engine task (core 1) so the ISR lands on core 1
void tape_timer_create(void);
// Current tape timeline in microseconds (used by the speaker monitor)
uint64_t tape_timer_now_us(void);

// TouchDuino diagnostics, reset by the engine when it logs them
struct TapeDiag {
    volatile uint32_t max_late_us;   // worst interrupt lateness
    volatile uint32_t late_count;    // interrupts more than 50 us late
    volatile uint32_t underruns;     // ISR switched to a buffer page the engine had not refilled
};
extern TapeDiag g_tape_diag;

#endif
