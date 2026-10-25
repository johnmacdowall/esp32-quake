/**
 * @file esp_board_sunton.c
 * @brief Sunton ESP32-8048S050 board support. The display (and its backlight)
 * is handled in esp_display_rgb.c; there is no touch controller on the N
 * version, no audio codec, and no buttons beyond BOOT (GPIO0, menu).
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
#endif
