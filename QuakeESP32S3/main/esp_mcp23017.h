/**
 * @file esp_mcp23017.h
 * @brief MCP23017 16-bit I2C GPIO expander used as a button pad (esp32-quake).
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#ifndef ESP_MCP23017_H
#define ESP_MCP23017_H
#include <stdint.h>
#include <stdbool.h>

// Starts a background task (core 0) that polls all 16 pins every few ms. All pins
// are inputs with pull-ups: a button connects a pin to GND. The chip may be absent
// or plugged in later: the task keeps probing for it. Calling it again does nothing.
void mcp23017Start(int sdaPin, int sclPin, uint8_t address, uint32_t clockHz);
// Debounced pressed buttons: bit 0..7 = GPA0..GPA7, bit 8..15 = GPB0..GPB7
uint16_t mcp23017GetPressed(void);
// True once the chip has answered
bool mcp23017IsPresent(void);
#endif
