# TouchDuino – working notes for Claude

ESP32-S3 port of MaxDuino (tape emulator for retro computers) with an LVGL touch UI,
for the Guition JC3248W535 board. The hardware/architecture spec is in [Touchduino.md](Touchduino.md); read it first.

## Build / flash
- PlatformIO CLI (not on PATH): `~/.platformio/penv/Scripts/pio.exe run` (`-t upload`, `-t monitor`)
- Python is available as `~/.platformio/penv/Scripts/python.exe` (no system python)
- Board enumerates as **COM9** (USB VID 303A:1001, native USB-JTAG/serial)
- `sdkconfig.defaults` is the source of truth; delete the generated `sdkconfig.jc3248w535` after editing it
- Upstream reference clones live in `reference/` (git-ignored). Re-clone if missing:
  `git clone --depth 1 https://github.com/rcmolina/MaxDuino.git reference/MaxDuino`
  `git clone --depth 1 https://github.com/NorthernMan54/JC3248W535EN.git reference/JC3248W535EN`

## Layout
| Path | What |
| --- | --- |
| `src/main.c` | NVS settings -> tape engine start -> SD mount -> display (LVGL pinned core 0) -> `ui_init` |
| `src/esp_bsp.*`, `esp_lcd_*`, `lv_port.*`, `lv_conf.h` | Board support copied from NorthernMan54/JC3248W535EN (display, touch, LVGL glue) |
| `src/engine/` | MaxDuino engine. Most `.cpp/.h` are **copied verbatim** from upstream (`MaxProcessing`, `isr`, `casProcessing`, `uef`, `oric`, `kansas_4b`, `zx8081`, `ayplay`, `mzf`, `mtx`, `caq`, `c64tap`, `CheckForExt`, ...) |
| `src/engine/Arduino.h` | Arduino shim (byte/word, bitRead, PROGMEM no-ops, `noInterrupts` = core-1 critical section) |
| `src/engine/configs.h` | Replaces MaxDuino userconfig chain; all formats enabled |
| `src/engine/TimerCounter.cpp` | gptimer 1 MHz; alarms re-armed relative to previous alarm (no drift); ISR IRAM-safe |
| `src/engine/pinSetup.h` | GPIO17 tape out via direct register writes; GPIO18 motor sense |
| `src/engine/file_utils.cpp` | SdFat replaced by `TapeFile` (FILE* + 16 KB read cache) |
| `src/engine/engine.cpp` | Replaces MaxDuino.ino: FreeRTOS task on core 1, command queue, TZX/TAP block pre-scan for block skip, REM handling, status snapshot |
| `src/engine/tape_engine.h` | C API used by the UI (`tape_play/pause/stop/block_skip/apply_settings/get_status`) |
| `src/engine/monitor.cpp` | **Speaker output**: ISR records edges into a ring; task on core 0 replays them to the onboard I2S amp (BCK 42, WS 2, DOUT 41) at 44.1 kHz, ~40 ms behind, DC-blocked |
| `src/app/storage.c` | SD at `/sdcard`: SDMMC 1-bit only (CLK12 CMD11 D0 13); `sd_remount()` for card swaps; NVS settings |
| `src/ui/ui.cpp` | LVGL UI: header, paged file list, preview pane (cover art / system badge), progress, 6 transport buttons, settings screen |
| `src/ui/cover.cpp` | JPG (LVGL tjpgd) / PNG (lodepng) decode + scale into PSRAM |

## Changes made to upstream MaxDuino files (keep minimal, mark with "TouchDuino")
- `buffer.h/.cpp`: buffsize 2048, readpos/writepos widened to `word`, buffer in DRAM
- `isr.cpp`: `wave2` marked `IRAM_ATTR`
- `MaxProcessing.cpp/.h`: `tapePath[]` global; `UniPlay` opens `entry.open(tapePath)`
- `casProcessing.cpp`: fixed upstream bug `for (byte i=9; i>=0; --i)` (infinite loop) -> `int`

## Decisions
- Sound: the user wants audio **out of the onboard speaker** (small speaker on the board's speaker connector). That's the I2S monitor, on by default, volume in Options. GPIO17 still outputs the line-level signal for a 3.5 mm jack (needs an RC filter; never drive a speaker directly from GPIO17).
- Motor default is **Force run** (`DEFAULT_MSELECTMASK 0`) because GPIO18 floats high (pull-up) with nothing connected, which would pause playback.
- Block skip uses a pre-scan table (TZX/TSX/CDT and Spectrum/Jupiter TAP). 0x12/0x13 runs are grouped with the following data block.
- MaxDuino has no LICENSE file upstream; keep attribution in sources.

## Status (update as work progresses)
- [x] Project scaffold from JC3248W535EN base, platform/toolchain install
- [x] Engine port written (shims, timer, file I/O, glue, block table)
- [x] Speaker monitor (I2S) written
- [x] UI written (browser, preview, transport, settings)
- [x] cover.cpp (JPG via LVGL tjpgd RGB888, PNG via lodepng C API declared manually)
- [x] Compiles clean (`pio run`), flashed to board 2026-11-14, boots without crash
- [x] Timer rewritten on TIMG1 T0 via LL (gptimer driver's IRAM linker fragment is NOT applied by PlatformIO; verified with nm/objdump that wave2/tape_isr/setPeriod are in IRAM 0x4037xxxx)
- [x] Settings has a speaker "Test" button (700 ms 1 kHz beep via `monitor_test_beep`)
- [x] SD card works in **SDMMC 1-bit** mode (as vendor demos). SPI mode does not work on this board and can latch a card into SPI mode until power-cycled, so the SPI fallback was removed.
- [x] ↻ button unmounts + remounts the card (no card-detect pin), stopping playback first; card swap verified by user 2026-11-15
- [x] Speaker test beep and .tap playback through the speaker verified by user 2026-11-15
- [ ] Copy some .tzx/.tap onto the card and test playback (check serial log for "play ..." / "navigable blocks")
- [ ] User to confirm display/touch UI looks right
- [ ] Test with speaker (arriving 2026-11-15): Options > Test, then play a .tzx and listen; then test loading into a real machine via 3.5 mm

## Gotchas
- ESP-IDF builds with -Werror=switch etc.; upstream engine files get relaxed warnings in `src/CMakeLists.txt` (only `src/engine/*.cpp`)
- `lodepng.h` turns into a C++ API when included from C++; don't include it from .cpp
- Anything called from the tape ISR must be IRAM (check with `xtensa-esp32s3-elf-nm -C firmware.elf`); `Timer` is an object in DRAM, not a reference (a reference lived in flash rodata)
- Serial log helper: `~/.platformio/penv/Scripts/python.exe -I tools/serlog.py COM9 <seconds> reset` (pyserial from penv; `reset` reboots the board first)

## Testing without hardware
- Serial log at 115200 (`pio.exe device monitor -p COM9`); engine logs `play ...`, `N navigable blocks`, `jump to block`.
