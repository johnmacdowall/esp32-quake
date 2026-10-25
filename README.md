# esp32-quake

Quake on the Sunton ESP32-8048S050 (ESP32-S3 N16R8, 5" 800x480 RGB IPS panel).

| Folder | What it is |
|---|---|
| `bringup/` | Board bring-up test: display color bars, PSRAM, microSD (passed) |
| `QuakeESP32S3/` | The Quake port: [recrof/Quake-ESP32S3](https://github.com/recrof/Quake-ESP32S3) (MG24Quake by Nicola Wrachien, based on id Software's WinQuake) plus an 800x480 RGB panel driver for this board |

Each folder is its own PlatformIO project: open the folder itself in VS Code.

## Status
- [x] Board bring-up: display, PSRAM, microSD
- [ ] Quake port: first boot on the 8048S050
- [ ] Button input PCB
- [ ] 3D-printed case

## Running Quake
1. Convert the game data. Put `pak0.pak` (shareware 1.06 or retail; **not** the 2021
   re-release) in a folder with `QuakeESP32S3/tools/MCUPackConverter/bin/Debug/MCUPackConverter.exe`
   and run it. It writes `PAK0Conv.PAK`.
2. Copy `PAK0Conv.PAK` to the **root** of the FAT32 microSD card, renamed to `PAK0.PAK`.
3. Open `QuakeESP32S3/` in PlatformIO: Build, Upload, Monitor.
4. Controls for now: BOOT button = menu; a Bluetooth LE gamepad or keyboard to play.
   With no controller the demo loop plays by itself.

Game data is never committed to this repo (see `.gitignore`).

## Board notes
| Function | GPIO |
|---|---|
| Backlight | 2 |
| HSYNC / VSYNC / DE / PCLK | 39 / 41 / 40 / 42 |
| B0-B4 | 8, 3, 46, 9, 1 |
| G0-G5 | 5, 6, 7, 15, 16, 4 |
| R0-R4 | 45, 48, 47, 21, 14 |
| microSD CS / MOSI / SCK / MISO | 10 / 11 / 12 / 13 |
| Serial console (CH340) | 43 / 44 |
| Free for buttons | 17, 18, 19, 20, 38 |

Measured: full 800x480 frame write to PSRAM ~26 ms; SD (SPI, 10 MHz) ~480 KB/s.

## License
`QuakeESP32S3/` is GPL (see `QuakeESP32S3/LICENSE`); the upstream commit it is based on is in
`QuakeESP32S3/UPSTREAM_COMMIT`.
