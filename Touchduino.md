# Project: TouchDuino (ESP32-S3 MaxDuino Port)

## Hardware Target
- Board: Guition JC3248W535C (ESP32-S3, 16MB Flash, 8MB OPI PSRAM)
- Framework: PlatformIO + ESP-IDF + LVGL 8.4 (based on NorthernMan54/JC3248W535EN)
- Display: 320x480 QSPI AXS15231B (CS:45, SCK:47, D0:21, D1:48, D2:40, D3:39, BL:1)
- Touch: I2C Capacitive (SDA:4, SCL:8)
- microSD (SPI): CS:10, MOSI:11, CLK:12, MISO:13 (mount at `/sdcard`)
- Tape Audio Output (3.5mm jack): GPIO 17 (RC-filtered digital pulse output; do NOT use onboard Class-D speaker amp for line-out)
- Motor Control Input (2.5mm REM jack): GPIO 18 (Internal pull-up; LOW = Motor ON / Play, HIGH = Motor OFF / Pause)

## Architecture Rules
1. **Dual-Core Isolation:** Keep all LVGL rendering, SD directory scanning, and image decoding on Core 0. Run the tape pulse generator (`gptimer` / RMT) and motor-sense pin monitoring on Core 1 so screen touches never cause tape loading jitter.
2. **Tape Engine:** Reference `./reference/MaxDuino` for TZX, TAP, CDT, UEF, CAS, P, O, MZF, and CAQ block parsing and baud/pulse timing tables. Replace Arduino `SdFat` calls with standard C `FILE*` (`fread`/`fseek`) and replace AVR timer ISRs with ESP-IDF `gptimer`.
3. **Cover Art & System Icons:**
   - When selecting or highlighting `game.tzx` (or `.tap`, `.cdt`, `.uef`, etc.), check for `game.jpg` or `game.png` in the same folder and display it in the UI preview pane (decoded into PSRAM).
   - If no matching game image exists, check the current folder (and parent folders up to `/sdcard`) for `folder.jpg`/`folder.png`, or match folder names (`Sinclair`, `Spectrum`, `Amstrad`, `CPC`, `BBC`, `MSX`, `Dragon`) to built-in system badge graphics.
4. **UI Controls:** Provide large touch targets for Folder/File Browser, Play/Pause, Stop, Prev/Next File, Block/Track Skip, Progress Bar, Baud/Options toggle, and a Motor Sense override toggle (Auto REM vs. Force Run).
5. **Build Command:** Always verify changes with `pio run` (and `pio run -t upload` when requested).

Maxduino repo https://github.com/rcmolina/MaxDuino

https://github.com/rcmolina/MaxDuino.git
