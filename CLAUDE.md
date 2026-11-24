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
| `src/main.c` | Crash log, USB-mode boot check, NVS settings -> tape engine start -> SD mount -> display (LVGL pinned core 0, `APP_DISPLAY_ROTATION` = LV_DISP_ROT_270) -> `ui_init` |
| `case/` | User's 3D-printed case (`Case_JC3248W535C.3mf` / `.stl`); the board sits turned 180 degrees in it, hence ROT_270 |
| `src/esp_bsp.*`, `esp_lcd_*`, `lv_port.*`, `lv_conf.h` | Board support copied from NorthernMan54/JC3248W535EN (display, touch, LVGL glue) |
| `src/engine/` | MaxDuino engine. Most `.cpp/.h` are **copied verbatim** from upstream (`MaxProcessing`, `isr`, `casProcessing`, `uef`, `oric`, `kansas_4b`, `zx8081`, `ayplay`, `mzf`, `mtx`, `caq`, `c64tap`, `CheckForExt`, ...) |
| `src/engine/Arduino.h` | Arduino shim (byte/word, bitRead, PROGMEM no-ops, `noInterrupts` = core-1 critical section) |
| `src/engine/configs.h` | Replaces MaxDuino userconfig chain; all formats enabled |
| `src/engine/TimerCounter.cpp` | gptimer 1 MHz; alarms re-armed relative to previous alarm (no drift); ISR IRAM-safe |
| `src/engine/pinSetup.h` | GPIO17 tape out via direct register writes; GPIO18 motor sense |
| `src/engine/file_utils.cpp` | SdFat replaced by `TapeFile` (FILE* + 4 KB read cache) |
| `src/engine/engine.cpp` | Replaces MaxDuino.ino: FreeRTOS task on core 1, command queue, TZX/TAP block pre-scan for block skip, REM handling, status snapshot |
| `src/engine/tape_engine.h` | C API used by the UI (`tape_play/pause/stop/block_skip/apply_settings/get_status`) |
| `src/engine/monitor.cpp` | **Speaker output**: ISR records edges into a ring; task on core 0 replays them to the onboard I2S amp (BCK 42, WS 2, DOUT 41) at 44.1 kHz, ~40 ms behind, DC-blocked |
| `src/app/storage.c` | SD at `/sdcard`: SDMMC 1-bit only (CLK12 CMD11 D0 13); `sd_remount()` for card swaps; USB file transfer mode (`sd_usb_*`); NVS settings |
| `src/ui/ui.cpp` | LVGL UI: header, paged file list, preview pane (cover art / system badge), progress, 6 transport buttons, settings screen |
| `src/ui/cover.cpp` | JPG (LVGL tjpgd) / PNG (lodepng) decode + scale into PSRAM |

## Changes made to upstream MaxDuino files (keep minimal, mark with "TouchDuino")
- `buffer.h/.cpp`: buffsize 4096, readpos/writepos widened to `word`, buffer in DRAM, clearBuffer fills 50 us hold entries
- `isr.cpp`: `wave2` marked `IRAM_ATTR`; underrun counter at the page swaps (`g_tape_diag`)
- `MaxProcessing.cpp/.h`: `tapePath[]` global; `UniPlay` opens `entry.open(tapePath)`
- `casProcessing.cpp`: fixed upstream bug `for (byte i=9; i>=0; --i)` (infinite loop) -> `int`
- `c64tap.cpp`: exact cycles-to-us (upstream integer rounding played PAL C64 tapes 1.5% fast); second half-pulse takes the odd microsecond

