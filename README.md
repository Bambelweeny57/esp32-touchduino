![ESP32-Touchduino Banner](images/banner.jpg)

# TouchDuino

An ESP32-S3 port of [MaxDuino](https://github.com/rcmolina/MaxDuino), the tape emulator for 1980s home computers. It has a 3.5" touch screen in place of physical buttons. Put tape images on a microSD card, pick one on the screen, and TouchDuino plays it as a cassette signal for your Spectrum, Amstrad CPC, BBC Micro, MSX and other machines to load.

It runs on the Guition **JC3248W535** board:
- ESP32-S3 with 16 MB flash and 8 MB PSRAM
- 320×480 QSPI touch display
- microSD slot
- built-in speaker amplifier

<img src="images/lcd-preview.jpg" alt="TouchDuino screen playing Manic Miner, with the loading stripes in the preview border" width="480">

*The screen is a real screenshot from the board, playing `MANIC.TAP` with its loading stripes in the preview border.*

> **Status:** working. Browsing, cover art, playback and the speaker are tested on the board. Loading into a real computer through the 3.5 mm output is still to be tested.

## Features

- **Supported formats:** TZX, TAP, CDT, TSX, UEF, CAS, P, O, AY, MZF/MZT/M12, MTX, CAQ and Commodore 64 TAP. These use MaxDuino's format code, so timing matches the original.
- **Touch controls:** previous/next file, block back/forward, play/pause and stop, all as large buttons. There's also a progress bar and a status line, for example `PLAY Blk 6/6 TAP 2%`.
- **Glass-style screen:** dark gradient background, rounded panels and gradient buttons. Long folder paths and file names scroll.
- **File browser:** folders are listed first. It remembers the last folder you used.
- **Preview "TV":** the selected tape is shown on a small TV screen. The screen shows the first of these it finds:
  1. `game.jpg` or `game.png` next to `game.tzx` (baseline JPEG or PNG; progressive JPEGs aren't supported);
  2. for Spectrum `.tzx` and `.tap` files, the **loading screen inside the tape** itself. It is found by its `CODE 16384,6912` header, or as a 6,912-byte screen block on its own. Screens that a game unpacks or draws with a custom loader can't be found this way;
  3. `folder.jpg` or `folder.png` in the folder or any parent folder;
  4. a system badge, if a folder is named `Spectrum`, `Sinclair`, `Amstrad`, `CPC`, `BBC`, `MSX`, `Dragon`, `C64` or `Commodore`;
  5. otherwise a black screen with a gentle "loading" animation in the border.
- **Live loading border:** while a tape plays, the TV border is drawn from the real tape signal, lined up with what you hear, like the original loaders:
  - **ZX Spectrum:** red/cyan bands during the pilot tone and thin blue/yellow bands during data;
  - **Amstrad CPC and C64:** a different colour for each pulse, like their turbo loaders;
  - **other machines:** a plain border.

  The border goes black (or the machine's own border colour) whenever there's no sound. Drawing it never touches the tape timing.
- **Speaker:** the tape signal is mirrored to the onboard speaker, so you can hear it load. Volume is adjustable and there's a test beep.
- **Line out:** GPIO 17 outputs the signal for a 3.5 mm jack to the computer's EAR/tape input.
- **Motor control (REM):** in **Auto REM**, the computer's motor line on GPIO 18 pauses and resumes playback, and the header shows **REM ▶** or **REM ❚❚**. In **Force run** the line is ignored and the header shows **RUN**.
- **Options:**
  - speed for TSX/CAS/CDT (1200–3850 baud);
  - **Shorten long gaps** (on by default): gaps over 5 s in the file play as 5 s. TZX transfers often contain long stretches of blank tape, such as 35 s after a loading screen. Turn it off if a game ever fails to load;
  - **Skip TZX block 2A** (on by default): ignores "stop the tape if in 48K mode" markers, which would wrongly pause 128K games;
  - MaxDuino's polarity/parity flag.

  Settings are kept after power-off.

## Hardware

| Function | GPIO |
| --- | --- |
| Display (QSPI) | CS 45, CLK 47, D0–D3 21/48/40/39, backlight 1 |
| Touch (I2C) | SDA 4, SCL 8 |
| microSD (SDMMC 1-bit) | CLK 12, CMD 11, D0 13 (CS/DAT3 10) |
| Speaker amp (I2S) | BCK 42, WS 2, DOUT 41 |
| **Tape out** | **17** |
| **Motor sense (REM)** | **18** (LOW = motor on / play) |

GPIO 17 and 18 are on the 4-pin expansion connectors (GND, 3.3V, IO17, IO18).

<img src="images/connectors.png" alt="ESP32-Touchduino JC3248W535 Pinout " width="400">

### Wiring the tape output

<img src="images/line-out.png" alt="Line-out filter: IO17 through R1 1 kΩ, C1 10 nF to ground, C2 10 µF coupling capacitor and R2 4.7 kΩ to ground, to the tip of a 3.5 mm plug; REM switch between IO18 and ground" width="640">

| Part | Value | Job |
| --- | --- | --- |
| R1 | 1 kΩ | Limits current from the pin; with C1 it rounds off the sharp edges |
| C1 | 10 nF | Low-pass filter with R1 (about 16 kHz), so even the fastest turbo loaders pass |
| C2 | 10 µF electrolytic, **+ toward the board** | Blocks the 1.65 V DC offset of the 0–3.3 V signal |
| R2 | 4.7 kΩ | Holds the output at 0 V when idle |

- **Plug:** jack **tip** carries the signal and **sleeve** goes to GND. With a stereo cable, joining tip and ring is fine.
- **No amplifier needed:** GPIO 17 swings 0–3.3 V, which is already at or above the level of a cassette player's headphone output. MaxDuino and TZXDuino builds feed the EAR input directly in the same way. If a machine struggles, try a *lower* level first, for example with a 10 kΩ volume pot in place of R2 (wiper to the tip).
- **Don't connect a speaker straight to GPIO 17.** Use the board's speaker connector to listen.
- **Different machines need different leads:**
  - **ZX Spectrum:** the EAR socket.
  - **Amstrad CPC 664/6128:** the DIN tape socket. The CPC 464's built-in deck needs a cassette adaptor or a modification.
  - **BBC Micro:** the 7-pin DIN cassette socket.
  - **Commodore 64:** has no audio input, so it needs a datasette-port interface instead of this circuit.
- **Motor sense (optional):** the computer's REM output is a switch (a relay contact) that closes while the motor should run. Wire it between GPIO 18 and GND. The pin has a pull-up, so closed means play. Select **Auto REM** in Options.
- **Auto REM without a cable:** with nothing on GPIO 18, Auto REM keeps playback paused. That's why the default is Force run.

## Building and flashing

You need [PlatformIO](https://platformio.org/) (VS Code extension or CLI). The first build downloads ESP-IDF 5.2 and the toolchain, which takes a while.

```bash
pio run                  # build
pio run -t upload        # flash (board shows up as a USB serial port)
pio run -t monitor       # serial log at 115200
```

The project uses ESP-IDF with LVGL 8.4. ESP-IDF settings live in `sdkconfig.defaults`. After you change that file, delete the generated `sdkconfig.jc3248w535` so it gets rebuilt.

If an upload fails with `No serial data received` after the stub starts, unplug and replug the board, or flash with esptool's ROM loader, which is slower but more tolerant:

```bash
python ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port COM9 --baud 115200 --no-stub write_flash 0x10000 .pio/build/jc3248w535/firmware.bin
```

## Using it

1. **Prepare the card.** Format a microSD card as FAT32 (FAT16 is fine for small cards) and copy your tape files onto it. Folders are fine, for example `/Spectrum/Games/Manic Miner.tzx`. Long file names work.
   - UEF files must be uncompressed. Run `gunzip -c game.uef > game.uef.tmp` and rename the result back to `game.uef`.
2. **Browse.** Tap a folder to open it, and ◀ (or `..`) to go back. ↻ re-reads the SD card: use it after swapping cards, since the board can't detect a card change by itself. It stops playback first.
3. **Play.** Tap a file to select it and see its preview, then tap again or press ▶. ▶ toggles pause while playing and ■ stops. While a tape is playing or paused, browsing is locked and the preview stays on that tape: press ■ to choose another file. The status line shows **End of tape** when it finishes.
4. **Skip blocks.** ◀◀ and ▶▶ jump between data blocks in TZX/TSX/CDT and Spectrum TAP files.
5. **Multi-load games.** Some games load in parts, such as days, levels or sides.
   - **TZX images** often contain "stop the tape" markers, and TouchDuino pauses there; press ▶ when the game asks. "Stop if 48K" markers are ignored unless you turn off **Skip TZX block 2A**.
   - **TAP files** have no such markers and play straight through. Pause after a part has loaded and resume when the game asks for the next one, as you would with a real cassette.
6. **Options.** The ⚙ screen holds the speaker on/off switch, volume and test beep. It also has the motor mode, baud rate and the MaxDuino flags.
7. **Screenshots.** Press and hold the folder path at the top for about a second. The current screen is saved to the card as `screenshot.bmp`, and the header shows **✓ Saved screenshot.bmp**.

## How it works

The aim is that touching the screen can never disturb the tape timing.

- **Core 1:** the tape engine and its pulse timer. The timer drives the ESP32-S3 timer hardware directly from internal RAM (IRAM), so playback keeps running even while flash is busy.
- **Core 0:** the display, touch, SD browsing, cover-art decoding and the speaker task.

The display controller has to receive every update as a whole frame, starting from the top. To keep drawing cheap, LVGL redraws only the parts of the screen that changed into fast internal RAM, and these are merged into a full-frame copy that's streamed to the panel. A still screen costs nothing, and an animated border frame takes about 30 ms, mostly waiting on the display transfer.

The format code in `src/engine/` is copied from MaxDuino almost unchanged. Small compatibility layers stand in for the Arduino pieces it expects:

| Original MaxDuino | TouchDuino replacement |
| --- | --- |
| Arduino core | `Arduino.h` |
| SdFat | `file_utils.cpp` (standard C file reads with a cache) |
| AVR timers | `TimerCounter.cpp` |
| `MaxDuino.ino` main loop | `engine.cpp` |

See [CLAUDE.md](CLAUDE.md) for detailed developer notes.

| Path | Contents |
| --- | --- |
| `src/engine/` | MaxDuino engine plus ESP32 glue, the speaker output (`monitor.cpp`) and the public API (`tape_engine.h`) |
| `src/ui/` | LVGL touch UI and cover-art decoder |
| `src/app/` | SD card mounting and saved settings |
| `src/esp_bsp.*`, `esp_lcd_*`, `lv_port.*` | Board support for the display and touch |

## Credits

- **[MaxDuino](https://github.com/rcmolina/MaxDuino)** by Rafael Molina Chasserot and contributors, building on TZXDuino (Andrew Beer, Duncan Edwards) and CASDuino. TouchDuino's tape engine is their work.
- **[JC3248W535EN](https://github.com/NorthernMan54/JC3248W535EN)** by NorthernMan54, the ESP-IDF + LVGL base project for this board.
- **[LVGL](https://lvgl.io/)**, the graphics library.

MaxDuino doesn't include a licence file. Check with its authors before you redistribute builds of this firmware.
