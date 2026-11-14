// Speaker output via the onboard I2S class-D amplifier.
//
// The tape ISR (core 1) records every level change with its timestamp on the
// tape timeline. This task (core 0) replays those edges as a square wave into
// I2S at 44.1 kHz, running a fixed ~40 ms behind the live signal. It never
// touches the tape timing itself, so a slow or stalled speaker task can't
// disturb loading.

#include "monitor.h"
#include "TimerCounter.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "monitor";

#define MON_I2S_BCK   42
#define MON_I2S_LRCK  2
#define MON_I2S_DOUT  41
#define MON_RATE      44100
#define MON_FRAMES    256
#define MON_LATENCY_US 40000
#define MON_RESYNC_US  30000

mon_edge_t g_mon_ring[MON_RING_SIZE];
volatile uint32_t g_mon_head = 0;
volatile uint32_t g_mon_tail = 0;
volatile uint8_t g_mon_last_level = 0;
volatile bool g_mon_enabled = true;

static volatile int s_volume = 40;
static i2s_chan_handle_t s_tx = NULL;

void monitor_set_enabled(bool on) { g_mon_enabled = on; }
void monitor_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
}

static volatile int s_beep_frames = 0;

void monitor_test_beep(int ms) { s_beep_frames = ms * (MON_RATE / 1000); }

static void monitor_task(void *arg)
{
    static int16_t buf[MON_FRAMES * 2];
    const int64_t step_ns = 1000000000LL / MON_RATE;
    int64_t cursor_ns = 0;
    uint8_t level = 0;
    float x_prev = 0, y_prev = 0;

    for (;;) {
        bool on = g_mon_enabled;
        int64_t target_ns = ((int64_t)tape_timer_now_us() - MON_LATENCY_US) * 1000;
        if (llabs(cursor_ns - target_ns) > (int64_t)MON_RESYNC_US * 1000) {
            cursor_ns = target_ns;
        }
        float amp = on ? (float)s_volume * 160.0f : 0.0f;   // 100% -> 16000
        bool beep = s_beep_frames > 0;
        if (beep) amp = (float)(s_volume < 20 ? 20 : s_volume) * 160.0f;

        for (int i = 0; i < MON_FRAMES; i++) {
            cursor_ns += step_ns;
            uint32_t t = (uint32_t)(cursor_ns / 1000);
            uint32_t tail = g_mon_tail;
            while (tail != g_mon_head) {
                const mon_edge_t *e = &g_mon_ring[tail & (MON_RING_SIZE - 1)];
                if ((int32_t)(e->t - t) > 0) break;
                level = e->level;
                tail++;
            }
            g_mon_tail = tail;
            if (beep) {
                level = (s_beep_frames / 22) & 1;   // ~1 kHz square
                if (s_beep_frames > 0) s_beep_frames = s_beep_frames - 1;
            }

            // Square wave through a DC blocker, so long pauses at one level
            // settle to silence instead of holding DC on the speaker
            float x = level ? amp : -amp;
            float y = x - x_prev + 0.995f * y_prev;
            x_prev = x;
            y_prev = y;
            if (y > 32000) y = 32000;
            if (y < -32000) y = -32000;
            buf[i * 2] = buf[i * 2 + 1] = (int16_t)y;
        }
        size_t written = 0;
        i2s_channel_write(s_tx, buf, sizeof(buf), &written, portMAX_DELAY);
    }
}

void monitor_start(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = MON_FRAMES;
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &s_tx, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed");
        return;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MON_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)MON_I2S_BCK,
            .ws = (gpio_num_t)MON_I2S_LRCK,
            .dout = (gpio_num_t)MON_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {},
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    xTaskCreatePinnedToCore(monitor_task, "spk", 4096, NULL, 6, NULL, 0);
    ESP_LOGI(TAG, "speaker monitor on I2S (BCK %d, WS %d, DOUT %d)", MON_I2S_BCK, MON_I2S_LRCK, MON_I2S_DOUT);
}
