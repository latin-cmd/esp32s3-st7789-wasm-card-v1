# ESP32-S3 ST7789 WASM Card V1

This version keeps the display, USB transport, storage, and safety boundaries
in native ESP-IDF code. A browser page served by the computer prepares a
240x280 image and uploads an independent WebAssembly rendering plugin over USB.

## Use

1. Flash with ESP-IDF 5.3.2.
2. Run `host/serve.ps1` and open `http://localhost:8765` in Chrome or Edge.
3. Click **连接 ESP32** and select the board's USB serial port.
4. Select an image, adjust crop/zoom/rotation, and upload it.
5. Optionally upload a `.wasm` plugin built against `plugins/README.md`.

The image and plugin are saved in LittleFS and restored after restart. If
either file is absent or invalid, the compiled-in LVGL business card remains
available as a fallback. Touch support is disabled in this version.

## Recovery and safety

- The USB protocol starts before any saved content is restored, so a bad image
  or plugin can always be replaced, or cleared with **RESET_CONTENT**.
- If the saved plugin did not finish loading on the previous boot (crash,
  watchdog, or power cut), it is moved to `/data/plugin.wasm.bad` and the
  device boots with the image or the built-in card.
- Hold the **BOOT** button (GPIO0) while the device starts to skip the saved
  plugin and image for that boot.
- Storage, NVS, or plugin-runtime failures are logged and never stop the
  built-in card from showing.
- Crashes are saved to the `coredump` partition. Read them with
  `idf.py -B build-v5.3.2 -p COM10 coredump-info`.

Plugin limits are listed in `plugins/README.md`.

## Build and flash

```powershell
& D:\TheWork\espressif\esp-idf-v5.3.2\export.ps1
idf.py -B build-v5.3.2 build
idf.py -B build-v5.3.2 -p COM10 flash monitor
```