## Decisions
- Sound: the user wants audio **out of the onboard speaker** (small speaker on the board's speaker connector). That's the I2S monitor, on by default, volume in Options. GPIO17 outputs the tape signal for the 3.5 mm jack through an **LM386 stage at gain 20** (`images/line-out-lm386.png`, or a ready-made LM386 module with the pin 1-8 part removed, fed straight from GPIO17). The plain RC filter (`images/line-out.png`) is too quiet for a real Spectrum. Never drive a speaker directly from GPIO17.
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
- [x] TAP/TZX playback on the board, speaker, UI confirmed by user
- [x] Loads on a real ZX Spectrum 48K and 128K through the LM386 stage (2026-11-16, see "Real Spectrum tests")
- [x] MSX loads (CAS, up to 2400 baud) and BBC Model B loads (UEF, compressed files too) (2026-11-17)
- [x] REM motor control verified on the MSX and the BBC (2.5 mm cable: tip -> IO18, sleeve -> GND); REM invert option added
- [x] USB file transfer mode (2026-11-16)
- [x] Display turned 180 degrees (LV_DISP_ROT_270) for the printed case; picture and touch verified by the user 2026-11-22
- [x] Amstrad CPC 464 loads (AmstradDiag.cdt with RUN"", and Auf Wiedersehen Monty, 2026-11-24) with REM
- [ ] Commodore 64 (adapter drawn, not built)
- [x] Oric Atmos loads (Manic Miner .tap, 2026-11-23) with REM
- [x] ZX81 loads (3D Monster Maze .p, 2026-11-23); ZX81 badge and raster-style loading stripes

## Gotchas
- ESP-IDF builds with -Werror=switch etc.; upstream engine files get relaxed warnings in `src/CMakeLists.txt` (only `src/engine/*.cpp`)
- `lodepng.h` turns into a C++ API when included from C++; don't include it from .cpp
- Anything called from the tape ISR must be IRAM (check with `xtensa-esp32s3-elf-nm -C firmware.elf`); `Timer` is an object in DRAM, not a reference (a reference lived in flash rodata)
- Serial log helper: `~/.platformio/penv/Scripts/python.exe -I tools/serlog.py COM9 <seconds> reset` (pyserial from penv; `reset` reboots the board first)

## Testing without hardware
- Serial log at 115200 (`pio.exe device monitor -p COM9`); engine logs `play ...`, `N navigable blocks`, `jump to block`.

## Display performance (2026-11-15)
- AXS15231B in QSPI mode has no RASET: every update must stream the whole frame from row 0. The base project therefore used `full_refresh`, which re-rendered all 480x320 pixels into PSRAM for ANY change (~80 ms/frame).
- `src/lv_port.c` now uses a **shadow frame buffer** (native 320x480, PSRAM) when rotated 90 degrees: LVGL redraws only dirty areas into a 40-line internal buffer, the flush rotates them into the shadow, and the last flush of a refresh streams the whole shadow (TE sync + double-buffered DMA). Animated border: ~32 ms/frame, most of it waiting on DMA/TE, not CPU.
- Built with `CONFIG_COMPILER_OPTIMIZATION_PERF` (-O2); this made GCC flag a strncpy truncation as an error in engine.cpp (now snprintf).
- UI rules: only set label text when it changed; animations only invalidate while active. Scrolling labels (header path, preview filename, 18 px/s) are OK now that only their area redraws (~20 small frames/s at idle).
- `ui.cpp`: `UI_PERF_LOG 1` logs render time every ~5 s of activity; `UI_ANIM_SELFTEST 1` forces the loading stripes for measuring.

## UI look
- "Glass" theme = opaque vertical gradients + 1 px translucent borders (no large alpha areas, no shadows: both are expensive here).
- Preview is a "TV": bezel > border frame (TV_BORDER 8 px) > screen 144x92. Border colour is the system's (Spectrum light grey, CPC blue, C64 light blue...).
- Loading border while playing is the **live signal** (`draw_live`): reads the last 20 ms of edges from `g_mon_ring` (recorded by the ISR for the speaker, now always recorded), ~35 ms behind to line up with the speaker, one row per ~185 us. Spectrum: red/cyan for pulses >= 540 us (pilot), blue/yellow for shorter (data); CPC/C64: a palette colour per pulse. **ZX81 (FK_ZX81):** `tv_screen` is hidden and `s_band_full` makes the bands fill the whole TV; drawn in raster order (each row swept left to right in time, ~185 us per row) so edges land mid-row and the black/white stripes come out slanted like a real ZX81 on an old TV; silence = white. Gaps > 3 ms = silence = system border colour (Spectrum black). Other formats (BBC, MSX, Oric, Jupiter...) keep a static border. `tape_status_t.phase` is still reported but no longer used by the UI.
- Image fallback chain: game.jpg/png -> folder.jpg/png up the tree -> system badge from folder name -> default placeholder (black screen, cycling Spectrum-style loading border).

## USB file transfer mode (2026-11-16)
- Options > **USB file transfer** shows the SD card to the PC as a USB drive (TinyUSB MSC on the native USB port, `src/app/storage.c` `sd_usb_*`, screen `build_usb()` in ui.cpp). Verified by the user: browse, delete, rename, new folder, copy, eject, Exit.
- Components via `src/idf_component.yml`: espressif/esp_tinyusb ~1.4.4 with **espressif/tinyusb ~0.15.0** (the default 0.21 doesn't match esp_tinyusb 1.4). `managed_components/` and `dependencies.lock` are generated.
- **Entered and left by reboot** (one-shot NVS flag `usbboot`, cleared when read, so a crash always ends in a normal boot). Runtime switching between the USB-Serial-JTAG and the OTG controller failed three ways: the USJ reset the chip (ESP_RST_USB), deleting the USJ PHY left the pad pull override on with no D+ pull-up (PC saw nothing), and an interrupt-watchdog.
- USB-mode boot: no sd_mount; `sd_usb_start()` inits the card raw, sets RTC_CNTL_USB_RESET_DISABLE | IO_MUX_RESET_DISABLE (otherwise the USJ resets the chip when the port is taken), installs TinyUSB. Exit = `tud_disconnect()` + `esp_restart()`.
- Normal boot: `sd_usb_boot_init()` routes the port back to the USJ if RTC still says OTG (a soft reset keeps the routing) and clears the reset-disable bits (esptool auto-reset needs them).
- **CONFIG_TINYUSB_MSC_BUFSIZE must stay 512.** 8192 hung the USB IN interrupt on the first disk read (INT WDT, `_xt_medint3` over `proc_read10_cmd` -> `usbd_edpt_xfer`). TINYUSB_DEBUG_LEVEL 0.
- **Crash diagnostics** (main.c): every log line is copied into an RTC_NOINIT ring; `esp_panic_handler` is wrapped (`-Wl,--wrap=esp_panic_handler` in platformio.ini) to append the panic reason + backtrace. On the next boot a log that ended in a crash (panic/watchdog) or reached USB mode is saved as `/sdcard/crashlog.txt`, and that file is printed line by line ~6 s after every boot (USJ drops bursts). Read it with `tools/serlog.py COM9 25 reset`, decode with `xtensa-esp32s3-elf-addr2line -pfiaC -e .pio/build/jc3248w535/firmware.elf <addrs>`.

## Tools and troubleshooting (2026-11-15)
- **Screenshot:** long-press the header path -> `lvgl_port_save_screenshot()` writes `/sdcard/screenshot.bmp` (480x320, 24-bit) from the shadow frame buffer. `images/lcd-preview.jpg` is a real screenshot placed in a device frame (render script was a one-off Pillow job).
  - Don't stream screenshots over USB: the USB-Serial-JTAG is only the *secondary* console (drops data), and the driver path ran at ~1 KB/s and froze the UI.
- **Upload fails with "No serial data received" after "Stub running":** the USB link is in a bad state; replug the board, or flash with the ROM loader:
  `~/.platformio/penv/Scripts/python.exe ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port COM9 --baud 115200 --no-stub write_flash 0x10000 .pio/build/jc3248w535/firmware.bin`
- **Engine diagnostics:** the engine logs the block table at play and every block-ID / play-pause-stop change (`engine:` lines). Used to show that DECATHLN.TAP is two full loads (Day 1 + Day 2): TAP files have no stop markers, so playback continues into Day 2 (user chose not to add a TAP auto-pause heuristic).
- Pillow is not in the PlatformIO penv; install into a scratch folder with `pip install --target` if needed.

## Playback behaviour fixes (2026-11-15)
- **Browsing locked while playing/paused** (`set_browse_locked` in ui.cpp): list children + back/refresh/prev/next get LV_STATE_DISABLED (disabled styles `st_item_dis` / `st_btn_dis`) and callbacks early-return; the preview stays on `s_playing` until the tape stops (`update_preview`).
- **Folder scan** uses FatFs `f_readdir` (name + size + attr in one pass) via `sd_fatfs_drive()`; `readdir` + `stat` per file was O(n^2) on FAT. Log shows `read X ms, list Y ms`.
- **Status after stop:** `stopFile()` sets "End of tape" (or "Stopped early (block XX)") when the engine stops itself; user stops set `s_user_stop` and show "Stopped".
- **Skip 2A on by default** (DEFAULT_SKIP2A 1, NVS migration via `cfgver` = 2): ID2A "stop if 48K" wrongly paused 128K games. ID20 pause-0 markers still pause.
- **Long gaps:** TZX block pauses come straight from the file (DNA Warrior has a 35 s gap after its loading screen and 52 s at the end; an Android player shortens them). Option **Shorten long gaps** (default on, NVS key `gaps`) caps them at 5 s via `maxGapMs` in MaxProcessing.cpp's IDPAUSE handler.
- Engine logs each TZX data block's gap at play (`id 0x10 @ off: N bytes, then X ms gap`) and any stop markers. Block/ID transitions are sampled every 50 ms, so short IDPAUSE states are not logged.

## Real-machine testing (overview)
- Machines: ZX Spectrum 48K + 128K, MSX (Just for Fun MSX1), BBC Model B (with a PiTube 6502 co-processor), **ZX81** (cherry-key self-build) and **Oric Atmos** (self-build) all **load and are in the README tables**; REM verified on the MSX, BBC and Oric. The **Amstrad CPC 464** loads too, with REM. Still to do: C64 (see below).
- Line out: LM386 at gain 20 (circuit or module), volume about mid, PC USB power. Note per machine: loads / fails partway / never starts, and the file used. The README's "Real-machine tests" section is the user-facing record: keep it in step with the notes below.
- Also verified on the board: speaker, live border, card swap, browse lock, End of tape, long-gap shortening (matches an Android player; MaxDuino on a TZXduino plays the full 35 s gap), USB file transfer, screen rotation.

## Real Spectrum tests (2026-11-16)
- **MANIC.TAP loads on the real Spectrum** (through the LM386 stage, `images/line-out-lm386.png`, VR1 mid, PC USB power).
- Bug 1, underrun: the 16 KB SD cache refill outlasted a buffer page. Fixed: TAPE_CACHE_SIZE 4 KB, buffsize 4096, clearBuffer fills 50 us hold entries (0x5032) instead of zeros (which play as 1 ms each).
- Bug 2, tape tail lost: upstream's 255-entry EOF flush is shorter than the bigger buffer, so stopFile() dropped ~200 bytes from the end of the last block ("R Tape loading error 20:5"). Fixed: `drain_tape()` in engine.cpp pads with holds and waits for two page swaps on a natural stop (not on a user stop).
- Diagnostics left in: `timing @` lines every 5 s (ISR lateness, underruns; TimerCounter `g_tape_diag`) and the **self-check decoder** in monitor.cpp. It decodes the recorded edge stream like the Spectrum ROM and logs `selfcheck block N: bytes, flag, parity OK/BAD, half-pulse min..max`. Use it first when a load fails: parity BAD or a short length = software; all OK = analog/noise.
- The GPIO17 + RC filter alone was too quiet for this Spectrum; the ZAXON TZXduino has an LM386 too (no pot on it).
- Results (VR1 mid, PC USB power): Manic Miner TAP x2 and Decathlon TAP x2 (48K); D.N.A. Warrior TZX, Auf Wiedersehen Monty TZX, Marauder TZX with countdown loader (128K). Marauder TAP hangs after loading but the self-check shows every byte sent correctly: a modified TAP (BASIC 295 bytes vs 83 in the TZX).
- The last block of a file self-checks as "N+15 bytes, 7 stray bits, 0-half 10.." because upstream's IDEOF flush toggles the pin every 10 us (255 times) straight after the data. Harmless (the ROM has stopped reading); the TZXduino does the same. Could make the self-check ignore pulses < 100 us.
- The earlier Decathlon "stopped at 50%" was never reproduced (likely a loose breadboard wire). Decathlon plays on into Day 2 after Day 1; the game ignores it.
- README now documents the LM386 circuit and the self-check; `images/line-out.png` (no amp) is kept as the REM/plain-filter reference.
- 2026-11-17: the user's ready-made LM386 module boards work with R1 removed (gain 20 instead of 200): 48K (Manic, Decathlon) and 128K (D.N.A. Warrior, Monty, Marauder TZX) all load, self-check all OK.
- Start-of-play "underruns 1" (harmless: silence before the pilot): the ISR played the start-up holds during the block-table scan (100+ ms) and then swapped to a half-filled page. Fixed in do_play(): the ISR is held (isStopped) during the scan, and the first write page is filled before it is released (UniLoop() clears isStopped, so it is set again after each call; drain_tape() is skipped while isStopped). Verified on D.N.A. Warrior: underruns 0 for the whole load.
- **CPC 464 (2026-11-24): AmstradDiag.cdt loads from TouchDuino** (usual volume). The earlier "Memory full" at block 1 (TouchDuino and both TZXduinos) was the user typing LOAD"": BASIC refuses a binary that loads below HIMEM (&0400 here, &0100 for the Montys). The machine was never faulty (12 RAM soak passes). Use RUN"". A corrupt CPC header gives "Read error" (CRC), not "Memory full"; |TAPE is only for machines with a disc ROM. **Auf Wiedersehen Monty (E).cdt** also loads: 512-byte loader, then a 64638-byte turbo block read by its own loader; Auto REM on, REM invert off (paused at 81 until the CPC motor came on, off/on at 941).
- **MSX (2026-11-17)**, Just for Fun MSX1, LM386 module, 1200 baud, Force run: **Chase H.Q. (EU).cas loads** (ASCII loader -> BLOAD CM custom loader with flashing border, multi-load: the game loaded at ~60% of the file, later levels follow). Choplift (EU).cas played to End of tape but the MSX rebooted when the game started: its loader does POKE &HFFFF,&HAA (slot-select for a specific machine's memory layout; a cartridge conversion), not a tape fault. The MSX motor relay is audible. User's CAS collection: D:/MSX/Games/CAS (A-Z folders) copied via USB mode to /sdcard/MSX/Games.
- A scratch script (casinfo) lists CAS blocks and the load command from the first header: 10x D3 = CLOAD, 10x EA = RUN"CAS:", 10x D0 = BLOAD"CAS:",R.
- **REM invert** option (Options, NVS key `reminv`, `tape_settings_t.rem_invert`, default off = contact closed/IO18 low = play), applied in engine.cpp `motor_line_on()` (also drives the header REM indicator).
- **REM verified on the MSX** (2.5 mm cable: tip -> IO18, sleeve -> GND; Auto REM on, REM invert off): MOTOR ON/OFF show REM play/pause in the header; during Chase H.Q. every relay change paused/resumed at the exact byte (296, 1456, 11310, 53488). Brief off/on blips of 2-6 ms happen (relay bounce or the loader flicking the motor): harmless, a ~20 ms debounce could be added if needed.
- Chase H.Q. also loads at **2400 baud**. Its layout: BASIC loader, CM loader (1128), 9736 screen, 2x 21128 main, then 5x (8-byte stage header + 6904 data). After the main load it reads the next stage header (motor off/on) and shows "stop the tape and press any key" with the motor left ON: stage 1 is already in memory, so the user pauses (not stops) to park the tape at the next stage.
- **MSX speeds** (Just for Fun MSX1): 1200 and 2400 baud load; **3600 and 3850 give "Device I/O error"** right after the first header (Found: then error; BASIC stops the motor and REM pauses TouchDuino). 3150 untested. Sorcery (EU).cas (Virgin: ASCII loader + 3 BLOAD files) loads at 2400 with REM off/on blips at each file (296, 16736, 30440). Use 2400 as the MSX default.
- **BBC Model B (2026-11-17): Chuckie Egg UEF loads** (*TAPE hung the user's machine; after BREAK the tape FS was usable with CHAIN""). Gotchas: (1) UEF plays standard timing **only when Baud = 1200**; any other baud uses MaxDuino's UEF turbo pulses (uef.cpp `if (BAUDRATE == 1200)`) and the BBC just says Searching. (2) Stairway to Hell / most archive UEFs are **gzip-compressed** (1f 8b); the engine needs "UEF File!" so they must be gunzipped (done on the PC so far). Both now done: **UEF always plays at 1200** (`s_force_1200` in engine.cpp, set in do_play from the extension and honoured by apply_settings), and **gzip files are unpacked into PSRAM** by `TapeFile::open_gzip()` (file_utils.cpp, ROM `tinfl_decompress`, up to 4 MB, served from memory; `filesize` is reset from `entry.fileSize()` after UniPlay). The tinfl decompressor state (~11 KB) must be heap-allocated: `tinfl_decompress_mem_to_mem` puts it on the 6 KB tape-task stack and rebooted the board. Verified: compressed Chuckie Egg at Baud 2400 loads. **REM works on the BBC too** (Auto REM, invert off; the user's BBC lead has the motor pins on the REM jack): the MOS stops the motor after each file (paused 12.5 s at byte 4206 while the loader ran) and the loader restarts it; resumed at the exact byte, End of tape.
- **Oric Atmos (2026-11-23): Manic Miner .tap loads** (BBC/Oric 7-pin DIN lead, Auto REM, invert off, Invert output off). Gotchas: (1) ORICSPEEDUP: oric.cpp plays standard timing only at BAUDRATE <= 2400, so do_play sets `s_force_baud = 2400` when `currentID == BLOCKID::ORIC` after UniPlay (UEF uses 1200 via the same `s_force_baud`). (2) An Oric file has ONE short sync + header at the start: if the tape is already playing when CLOAD"" starts, the Oric misses it (it may then lock onto a header-like pattern mid-data, show "Loading .. c" and give up). With Auto REM, press play first: TouchDuino waits at REM pause until CLOAD"" switches the relay on (verified: started 1.8 s after, from byte 4). (3) "Loading ..      C" is correct: blank name, C = code (B = BASIC). The game then asks for its original unlock code (protection, not a load fault); the TZXduino behaves identically.
- **Invert output** option (Options, NVS `outinv`, `tape_settings_t.out_invert`, `g_tape_invert` in pinSetup.h's `tape_out_write`): flips only the GPIO, `g_tape_level` stays logical so speaker/border/self-check are unaffected; applied live. Not needed by any machine so far. MaxDuino's own polarity flag only sets the level at pauses, not data.
- Oric badge: `SYS_ORIC` from an `oric`/`atmos` folder name (black, white text, red stripes); .tap stays Spectrum by extension, the engine detects Oric by header.

## C64 datasette interface (planned)
- Datasette port is digital: READ (to C64), SENSE (tie to GND = PLAY pressed), MOTOR (~6-9 V from C64), GND. c64tap.cpp is compiled in; the UI has a C64 badge and live border.
- Design (2026-11-23, not built): TouchDuino's case only gets a 3.5 mm stereo socket (**tip IO17, ring IO18, sleeve GND**); the adapter is at the C64 end, powered by the C64's +5 V (B-2). Schematic `images/c64-interface.png` (`tools/draw_c64.py`), stripboard 9 strips x 24 holes `images/c64-stripboard.png` (`tools/draw_c64_stripboard.py`, checks its own connectivity). IO17 -> 1k -> 74HCT125 -> READ (D-4); MOTOR (C-3) -> 1.5k -> PC817 LED (1N4148 across), transistor IO18 -> GND; SENSE (F-6) to GND (A-1); power LED + 1k. Auto REM on, REM invert off. Never plug the C64 lead into the LM386 audio jack. **Edge plug must be keyed:** upside down reverses A-1..F-6 (MOTOR lands on READ). With SENSE tied low the KERNAL probably runs the motor at the BASIC prompt, so Auto REM may not hold the tape before LOAD: type LOAD first, then play (to check).

## ZX81 and ZON-X ideas (from 2026-11-22, not started)
- The user has a cherry-key self-built ZX81 with a self-built **ZON-X compatible AY sound interface** (GAL address decoding): https://github.com/Bambelweeny57/ZXEightyZON (showcased with the Multi Scroller demo). 16K of 32K RAM active; an FPGA ULA would give 32K, but the user prefers to stay authentic (with LS logic only an AY-3-8912 worked, and not with that ULA; untried with the GAL decoder).
- Idea: play AY chiptunes (AY-3-8910 / YM2149 versions, e.g. Afterburner, Space Harrier) on it: a Z80 player saved as a `.P` file that TouchDuino loads (zx8081 path), either with the register data in the same `.P` or a player for `.ay` files like the Spectrum's. Check the ZON-X port addresses in that repo first; .ay files contain Spectrum player code (ports 0xFFFD/0xBFFD, interrupts) that would need adapting, and the 16K RAM limit matters for register dumps.
- Plain `.P` loading works on this ZX81 (2026-11-23: 3dmon.p, 10552 bytes, ~2 min 20 s, underruns 0, End of tape). ZX81 system in ui.cpp: `SYS_ZX81` from a `zx81`/`zx80` folder name (checked before "sinclair") or the `.p/.81/.p81/.o` extensions; badge black on white, white border.
- Plan for the AY player: a **separate repo** for the Z80 work, with **sjasmplus** (can output .P and .tap) + the **DeZog** VS Code debugger + **ZEsarUX** (ZX81/Spectrum emulator; check its ZX81 AY/ZON-X sound option). Build and debug in the emulator, then copy the .P to TouchDuino's card for the real ZX81.

## Spectrum loading screens (2026-11-15)
- `cover_from_spectrum_tape()` in cover.cpp: walks TZX (ID10/11/14 data) or TAP blocks, finds a SCREEN$ (a "CODE 16384,6912" header + data block, else a headerless 6914-byte block with flag 0xFF), decodes the 6912 bytes (interleaved bitmap + attributes, BRIGHT 0xFF / normal 0xD7) to 256x192 and box-filter scales it to the TV screen (122x92).
- Preview order: game.jpg/png -> tape loading screen -> folder.jpg/png -> system badge -> placeholder. Verified on DNA Warrior 2026-11-15.
- Idea for later: draw the screen in live while it loads (Spectrum third-by-third order).
