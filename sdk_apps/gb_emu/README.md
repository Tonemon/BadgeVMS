# Game Boy Emulator

Headless Game Boy emulator app. Normally launched by **`gb_launcher`**; can also be started directly for testing.

## Emulator core

**Peanut-GB** by deltabeard  
Source: `peanut_gb.h` (single-header library, vendored from [deltabeard/Peanut-GB](https://github.com/deltabeard/Peanut-GB))  
Licence: MIT

Supports original Game Boy (DMG) games. Runs at native 160×144; the ESP32-P4 PPA hardware scaler upscales to 720×720 automatically.

> **GBC colour mode** is not implemented in this version — `.gbc` ROMs load but render in DMG green.

## How it works

1. Receives the ROM path as `argv[1]`.
2. Loads the ROM into a heap buffer.
3. Initialises peanut-gb with ROM/RAM callbacks and registers `lcd_draw_line` as the scanline renderer.
4. If the cartridge has battery-backed RAM, allocates it and loads a `.sav` file from the same directory as the ROM (e.g. `Pokemon Red.gb.sav`).
5. Runs a `gb_run_frame()` loop: poll events → update `g_gb.direct.joypad` → run frame → present framebuffer.
6. **ESC** exits the loop; the `.sav` file is written before the process exits.

No audio — the badge has no audio hardware (`ENABLE_SOUND` is not defined).

## Controls

| Key | GB button |
|-----|----------|
| Arrow keys / WASD | D-pad |
| Z | A |
| X | B |
| Enter | Start |
| Right Shift | Select |
| ESC | Quit emulator |

## Battery save RAM

Games with SRAM (e.g. Pokémon) automatically save to a `.sav` file next to the ROM on the SD card:

```
SD0:[ROMS.GB]Pokemon Red.gb      ← ROM
SD0:[ROMS.GB]Pokemon Red.gb.sav  ← save file (created automatically)
```

## Connected apps

| App | Role |
|-----|------|
| `gb_launcher` | Scans for ROMs and spawns this app |

## Running directly (testing)

```
gb_emu.elf <path-to-rom>
# e.g.
gb_emu.elf "SD0:[ROMS.GB]Tetris.gb"
```

If launched without a ROM path the screen goes black for 3 seconds and the process exits.

## ROM files

See `gb_launcher/README.md` for SD card directory layout.
