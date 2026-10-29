/**
 * @file esp_mcp23017.c
 * @brief MCP23017 16-bit I2C GPIO expander used as a button pad (esp32-quake).
 *
 * Registers are used in the power-on layout (IOCON.BANK = 0: A and B registers
 * interleaved, address auto-increment on).
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#include "esp_mcp23017.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "hal/usb_serial_jtag_ll.h"
#endif

#define REG_IODIRA          0x00    // 1 = input
#define REG_IPOLA           0x02    // 1 = inverted (pressed reads as 1)
#define REG_IOCON           0x0A
#define REG_GPPUA           0x0C    // 1 = 100k pull-up
#define REG_GPIOA           0x12
#define POLL_MS             5
#define RETRY_MS            1000
#define I2C_TIMEOUT_MS      20
#define TASK_PRIORITY       3
#define TASK_CORE           0

static const char *TAG = "mcp23017";
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t dev;
static uint8_t devAddress;
static volatile uint16_t pressed;
static volatile bool present;

uint16_t mcp23017GetPressed(void)
{
    return pressed;
}

const char *mcp23017PinName(int bit)
{
    static const char *const names[16] =
    {
        "GPA0 (pin 21)", "GPA1 (pin 22)", "GPA2 (pin 23)", "GPA3 (pin 24)",
        "GPA4 (pin 25)", "GPA5 (pin 26)", "GPA6 (pin 27)", "GPA7 (pin 28)",
        "GPB0 (pin 1)", "GPB1 (pin 2)", "GPB2 (pin 3)", "GPB3 (pin 4)",
        "GPB4 (pin 5)", "GPB5 (pin 6)", "GPB6 (pin 7)", "GPB7 (pin 8)",
    };
    return bit >= 0 && bit < 16 ? names[bit] : "?";
}

// one log line per button change: makes checking the wiring easy
static void logChanges(uint16_t before, uint16_t after)
{
    uint16_t changed = before ^ after;
    for (int i = 0; i < 16; i++)
    {
        if (changed & (1 << i))
        {
            ESP_LOGI(TAG, "buttons: %s %s", mcp23017PinName(i), (after & (1 << i)) ? "down" : "up");
        }
    }
}

bool mcp23017IsPresent(void)
{
    return present;
}

static bool configure(void)
{
    // all pins inputs, inverted (a pressed button reads 1), pull-ups on
    const uint8_t iocon[] = { REG_IOCON, 0x00 };
    const uint8_t dirPol[] = { REG_IODIRA, 0xFF, 0xFF, 0xFF, 0xFF };     // IODIRA, IODIRB, IPOLA, IPOLB
    const uint8_t pullUps[] = { REG_GPPUA, 0xFF, 0xFF };                  // GPPUA, GPPUB
    return i2c_master_transmit(dev, iocon, sizeof(iocon), I2C_TIMEOUT_MS) == ESP_OK
        && i2c_master_transmit(dev, dirPol, sizeof(dirPol), I2C_TIMEOUT_MS) == ESP_OK
        && i2c_master_transmit(dev, pullUps, sizeof(pullUps), I2C_TIMEOUT_MS) == ESP_OK;
}

static bool readPins(uint16_t *pins)
{
    uint8_t reg = REG_GPIOA, v[2];
    if (i2c_master_transmit_receive(dev, &reg, 1, v, 2, I2C_TIMEOUT_MS) != ESP_OK)
    {
        return false;
    }
    *pins = v[0] | (v[1] << 8);
    return true;
}

static void pollTask(void *arg)
{
    (void) arg;
    bool reported = false;
    uint16_t last = 0;
    while (1)
    {
        if (!present)
        {
            uint16_t first;
            if (i2c_master_probe(bus, devAddress, I2C_TIMEOUT_MS) == ESP_OK && configure() && readPins(&first))
            {
                ESP_LOGI(TAG, "button expander found at 0x%02X", devAddress);
                // no debouncing for the very first state: a button held at start is seen at once
                logChanges(0, first);
                pressed = first;
                last = first;
                present = true;
                vTaskDelay(pdMS_TO_TICKS(POLL_MS));
                continue;
            }
            else
            {
                if (!reported)
                {
                    ESP_LOGW(TAG, "no button expander at 0x%02X (check wiring, RESET pin to 3.3V); retrying", devAddress);
                    reported = true;
                }
                vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
                continue;
            }
        }
        uint16_t pins;
        if (!readPins(&pins))
        {
            // unplugged or bus error: release everything and probe again
            ESP_LOGW(TAG, "button expander stopped answering");
            logChanges(pressed, 0);
            present = false;
            pressed = 0;
            last = 0;
            reported = false;
            continue;
        }
        // debounce: a pin changes state after two identical reads in a row
        uint16_t stable = ~(pins ^ last);
        uint16_t now = (pressed & ~stable) | (pins & stable);
        if (now != pressed)
        {
            logChanges(pressed, now);
        }
        pressed = now;
        last = pins;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void mcp23017Start(int sdaPin, int sclPin, uint8_t address, uint32_t clockHz)
{
    static bool started;
    if (started)
    {
        return;     // already running (update mode check at boot, then the game)
    }
    started = true;
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    // GPIO19/20 are the native USB pins: the USB PHY pads must be released
    // before they can carry I2C (the console uses the CH340 on UART0).
    if ((sdaPin == 19 || sdaPin == 20) || (sclPin == 19 || sclPin == 20))
    {
        usb_serial_jtag_ll_phy_enable_pad(false);
    }
#endif
    i2c_master_bus_config_t busCfg =
    {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sdaPin,
        .scl_io_num = sclPin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   // weak: fit 4.7k pull-ups on SDA and SCL
    };
    if (i2c_new_master_bus(&busCfg, &bus) != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C bus init failed (SDA %d, SCL %d)", sdaPin, sclPin);
        return;
    }
    i2c_device_config_t devCfg =
    {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = clockHz,
    };
    if (i2c_master_bus_add_device(bus, &devCfg, &dev) != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C device add failed");
        return;
    }
    devAddress = address;
    // stack in PSRAM: internal RAM is scarce
    xTaskCreatePinnedToCoreWithCaps(pollTask, "buttons", 3072, NULL, TASK_PRIORITY, NULL, TASK_CORE, MALLOC_CAP_SPIRAM);
}
