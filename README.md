# esp32-quake

Quake on the Sunton ESP32-8048S050 (ESP32-S3 N16R8, 5" 800x480 RGB IPS panel).

| Folder | What it is |
|---|---|
| `bringup/` | Board bring-up test: display color bars, PSRAM, microSD (passed) |
| `QuakeESP32S3/` | The Quake port: [recrof/Quake-ESP32S3](https://github.com/recrof/Quake-ESP32S3) (MG24Quake by Nicola Wrachien, based on id Software's WinQuake) plus an 800x480 RGB panel driver for this board |

Each folder is its own PlatformIO project: open the folder itself in VS Code.

## Status
- [x] Board bring-up: display, PSRAM, microSD
- [x] Quake port running on the 8048S050: 640x480 (4:3, black bars), stable, no tearing
- [x] Flash + PSRAM at 120 MHz (passed a 15+ minute warm soak)
- [ ] Sound: onboard NS4168 I2S amp on P6 "Speak" (I2S clock pin GPIO0 vs GPIO19 to confirm)
- [ ] Buttons: MCP23017 I2C expander (P3: IO19 SDA, IO20 SCL; P4: 3.3V, GND)
- [x] Wi-Fi firmware updates with automatic rollback (update mode)
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

## Wi-Fi updates
Wi-Fi only runs in **update mode**, so the game keeps all its RAM. Enter it by pressing BOOT
(or any pad button) during the short "Press BOOT..." prompt at power-on.

- **Setup:** with no Wi-Fi stored, the board opens the network `QUAKE-SETUP`
  (password `quakesetup`). Join it from a phone, open http://192.168.20.1/, enter the home
  Wi-Fi details and choose an update password. Nothing secret is built into the firmware.
- **Updating:** on the home network the screen shows the board's address. Open it in a
  browser to upload `QuakeESP32S3/.pio/build/esp32-8048S050/firmware.bin`, or:
  `curl -u quake:PASSWORD -H "Expect:" --data-binary @firmware.bin http://quake.lan/update`
  A progress bar shows on the page and on the handheld.
- **Safety net:** new firmware starts in update mode and confirms itself once its Wi-Fi is up.
  If it crashes or is reset before that, the bootloader returns to the previous firmware.
  After confirming, the game starts by itself after 15 s.
- **quake.lan:** the board's DHCP host name is `quake`; with pfSense's "Register DHCP leases
  in the DNS Resolver" on, http://quake.lan/ reaches it.
- Uploads are checked: only a Quake `firmware.bin` for this project is accepted.
- The first firmware with Wi-Fi updates changes the partition table: flash it over USB
  (delete `sdkconfig.esp32-8048S050`, Full Clean, Upload). Savegames start over once.

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

Board revision: V1.3 (printed next to the microSD slot). Speaker amplifier: NS4168 (U4).
Connectors are 1.25 mm pitch (Molex PicoBlade / "MX1.25").

## Performance (shareware demo loop, fps printed once a second)
| Configuration | fps |
|---|---|
| PSRAM frame buffer, 80 MHz | 22-27 (picture shifted / tore) |
| Bounce buffers + triple-buffered 320x240 images, 80 MHz | 29-42 |
| Same, flash + PSRAM at 120 MHz | 33-48 (about +15%) |

Other measurements: SD card (SPI, 20 MHz) mounts; ~480 KB/s at 10 MHz in the bring-up test.

## Display driver notes (QuakeESP32S3/main/esp_display_rgb.c)
- No 800x480 frame buffer. The LCD runs in bounce buffer mode: two 8-line buffers in
  internal RAM, refilled in the LCD interrupt by scaling the current 320x240 image (PSRAM).
- Three 320x240 images rotate: the game never draws into the one on screen (no tearing).
- `CONFIG_LCD_RGB_RESTART_IN_VSYNC` must stay off: in bounce buffer mode it makes the
  picture jump vertically.
- Changing `sdkconfig.defaults` needs `sdkconfig.esp32-8048S050` deleted + Full Clean.

## License
`QuakeESP32S3/` is GPL (see `QuakeESP32S3/LICENSE`); the upstream commit it is based on is in
`QuakeESP32S3/UPSTREAM_COMMIT`.
