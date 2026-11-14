// Tape pulse timer: TIMG1 timer 0 driven directly through the (always_inline)
// timer LL, with an IRAM interrupt allocated on core 1.
//
// We don't use the gptimer driver because PlatformIO doesn't apply its
// linker fragment, which leaves gptimer_set_alarm_action() in flash. That
// would make the ISR stall (or crash) whenever flash cache is disabled.
//
// Each alarm is armed relative to the previous alarm time (not "now"), so ISR
// latency never accumulates into the pulse timing.

#include "TimerCounter.h"
#include "hal/timer_ll.h"
#include "soc/timer_group_struct.h"
#include "soc/system_struct.h"
#include "esp_intr_alloc.h"
#include "soc/interrupts.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "monitor.h"
#include "pinSetup.h"

static const char *TAG = "tape_timer";

#define TG_HW   (&TIMERG1)
#define TG_NUM  0

static intr_handle_t s_intr = NULL;
static volatile timerCallback s_cb = NULL;
static volatile uint32_t s_period = 50000;
static volatile bool s_in_isr = false;
static volatile uint64_t s_alarm = 0;

// Snapshot for other cores: tape time <-> esp_timer time (seqlock)
static volatile uint32_t s_seq = 0;
static volatile uint32_t s_snap_tape = 0;
static volatile int64_t s_snap_esp = 0;

__attribute__((always_inline)) static inline uint64_t read_counter(void)
{
    timer_ll_trigger_soft_capture(TG_HW, TG_NUM);
    return timer_ll_get_counter_value(TG_HW, TG_NUM);
}

__attribute__((always_inline)) static inline void arm_at(uint64_t target, uint64_t now)
{
    if (target < now + 2) target = now + 2;   // never arm in the past
    s_alarm = target;
    timer_ll_set_alarm_value(TG_HW, TG_NUM, target);
    timer_ll_enable_alarm(TG_HW, TG_NUM, true);  // alarm enable self-clears on each alarm
}

static void IRAM_ATTR tape_isr(void *arg)
{
    if (!(timer_ll_get_intr_status(TG_HW) & TIMER_LL_EVENT_ALARM(TG_NUM))) return;
    timer_ll_clear_intr_status(TG_HW, TIMER_LL_EVENT_ALARM(TG_NUM));

    uint64_t alarm = s_alarm;
    s_period = 50000;               // idle tick when no callback attached
    timerCallback cb = s_cb;
    if (cb) {
        s_in_isr = true;
        cb();                       // wave2(): sets the pin and calls Timer.setPeriod()
        s_in_isr = false;
    }
    monitor_isr_record((uint32_t)alarm, g_tape_level);

    s_seq = s_seq + 1;
    s_snap_tape = (uint32_t)alarm;
    s_snap_esp = esp_timer_get_time();
    s_seq = s_seq + 1;

    arm_at(alarm + s_period, read_counter());
}

void tape_timer_create(void)
{
    if (s_intr) return;
    SYSTEM.perip_clk_en0.timergroup1_clk_en = 1;   // already on (IWDT lives in TG1); never reset it
    timer_ll_enable_counter(TG_HW, TG_NUM, false);
    timer_ll_set_clock_source(TG_HW, TG_NUM, GPTIMER_CLK_SRC_APB);
    timer_ll_set_clock_prescale(TG_HW, TG_NUM, 80);   // 80 MHz APB / 80 = 1 MHz
    timer_ll_set_count_direction(TG_HW, TG_NUM, GPTIMER_COUNT_UP);
    timer_ll_enable_auto_reload(TG_HW, TG_NUM, false);
    timer_ll_set_reload_value(TG_HW, TG_NUM, 0);
    timer_ll_trigger_soft_reload(TG_HW, TG_NUM);
    timer_ll_enable_intr(TG_HW, TIMER_LL_EVENT_ALARM(TG_NUM), false);
    timer_ll_clear_intr_status(TG_HW, TIMER_LL_EVENT_ALARM(TG_NUM));

    ESP_ERROR_CHECK(esp_intr_alloc(ETS_TG1_T0_LEVEL_INTR_SOURCE, ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3,
                                   tape_isr, NULL, &s_intr));
    timer_ll_enable_intr(TG_HW, TIMER_LL_EVENT_ALARM(TG_NUM), true);
    timer_ll_enable_counter(TG_HW, TG_NUM, true);
    arm_at(50000, 0);
    ESP_LOGI(TAG, "tape timer running (TIMG1 T0), ISR on core %d", xPortGetCoreID());
}

uint64_t tape_timer_now_us(void)
{
    // Safe from any core: extrapolate from the ISR's last snapshot
    uint32_t seq, tape;
    int64_t esp;
    do {
        seq = s_seq;
        tape = s_snap_tape;
        esp = s_snap_esp;
    } while ((seq & 1) || seq != s_seq);
    return (uint64_t)tape + (uint64_t)(esp_timer_get_time() - esp);
}

TimerCounter::TimerCounter() {}

void TimerCounter::initialize(unsigned long microseconds)
{
    s_cb = NULL;
    setPeriod(microseconds);
}

IRAM_ATTR void TimerCounter::setPeriod(unsigned long microseconds)
{
    if (microseconds == 0) microseconds = 1;
    if (s_in_isr) {
        s_period = microseconds;    // applied by the ISR after the callback returns
        return;
    }
    // Called from the engine task (core 1): re-arm relative to now
    if (!s_intr) return;
    noInterrupts();
    uint64_t now = read_counter();
    arm_at(now + microseconds, now);
    interrupts();
}

void TimerCounter::stop()
{
    s_cb = NULL;
}

void TimerCounter::attachInterrupt(timerCallback isr)
{
    s_cb = isr;
}

DRAM_ATTR TimerCounter Timer;
