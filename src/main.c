// Bring-up test for Sunton ESP32-8048S050 (ESP32-S3, 800x480 ST7262 RGB panel)
// 1. Reports flash / PSRAM
// 2. Draws color bars, then benchmarks full-frame redraws
// 3. Mounts the microSD card, lists the root, checks for id1/pak0.pak and
//    measures sequential read speed
//
// Pin map and timings from rzeldent/platformio-espressif32-sunton (esp32-8048S050N.json)

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_flash.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"

static const char *TAG = "bringup";

// ---------- Display ----------
#define LCD_H_RES        800
#define LCD_V_RES        480
#define LCD_PCLK_HZ      (16 * 1000 * 1000)

#define PIN_BCKL         2
#define PIN_HSYNC        39
#define PIN_VSYNC        41
#define PIN_DE           40
#define PIN_PCLK         42

// esp_lcd data order for RGB565: D0-D4 = B0-B4, D5-D10 = G0-G5, D11-D15 = R0-R4.
// If red and blue come out swapped on the color bars, swap the first and last
// groups of five below.
static const int data_pins[16] = {
    8, 3, 46, 9, 1,          // B0..B4
    5, 6, 7, 15, 16, 4,      // G0..G5
    45, 48, 47, 21, 14,      // R0..R4
};

// ---------- microSD (SPI) ----------
#define PIN_SD_CS        10
#define PIN_SD_MOSI      11
#define PIN_SD_SCK       12
#define PIN_SD_MISO      13
#define MOUNT_POINT      "/sdcard"

#define RGB565(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

static esp_lcd_panel_handle_t panel;
static uint16_t *fb;

static void report_memory(void)
{
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    ESP_LOGI(TAG, "Flash: %lu MB", (unsigned long)(flash / (1024 * 1024)));
    ESP_LOGI(TAG, "PSRAM: %u KB total, %u KB free",
             (unsigned)(esp_psram_get_size() / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    ESP_LOGI(TAG, "Internal RAM free: %u KB (largest block %u KB)",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

static void display_init(void)
{
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_PLL160M,
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags = {
                .hsync_idle_low = 0,
                .vsync_idle_low = 0,
                .de_idle_high = 0,
                .pclk_active_neg = 1,
                .pclk_idle_high = 0,
            },
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        // Bounce buffers in internal RAM keep the picture stable while the CPU
        // is hammering PSRAM (which Quake will do constantly).
        .bounce_buffer_size_px = LCD_H_RES * 10,
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = PIN_HSYNC,
        .vsync_gpio_num = PIN_VSYNC,
        .de_gpio_num = PIN_DE,
        .pclk_gpio_num = PIN_PCLK,
        .disp_gpio_num = -1,
        .flags = {
            .fb_in_psram = 1,
        },
    };
    memcpy(cfg.data_gpio_nums, data_pins, sizeof(data_pins));

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel, 1, (void **)&fb));

    // Backlight on only after the panel is running, so you never see garbage.
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << PIN_BCKL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(PIN_BCKL, 1);
    ESP_LOGI(TAG, "Display up, frame buffer at %p", fb);
}

// Push the frame buffer: when the pointer IS the driver's frame buffer,
// draw_bitmap just writes back the CPU cache so the DMA sees new pixels.
static void present(void)
{
    esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_H_RES, LCD_V_RES, fb);
}

static void draw_color_bars(void)
{
    // Expected left to right: white, yellow, cyan, green, magenta, red, blue, black
    const uint16_t bars[8] = {
        RGB565(255, 255, 255), RGB565(255, 255, 0), RGB565(0, 255, 255), RGB565(0, 255, 0),
        RGB565(255, 0, 255),   RGB565(255, 0, 0),   RGB565(0, 0, 255),   RGB565(0, 0, 0),
    };
    for (int y = 0; y < LCD_V_RES; y++) {
        uint16_t *row = fb + y * LCD_H_RES;
        for (int x = 0; x < LCD_H_RES; x++) {
            if (y >= LCD_V_RES - 40) {
                // Bottom strip: grayscale ramp to check bit depth / banding
                uint8_t v = x * 255 / (LCD_H_RES - 1);
                row[x] = RGB565(v, v, v);
            } else {
                row[x] = bars[x / (LCD_H_RES / 8)];
            }
        }
    }
    // 1-pixel border so you can confirm nothing is cropped or shifted
    for (int x = 0; x < LCD_H_RES; x++) {
        fb[x] = fb[(LCD_V_RES - 1) * LCD_H_RES + x] = RGB565(255, 0, 0);
    }
    for (int y = 0; y < LCD_V_RES; y++) {
        fb[y * LCD_H_RES] = fb[y * LCD_H_RES + LCD_H_RES - 1] = RGB565(255, 0, 0);
    }
    present();
}

// How fast can we fill a full 800x480 frame? Rough ceiling for Quake's
// final blit (palette lookup + scale) before any rendering cost.
static void benchmark_fill(void)
{
    const int frames = 60;
    int64_t t0 = esp_timer_get_time();
    for (int f = 0; f < frames; f++) {
        uint16_t c = RGB565((f * 4) & 0xFF, 64, 255 - ((f * 4) & 0xFF));
        uint32_t c2 = ((uint32_t)c << 16) | c;
        uint32_t *p = (uint32_t *)fb;
        for (int i = 0; i < LCD_H_RES * LCD_V_RES / 2; i++) p[i] = c2;
        present();
    }
    int64_t us = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "Full-frame fill: %.1f fps (%.2f ms/frame)",
             frames * 1e6 / us, us / 1000.0 / frames);
}

static void sd_test(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000;   // drop to SDMMC_FREQ_DEFAULT if mount fails

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return;
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = host.slot;

    sdmmc_card_t *card;
    err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s (card inserted? FAT32/exFAT?)", esp_err_to_name(err));
        return;
    }
    sdmmc_card_print_info(stdout, card);

    ESP_LOGI(TAG, "Root of SD card:");
    DIR *d = opendir(MOUNT_POINT);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        printf("  %s%s\n", e->d_name, e->d_type == DT_DIR ? "/" : "");
    }
    if (d) closedir(d);

    const char *pak = MOUNT_POINT "/id1/pak0.pak";
    struct stat st;
    if (stat(pak, &st) != 0) {
        ESP_LOGW(TAG, "%s not found (copy it over when you're ready)", pak);
        return;
    }
    ESP_LOGI(TAG, "Found pak0.pak: %ld bytes", (long)st.st_size);

    // Sequential read speed (first 4 MB) - this is what level loads will feel like
    FILE *f = fopen(pak, "rb");
    if (!f) return;
    const size_t chunk = 32 * 1024;
    uint8_t *buf = heap_caps_malloc(chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    size_t total = 0;
    int64_t t0 = esp_timer_get_time();
    while (total < 4 * 1024 * 1024) {
        size_t n = fread(buf, 1, chunk, f);
        if (n == 0) break;
        total += n;
    }
    int64_t us = esp_timer_get_time() - t0;
    fclose(f);
    free(buf);
    ESP_LOGI(TAG, "SD read: %u KB in %.2f s = %.0f KB/s",
             (unsigned)(total / 1024), us / 1e6, total / 1024.0 / (us / 1e6));
}

void app_main(void)
{
    report_memory();
    display_init();
    draw_color_bars();
    vTaskDelay(pdMS_TO_TICKS(3000));
    benchmark_fill();
    draw_color_bars();
    sd_test();
    report_memory();
    ESP_LOGI(TAG, "Bring-up done. Color bars should be on screen.");
}
