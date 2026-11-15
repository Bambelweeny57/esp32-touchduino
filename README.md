![ESP32-Touchduino Banner](images/banner.jpg)

# TouchDuino

An ESP32-S3 port of [MaxDuino](https://github.com/rcmolina/MaxDuino), the tape emulator for 1980s home computers. It has a 3.5" touch screen in place of physical buttons. Put tape images on a microSD card, pick one on the screen, and TouchDuino plays it as a cassette signal for your Spectrum, Amstrad CPC, BBC Micro, MSX and other machines to load.

It runs on the Guition **JC3248W535** board:
- ESP32-S3 with 16 MB flash and 8 MB PSRAM
- 320×480 QSPI touch display
- microSD slot
- built-in speaker amplifier

<img src="images/lcd-preview.jpg" alt="ESP32-Touchduino LCD Interface" width="400">

> **Status:** early, but working. The firmware builds, boots, mounts the SD card and drives the display and touch. Playback through the speaker and into real machines is being tested now.

## Features

- **Supported formats:** TZX, TAP, CDT, TSX, UEF, CAS, P, O, AY, MZF/MZT/M12, MTX, CAQ and Commodore 64 TAP. These use MaxDuino's format code, so timing matches the original.
- **Touch controls:** previous/next file, block back/forward, play/pause and stop, all as large buttons. There's also a progress bar and a block counter.
- **File browser:** folders are listed first. It remembers the last folder you used.
- **Cover art:** if `game.tzx` has a `game.jpg` or `game.png` next to it, that image is shown. Failing that:
  - it looks for `folder.jpg` or `folder.png` in the folder or any parent folder;
  - if there's none, a folder named `Spectrum`, `Sinclair`, `Amstrad`, `CPC`, `BBC`, `MSX` or `Dragon` gets a system badge.
- **Speaker:** the tape signal is mirrored to the onboard speaker, so you can hear it load. Volume is adjustable and there's a test beep.
- **Line out:** GPIO 17 outputs the signal for a 3.5 mm jack to the computer's EAR/tape input.
- **Motor control (REM):** in **Auto REM**, the computer's motor line on GPIO 18 pauses and resumes playback. In **Force run** the line is ignored.
- **Options:** speed for TSX/CAS/CDT (1200–3850 baud), MaxDuino's polarity/parity flag and "skip block 2A". Settings are kept after power-off.

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

### Wiring the tape output

- **Don't connect a speaker straight to GPIO 17.** Use the board's speaker connector to listen.
- **Line out:** feed the computer's EAR/tape input from GPIO 17 through a simple RC filter. A good starting point is a 1 kΩ resistor in series with a 10 nF capacitor to GND, then optionally a 10 µF coupling capacitor to the jack tip. Jack sleeve goes to GND.
- **Motor sense:** wire the computer's REM output so it pulls GPIO 18 to GND when the motor should run. The pin has a pull-up.
- **Auto REM without a cable:** with nothing on GPIO 18, Auto REM keeps playback paused. That's why the default is Force run.

## Building and flashing

You need [PlatformIO](https://platformio.org/) (VS Code extension or CLI). The first build downloads ESP-IDF 5.2 and the toolchain, which takes a while.

```bash
pio run                  # build
pio run -t upload        # flash (board shows up as a USB serial port)
pio run -t monitor       # serial log at 115200
```

The project uses ESP-IDF with LVGL 8.4. ESP-IDF settings live in `sdkconfig.defaults`. After you change that file, delete the generated `sdkconfig.jc3248w535` so it gets rebuilt.

## Using it

1. **Prepare the card.** Format a microSD card as FAT32 (FAT16 is fine for small cards) and copy your tape files onto it. Folders are fine, for example `/Spectrum/Games/Manic Miner.tzx`. Long file names work.
   - UEF files must be uncompressed. Run `gunzip -c game.uef > game.uef.tmp` and rename the result back to `game.uef`.
2. **Browse.** Tap a folder to open it, ◀ (or `..`) to go back, and ↻ to rescan, for example after swapping cards.
3. **Play.** Tap a file to select it and see its preview, then tap again or press ▶. ▶ toggles pause while playing and ■ stops.
4. **Skip blocks.** ◀◀ and ▶▶ jump between data blocks in TZX/TSX/CDT and Spectrum TAP files.
5. **Options.** The ⚙ screen holds the speaker on/off switch, volume and test beep. It also has the motor mode, baud rate and the MaxDuino flags.

## How it works

The aim is that touching the screen can never disturb the tape timing.

- **Core 1:** the tape engine and its pulse timer. The timer drives the ESP32-S3 timer hardware directly from internal RAM (IRAM), so playback keeps running even while flash is busy.
- **Core 0:** the display, touch, SD browsing, cover-art decoding and the speaker task.

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
