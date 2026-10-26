/**
 * @file esp_board_sunton.c
 * @brief Sunton ESP32-8048S050 board support (esp32-quake). The display (and its
 * backlight) is handled in esp_display_rgb.c; there is no touch controller on the
 * N version. Buttons: BOOT (GPIO0, menu) and an optional MCP23017 I2C button pad
 * (see board_config.h for the wiring and the button map).
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#include "esp_board.h"
#include "board_config.h"

#if BOARD == BOARD_SUNTON_8048S050
#include "driver/gpio.h"
#include "quakedef.h"
#include "esp_input.h"
#include "esp_mcp23017.h"

void boardInit(void)
{
    // keep the SD card deselected until its driver takes over the pin
    gpio_config_t io = { 0 };
    io.mode = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = 1ULL << SD_PIN_CS;
    gpio_config(&io);
    gpio_set_level(SD_PIN_CS, 1);
}

void boardBacklightOn(void)
{
}

void boardAudioCodecInit(void)
{
}

bool boardWakeButtonPressed(void)
{
    return false;
}

bool boardHasTouch(void)
{
    return false;
}

int boardTouchRead(boardTouchPoint_t *points)
{
    (void) points;
    return -1;
}

#if BUTTON_PAD_MCP23017
// Button names used by MCP23017_BUTTON_MAP (board_config.h): virtual gamepad
// buttons (esp_input.c turns them into Quake keys), or special functions (< 0).
#define BTN_UP              GP_UP           // forward / menu up
#define BTN_DOWN            GP_DOWN         // back / menu down
#define BTN_LEFT            GP_LEFT         // turn left
#define BTN_RIGHT           GP_RIGHT        // turn right
#define BTN_FIRE            GP_RT           // fire / menu select
#define BTN_JUMP            GP_A            // jump, swim up / menu select
#define BTN_WEAPON          GP_X            // next weapon
#define BTN_MENU            GP_START        // menu / back
#define BTN_STRAFE_LEFT     GP_LB
#define BTN_STRAFE_RIGHT    GP_RB
#define BTN_RUN             GP_LT           // hold to run
#define BTN_LOOK_UP         GP_L3
#define BTN_LOOK_DOWN       GP_R3
#define BTN_SWIM_DOWN       GP_B            // swim down / menu back
#define BTN_SCORES          GP_Y
#define BTN_CONSOLE         GP_SELECT
#define BTN_NONE            (-1)
#define BTN_VOLUME_UP       (-2)
#define BTN_VOLUME_DOWN     (-3)
#define VOLUME_STEP         0.1f

static const int8_t buttonMap[16] = MCP23017_BUTTON_MAP;

void boardInputInit(void)
{
    mcp23017Start(MCP23017_I2C_SDA, MCP23017_I2C_SCL, MCP23017_I2C_ADDRESS, MCP23017_I2C_HZ);
}

static void changeVolume(float delta)
{
    float v = Cvar_VariableValue("volume") + delta;
    v = v < 0 ? 0 : (v > 1 ? 1 : v);
    v = (int) (v * 10 + 0.5f) / 10.0f;      // keep it on the 10% steps
    Cvar_SetValue("volume", v);
    Con_Printf("Volume %d%%\n", (int) (v * 100 + 0.5f));
}

// Game task, every frame
uint32_t boardGetGamepadButtons(void)
{
    static uint16_t oldPressed;
    uint16_t pressed = mcp23017GetPressed();
    uint16_t newlyPressed = pressed & ~oldPressed;
    oldPressed = pressed;
    uint32_t gp = 0;
    for (int i = 0; i < 16; i++)
    {
        int b = buttonMap[i];
        if (b >= 0 && (pressed & (1 << i)))
        {
            gp |= 1u << b;
        }
        else if (newlyPressed & (1 << i))
        {
            if (b == BTN_VOLUME_UP)
            {
                changeVolume(VOLUME_STEP);
            }
            else if (b == BTN_VOLUME_DOWN)
            {
                changeVolume(-VOLUME_STEP);
            }
        }
    }
    return gp;
}
#endif // BUTTON_PAD_MCP23017
#endif // BOARD == BOARD_SUNTON_8048S050
