/**
 * @file esp_update.h
 * @brief Wi-Fi update mode (esp32-quake): Wi-Fi setup through the QUAKE-SETUP
 * access point, firmware upload from a browser or curl, rollback safety net.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#ifndef ESP_UPDATE_H
#define ESP_UPDATE_H
#include <stdbool.h>

// Called at boot, after the display is up. True if update mode must run instead of
// the game: new firmware waiting to be confirmed, a restart into update mode was
// requested, or BOOT / a pad button is held during the short window it offers.
bool updateModeRequested(void);
// Runs update mode. Never returns: it restarts into the game or the new firmware.
void updateModeRun(void);
#endif
