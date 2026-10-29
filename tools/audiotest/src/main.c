// Audio pin test for the Sunton ESP32-8048S050 V1.3 (esp32-quake).
//
// The onboard NS4168 I2S amplifier (speaker socket P6 "Speak") gets its data on
// GPIO17 and its word select (LRCLK) on GPIO18. Its bit clock (BCLK) is on GPIO0
// on V1.1 boards and GPIO19 on V1.0; V1.3 is undocumented. This test plays:
//
//   A: BCLK on GPIO0  -> ONE long LOW tone
//   B: BCLK on GPIO19 -> THREE short HIGH beeps
//
// over and over, and prints which one is playing. Whichever you hear is the wiring.
// Unplug the button pad cable from P3 first: GPIO19 is its SDA line.
// The volume is kept low: a small buzzer can be overdriven at full power.

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

#define SAMPLE_RATE     16000
#define PIN_DOUT        17
#define PIN_WS          18
#define AMPLITUDE       0.15f       // 15% of full scale
#define CHUNK_FRAMES    256

static const char *TAG = "audiotest";

static void playTones(int bclkPin, float freq, int beeps, int beepMs, int gapMs)
{
    i2s_chan_handle_t tx;
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.auto_clear = true;          // silence when no data
    ESP_ERROR_CHECK(i2s_new_channel(&chanCfg, &tx, NULL));
    i2s_std_config_t stdCfg =
    {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
        {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclkPin,
            .ws = PIN_WS,
            .dout = PIN_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &stdCfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));

    static int16_t buf[CHUNK_FRAMES * 2];       // stereo: same sample on both channels
    static int16_t silence[CHUNK_FRAMES * 2];
    float phase = 0, step = 2.0f * (float) M_PI * freq / SAMPLE_RATE;
    for (int b = 0; b < beeps; b++)
    {
        int frames = SAMPLE_RATE * beepMs / 1000;
        int fade = SAMPLE_RATE / 200;           // 5 ms fade in/out: no clicks
        for (int f = 0; f < frames; f += CHUNK_FRAMES)
        {
            for (int i = 0; i < CHUNK_FRAMES; i++)
            {
                int n = f + i;
                float env = 1.0f;
                if (n < fade)
                    env = (float) n / fade;
                else if (n > frames - fade)
                    env = n >= frames ? 0 : (float) (frames - n) / fade;
                int16_t s = (int16_t) (sinf(phase) * AMPLITUDE * env * 32767);
                buf[2 * i] = buf[2 * i + 1] = s;
                phase += step;
                if (phase > 2.0f * (float) M_PI)
                    phase -= 2.0f * (float) M_PI;
            }
            size_t written;
            i2s_channel_write(tx, buf, sizeof(buf), &written, portMAX_DELAY);
        }
        for (int g = 0; g < SAMPLE_RATE * gapMs / 1000; g += CHUNK_FRAMES)
        {
            size_t written;
            i2s_channel_write(tx, silence, sizeof(silence), &written, portMAX_DELAY);
        }
    }
    i2s_channel_disable(tx);
    i2s_del_channel(tx);
}

void app_main(void)
{
    ESP_LOGI(TAG, "NS4168 bit clock test: A = BCLK GPIO0 (one LOW tone), B = BCLK GPIO19 (three HIGH beeps)");
    while (1)
    {
        printf("\n>>> A: bit clock on GPIO0  - ONE long LOW tone (500 Hz)\n");
        playTones(0, 500.0f, 1, 1200, 0);
        vTaskDelay(pdMS_TO_TICKS(1500));
        printf(">>> B: bit clock on GPIO19 - THREE short HIGH beeps (2000 Hz)\n");
        playTones(19, 2000.0f, 3, 200, 200);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}
