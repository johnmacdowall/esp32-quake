/**
 * @file esp_display_rgb.c
 * @brief Display driver for 16-bit parallel RGB panels (Sunton ESP32-8048S050,
 * 800x480 ST7262), implementing the display.h interface used by esp_graphics.c.
 *
 * There is no 800x480 frame buffer. The game's finished 320x240 RGB565 image is
 * kept in PSRAM (three of them, rotated: the game never draws into the image on
 * screen, so there is no tearing). The LCD driver runs in bounce buffer mode:
 * its DMA always reads two small buffers in internal RAM, and whenever one has
 * been sent, onBounceEmpty() (in the LCD interrupt, core 0) fills it with the
 * next few panel lines, scaling the current image up on the fly.
 *
 * Compared with a PSRAM frame buffer: the LCD DMA can no longer be starved by
 * the game's PSRAM traffic (no sideways slips), PSRAM traffic for the display
 * drops about 4x (150 kB read per refresh instead of 768 kB, 150 kB written per
 * game frame instead of 614 kB), and the 768 kB frame buffer is freed for the
 * PAK cache.
 *
 * The rest of the port still produces 320x240 RGB565 lines in big endian, as
 * SPI panels want them: they are byte-swapped when stored.
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
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "sdkconfig.h"

#ifndef RGB_PANEL_FILL
#define RGB_PANEL_FILL  0
#endif
#if RGB_PANEL_FILL
// Full screen: 320 -> 800 columns (x2.5: pixels repeated 3, 2, 3, 2... times),
// 240 -> 480 rows (x2). The image is 25% wider than Quake's 4:3.
#if LCD_WIDTH * 5 / 2 != RGB_PANEL_WIDTH || LCD_HEIGHT * 2 != RGB_PANEL_HEIGHT
#error "RGB_PANEL_FILL needs an 800x480 panel"
#endif
#define SCALE_Y         2
#define OUT_WIDTH       RGB_PANEL_WIDTH
#else
#if RGB_PANEL_SCALE < 1 || LCD_WIDTH * RGB_PANEL_SCALE > RGB_PANEL_WIDTH || LCD_HEIGHT * RGB_PANEL_SCALE > RGB_PANEL_HEIGHT
#error "RGB_PANEL_SCALE too large for the panel"
#endif
#define SCALE_Y         RGB_PANEL_SCALE
#define OUT_WIDTH       (LCD_WIDTH * RGB_PANEL_SCALE)
#endif
#define OUT_HEIGHT      (LCD_HEIGHT * SCALE_Y)
#define OUT_X           ((RGB_PANEL_WIDTH - OUT_WIDTH) / 2)
#define OUT_Y           ((RGB_PANEL_HEIGHT - OUT_HEIGHT) / 2)

#ifndef RGB_PANEL_BOUNCE_LINES
#define RGB_PANEL_BOUNCE_LINES  4       // panel lines per bounce buffer (x2 buffers, internal RAM)
#endif
#if (RGB_PANEL_HEIGHT % RGB_PANEL_BOUNCE_LINES) != 0
#error "RGB_PANEL_BOUNCE_LINES must divide RGB_PANEL_HEIGHT"
#endif
#if (OUT_X % 2) != 0
#error "the image must start on an even column (32-bit writes)"
#endif

#define NUM_IMAGES      3
#define IMAGE_PIXELS    (LCD_WIDTH * LCD_HEIGHT)

static const char *TAG = "display_rgb";
static esp_lcd_panel_handle_t panel;
static uint16_t *images[NUM_IMAGES];        // 320x240 RGB565, native byte order, PSRAM
static portMUX_TYPE imageLock = portMUX_INITIALIZER_UNLOCKED;
static volatile int shownImage;             // being sent to the panel
static volatile int pendingImage = -1;      // complete, shown from the next refresh
static int latestImage;                     // most recent complete image
static int drawImage = -1;                  // being written by the game, -1 = none
static int windowLine;                      // next 320x240 line to be written
static int windowFirst, windowEnd;          // lines written in this frame
static bool ready;

bool displayIsReady(void)
{
    return ready;
}

// LCD interrupt (core 0): fill a bounce buffer with the panel lines starting at pixel posPx
static bool onBounceEmpty(esp_lcd_panel_handle_t p, void *bounceBuf, int posPx, int lenBytes, void *ctx)
{
    (void) p;
    (void) ctx;
    if (posPx == 0)
    {
        // start of a refresh: switch to the newest complete image
        portENTER_CRITICAL_ISR(&imageLock);
        if (pendingImage >= 0)
        {
            shownImage = pendingImage;
            pendingImage = -1;
        }
        portEXIT_CRITICAL_ISR(&imageLock);
    }
    const uint16_t *image = images[shownImage];
    int firstRow = posPx / RGB_PANEL_WIDTH;
    int rows = lenBytes / (RGB_PANEL_WIDTH * (int) sizeof(uint16_t));
    for (int r = 0; r < rows; r++)
    {
        int y = firstRow + r;
        uint32_t *out = (uint32_t*) bounceBuf + r * (RGB_PANEL_WIDTH / 2);
        if (y < OUT_Y || y >= OUT_Y + OUT_HEIGHT)
        {
            memset(out, 0, RGB_PANEL_WIDTH * sizeof(uint16_t));
            continue;
        }
        const uint16_t *src = image + ((y - OUT_Y) / SCALE_Y) * LCD_WIDTH;
        // black bars (if any)
        for (int i = 0; i < OUT_X / 2; i++)
        {
            out[i] = 0;
            out[RGB_PANEL_WIDTH / 2 - 1 - i] = 0;
        }
        uint32_t *dst = out + OUT_X / 2;
#if RGB_PANEL_FILL
        // 4 source pixels a b c d -> a a a b b c c c d d (five 32-bit words, low half = left pixel)
        for (int x = 0; x < LCD_WIDTH; x += 4, dst += 5)
        {
            uint32_t a = src[x], b = src[x + 1], c = src[x + 2], d = src[x + 3];
            dst[0] = a | (a << 16);
            dst[1] = a | (b << 16);
            dst[2] = b | (c << 16);
            dst[3] = c | (c << 16);
            dst[4] = d | (d << 16);
        }
#elif RGB_PANEL_SCALE == 2
        // two source pixels at a time -> four output pixels
        const uint32_t *src2 = (const uint32_t*) src;
        for (int x = 0; x < LCD_WIDTH / 2; x++, dst += 2)
        {
            uint32_t ab = src2[x];
            uint32_t a = ab & 0xFFFF, b = ab >> 16;
            dst[0] = a | (a << 16);
            dst[1] = b | (b << 16);
        }
#else
        uint16_t *dst16 = (uint16_t*) dst;
        for (int x = 0; x < LCD_WIDTH; x++)
        {
            for (int s = 0; s < RGB_PANEL_SCALE; s++)
            {
                *dst16++ = src[x];
            }
        }
#endif
    }
    return false;
}

void displaySetWindow(int firstLine, int numLines)
{
    windowLine = firstLine;
    windowFirst = firstLine;
    windowEnd = firstLine + numLines;
    if (drawImage < 0)
    {
        // draw into an image that is neither on screen nor waiting to be shown
        portENTER_CRITICAL(&imageLock);
        int busy1 = shownImage;
        int busy2 = pendingImage >= 0 ? pendingImage : latestImage;
        portEXIT_CRITICAL(&imageLock);
        for (int i = 0; i < NUM_IMAGES; i++)
        {
            if (i != busy1 && i != busy2)
            {
                drawImage = i;
                break;
            }
        }
    }
}

void displaySendPixels(const uint16_t *data, size_t numPixels, bool last)
{
    if (drawImage < 0)
    {
        return;
    }
    uint16_t *image = images[drawImage];
    int lines = numPixels / LCD_WIDTH;
    if (windowLine + lines > LCD_HEIGHT)
    {
        lines = LCD_HEIGHT - windowLine;
    }
    // byte swap (big endian SPI order -> native), two pixels at a time
    const uint32_t *src = (const uint32_t*) data;
    uint32_t *dst = (uint32_t*) (image + windowLine * LCD_WIDTH);
    for (int i = 0; i < lines * LCD_WIDTH / 2; i++)
    {
        uint32_t p = src[i];
        dst[i] = ((p & 0x00FF00FF) << 8) | ((p >> 8) & 0x00FF00FF);
    }
    windowLine += lines;
    if (last)
    {
        // lines this frame did not draw (status bar, when unchanged) come from the latest image
        if (windowFirst > 0)
        {
            memcpy(image, images[latestImage], windowFirst * LCD_WIDTH * sizeof(uint16_t));
        }
        if (windowEnd < LCD_HEIGHT)
        {
            memcpy(image + windowEnd * LCD_WIDTH, images[latestImage] + windowEnd * LCD_WIDTH,
                (LCD_HEIGHT - windowEnd) * LCD_WIDTH * sizeof(uint16_t));
        }
        portENTER_CRITICAL(&imageLock);
        pendingImage = drawImage;
        portEXIT_CRITICAL(&imageLock);
        latestImage = drawImage;
        drawImage = -1;
    }
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
    for (int i = 0; i < NUM_IMAGES; i++)
    {
        images[i] = heap_caps_calloc(IMAGE_PIXELS, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!images[i])
        {
            ESP_LOGE(TAG, "no PSRAM for the screen images");
            return;
        }
    }
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
        .num_fbs = 0,
        .bounce_buffer_size_px = RGB_PANEL_WIDTH * RGB_PANEL_BOUNCE_LINES,
        .hsync_gpio_num = RGB_PANEL_PIN_HSYNC,
        .vsync_gpio_num = RGB_PANEL_PIN_VSYNC,
        .de_gpio_num = RGB_PANEL_PIN_DE,
        .pclk_gpio_num = RGB_PANEL_PIN_PCLK,
        .disp_gpio_num = -1,
        .flags.no_fb = 1,
    };
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
    cfg.dma_burst_size = 64;
#else
    cfg.sram_trans_align = 4;
#endif
    memcpy(cfg.data_gpio_nums, dataPins, sizeof(dataPins));

    esp_lcd_rgb_panel_event_callbacks_t callbacks = { 0 };
    callbacks.on_bounce_empty = onBounceEmpty;
    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &panel);
    if (err == ESP_OK)
    {
        err = esp_lcd_rgb_panel_register_event_callbacks(panel, &callbacks, NULL);
    }
    if (err == ESP_OK)
    {
        err = esp_lcd_panel_reset(panel);
    }
    if (err == ESP_OK)
    {
        err = esp_lcd_panel_init(panel);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "RGB panel init failed: %s", esp_err_to_name(err));
        return;
    }

    // backlight on once the panel shows the (black) image
    if (DISPLAY_PIN_BACKLIGHT >= 0)
    {
        gpio_config_t io = { 0 };
        io.mode = GPIO_MODE_OUTPUT;
        io.pin_bit_mask = 1ULL << DISPLAY_PIN_BACKLIGHT;
        gpio_config(&io);
        vTaskDelay(pdMS_TO_TICKS(30));      // at least one refresh
        gpio_set_level(DISPLAY_PIN_BACKLIGHT, DISPLAY_BACKLIGHT_ON_LEVEL);
    }
    ESP_LOGI(TAG, "%dx%d panel, game image %dx%d at (%d, %d), bounce buffers %d lines",
        RGB_PANEL_WIDTH, RGB_PANEL_HEIGHT, OUT_WIDTH, OUT_HEIGHT, OUT_X, OUT_Y, RGB_PANEL_BOUNCE_LINES);
    ready = true;
}
#endif // DISPLAY_CONTROLLER == DISPLAY_CONTROLLER_RGB
