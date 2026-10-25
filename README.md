# esp32-quake

Quake on the Sunton ESP32-8048S050 (ESP32-S3 N16R8, 5" 800x480 RGB IPS panel).

## Status
- [ ] Board bring-up: display, PSRAM, microSD (`src/main.c`)
- [ ] Button input PCB
- [ ] Quake engine port
- [ ] 3D-printed case

## Build
PlatformIO (VS Code) with the ESP-IDF framework. Open this folder in PlatformIO, then Build / Upload / Monitor.

## Hardware notes
| Function | GPIO |
|---|---|
| Backlight | 2 |
| HSYNC / VSYNC / DE / PCLK | 39 / 41 / 40 / 42 |
| B0-B4 | 8, 3, 46, 9, 1 |
| G0-G5 | 5, 6, 7, 15, 16, 4 |
| R0-R4 | 45, 48, 47, 21, 14 |
| microSD CS / MOSI / SCK / MISO | 10 / 11 / 12 / 13 |

Pin map from [rzeldent/platformio-espressif32-sunton](https://github.com/rzeldent/platformio-espressif32-sunton).
Game data (`id1/pak0.pak`) goes on the microSD card; it is not included in this repo.
