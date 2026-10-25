/**
 * @file esp_display_rgb.c
 * @brief Display driver for 16-bit parallel RGB panels (Sunton ESP32-8048S050,
 * 800x480 ST7262), implementing the display.h interface used by esp_graphics.c.
 *
 * The panel is refreshed continuously by the LCD peripheral's DMA from a frame
 * buffer in PSRAM. The rest of the port still produces 320x240 RGB565 lines
 * (big endian, as SPI panels want them); each line is byte-swapped, scaled by
 * RGB_PANEL_SCALE and written to the frame buffer, centered. No bounce buffers:
 * they would cost a large share of core 0, which also renders half the walls.
 *
 * Part of esp32-quake (https://github.com/johnmacdowall/esp32-quake), added to
 * the QuakeESP32S3 port by recrof (based on MG24Quake by Nicola Wrachien).
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#include <string.h>
#include "display.h"

#if DISPLAY_CONTROLLER == DISPLAY_CONTROLLER_RGB
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_cache.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if !CONFIG_LCD_RGB_RESTART_IN_VSYNC
#error "CONFIG_LCD_RGB_RESTART_IN_VSYNC is off: the picture can shift sideways. It is set in sdkconfig.defaults: delete the generated sdkconfig.esp32-8048S050 file (next to platformio.ini) and build again."
#endif

#if RGB_PANEL_SCALE < 1 || LCD_WIDTH * RGB_PANEL_SCALE > RGB_PANEL_WIDTH || LCD_HEIGHT * RGB_PANEL_SCALE > RGB_PANEL_HEIGHT
#error "RGB_PANEL_SCALE too large for the panel"
#endif

#ifndef RGB_PANEL_FILL
#define RGB_PANEL_FILL  0
#endif
#if RGB_PANEL_FILL
// Full screen: 320 -> 800 columns (x2.5: pixels repeated 3, 2, 3, 2... times),
// 240 -> 480 rows (x2). The image is 25% wider than Quake's 4:3.
#if RGB_PANEL_SCALE != 2 || LCD_WIDTH * 5 / 2 != RGB_PANEL_WIDTH || LCD_HEIGHT * 2 != RGB_PANEL_HEIGHT
#error "RGB_PANEL_FILL needs RGB_PANEL_SCALE 2 and an 800x480 panel"
#endif
#define OUT_WIDTH       RGB_PANEL_WIDTH
#else
#define OUT_WIDTH       (LCD_WIDTH * RGB_PANEL_SCALE)
#endif
#define OUT_HEIGHT      (LCD_HEIGHT * RGB_PANEL_SCALE)
#define OUT_X           ((RGB_PANEL_WIDTH - OUT_WIDTH) / 2)
#define OUT_Y           ((RGB_PANEL_HEIGHT - OUT_HEIGHT) / 2)
#define ROW_BYTES       (RGB_PANEL_WIDTH * sizeof(uint16_t))

static const char *TAG = "display_rgb";
static esp_lcd_panel_handle_t panel;
static uint16_t *frameBuffer;
static int windowLine;              // next 320x240 line to be written
static bool ready;

bool displayIsReady(void)
{
    return ready;
}

void displaySetWindow(int firstLine, int numLines)
{
    (void) numLines;
    windowLine = firstLine;
}

// The LCD DMA reads PSRAM directly: write the CPU cache back for these rows
static void flushRows(int firstRow, int numRows)
{
    if (numRows <= 0)
    {
        return;
    }
    esp_cache_msync((uint8_t*) frameBuffer + firstRow * ROW_BYTES, numRows * ROW_BYTES,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void displaySendPixels(const uint16_t *data, size_t numPixels, bool last)
{
    (void) last;
    int lines = numPixels / LCD_WIDTH;
    if (windowLine + lines > LCD_HEIGHT)
    {
        lines = LCD_HEIGHT - windowLine;
    }
    int firstRow = OUT_Y + windowLine * RGB_PANEL_SCALE;
    for (int l = 0; l < lines; l++)
    {
        const uint16_t *src = data + l * LCD_WIDTH;
        uint16_t *row = frameBuffer + (firstRow + l * RGB_PANEL_SCALE) * RGB_PANEL_WIDTH + OUT_X;
#if RGB_PANEL_FILL
        // 4 source pixels a b c d -> 10 output pixels a a a b b c c c d d, as five
        // 32-bit writes (low half = left pixel)
        uint32_t *dst = (uint32_t*) row;
        for (int x = 0; x < LCD_WIDTH; x += 4, dst += 5)
        {
            uint32_t a = __builtin_bswap16(src[x]);
            uint32_t b = __builtin_bswap16(src[x + 1]);
            uint32_t c = __builtin_bswap16(src[x + 2]);
            uint32_t d = __builtin_bswap16(src[x + 3]);
            dst[0] = a | (a << 16);
            dst[1] = a | (b << 16);
            dst[2] = b | (c << 16);
            dst[3] = c | (c << 16);
            dst[4] = d | (d << 16);
        }
#elif RGB_PANEL_SCALE == 2
        // each pixel twice, as one 32-bit write (OUT_X is even: aligned)
        uint32_t *dst = (uint32_t*) row;
        for (int x = 0; x < LCD_WIDTH; x++)
        {
            uint32_t p = __builtin_bswap16(src[x]);
            dst[x] = p | (p << 16);
        }
#else
        for (int x = 0; x < LCD_WIDTH; x++)
        {
            uint16_t p = __builtin_bswap16(src[x]);
            for (int s = 0; s < RGB_PANEL_SCALE; s++)
            {
                row[x * RGB_PANEL_SCALE + s] = p;
            }
        }
#endif
        for (int s = 1; s < RGB_PANEL_SCALE; s++)
        {
            memcpy(row + s * RGB_PANEL_WIDTH, row, OUT_WIDTH * sizeof(uint16_t));
        }
    }
    flushRows(firstRow, lines * RGB_PANEL_SCALE);
    windowLine += lines;
}

int displayWaitOne(void)
{
    // the copy is synchronous: nothing is ever pending
    return 0;
}

void displayWaitAll(void)
{
}

void displayInit(void)
{
    static const int dataPins[16] = RGB_PANEL_DATA_PINS;
    esp_lcd_rgb_panel_config_t cfg =
    {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings =
        {
            .pclk_hz = RGB_PANEL_PCLK_HZ,
            .h_res = RGB_PANEL_WIDTH,
            .v_res = RGB_PANEL_HEIGHT,
            .hsync_pulse_width = RGB_PANEL_HSYNC_PULSE,
            .hsync_back_porch = RGB_PANEL_HSYNC_BACK_PORCH,
            .hsync_front_porch = RGB_PANEL_HSYNC_FRONT_PORCH,
            .vsync_pulse_width = RGB_PANEL_VSYNC_PULSE,
            .vsync_back_porch = RGB_PANEL_VSYNC_BACK_PORCH,
            .vsync_front_porch = RGB_PANEL_VSYNC_FRONT_PORCH,
            .flags.pclk_active_neg = RGB_PANEL_PCLK_ACTIVE_NEG,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .hsync_gpio_num = RGB_PANEL_PIN_HSYNC,
        .vsync_gpio_num = RGB_PANEL_PIN_VSYNC,
        .de_gpio_num = RGB_PANEL_PIN_DE,
        .pclk_gpio_num = RGB_PANEL_PIN_PCLK,
        .disp_gpio_num = -1,
        .flags.fb_in_psram = 1,
    };
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
    cfg.dma_burst_size = 64;
#else
    cfg.psram_trans_align = 64;
#endif
    memcpy(cfg.data_gpio_nums, dataPins, sizeof(dataPins));

    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &panel);
    if (err == ESP_OK)
    {
        err = esp_lcd_panel_reset(panel);
    }
    if (err == ESP_OK)
    {
        err = esp_lcd_panel_init(panel);
    }
    if (err == ESP_OK)
    {
        err = esp_lcd_rgb_panel_get_frame_buffer(panel, 1, (void**) &frameBuffer);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "RGB panel init failed: %s", esp_err_to_name(err));
        return;
    }
    memset(frameBuffer, 0, RGB_PANEL_HEIGHT * ROW_BYTES);
    flushRows(0, RGB_PANEL_HEIGHT);

    // backlight on once the panel shows black (boardBacklightOn() is a no-op on this board)
    if (DISPLAY_PIN_BACKLIGHT >= 0)
    {
        gpio_config_t io = { 0 };
        io.mode = GPIO_MODE_OUTPUT;
        io.pin_bit_mask = 1ULL << DISPLAY_PIN_BACKLIGHT;
        gpio_config(&io);
        vTaskDelay(pdMS_TO_TICKS(30));      // at least one refresh with the cleared buffer
        gpio_set_level(DISPLAY_PIN_BACKLIGHT, DISPLAY_BACKLIGHT_ON_LEVEL);
    }
    ESP_LOGI(TAG, "%dx%d panel, game image %dx%d at (%d, %d)", RGB_PANEL_WIDTH, RGB_PANEL_HEIGHT,
        OUT_WIDTH, OUT_HEIGHT, OUT_X, OUT_Y);
    ready = true;
}
#endif // DISPLAY_CONTROLLER == DISPLAY_CONTROLLER_RGB
