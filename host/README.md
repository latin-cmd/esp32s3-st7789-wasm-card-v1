# Computer-side Card Studio

Run `serve.ps1`, open `http://localhost:8765` in Chrome or Edge, click
**连接 ESP32**, and select the board's USB serial port. Close ESP-IDF Monitor
first because only one program can own the serial port at a time.

The page uses the browser Web Serial API. It converts images locally to a
240x280 RGB565 frame stored MSB-first (the byte order required by this
LVGL build) and transfers images or independent `.wasm` plugins over
the `CARD/1` framed protocol with CRC32 verification. The files are stored in
the ESP32 LittleFS partition and restored after restart.
