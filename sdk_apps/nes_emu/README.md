# NES Emulator

Headless NES emulator app. Normally launched by **`nes_launcher`**; can also be started directly for testing.

## Emulator core

**nofrendo** — retro-go fork by ducalex  
Source: `nofrendo/` (vendored from [ducalex/retro-go](https://github.com/ducalex/retro-go), `components/nofrendo/`)  
Licence: GPLv2

Supports ~63 mappers (iNES format). Runs at native 256×240; the ESP32-P4 PPA hardware scaler upscales to 720×720 automatically.

## How it works

1. Receives the ROM path as `argv[1]`.
2. Initialises nofrendo with a blit callback; calls `nes_setvidbuf()` to point nofrendo at an internal bitmap buffer.
3. Loads an RGB565 palette via `nofrendo_buildpalette()`.
4. Runs `nofrendo_start()` — this is the blocking emulation loop.
5. Each frame the blit callback polls events, updates joypad state via `input_update()`, converts the indexed bitmap to RGB565, and presents the framebuffer.
6. **ESC** calls `nofrendo_stop()` and the process exits.

No audio — the badge has no audio hardware.

## Controls

| Key | NES button |
|-----|-----------|
| Arrow keys / WASD | D-pad |
| Z | A |
| X | B |
| Enter | Start |
| Right Shift | Select |
| ESC | Quit emulator |

## Connected apps

| App | Role |
|-----|------|
| `nes_launcher` | Scans for ROMs and spawns this app |

## Running directly (testing)

```
nes_emu.elf <path-to-rom>
# e.g.
nes_emu.elf "SD0:[ROMS.NES]Super Mario Bros.nes"
```

If launched without a ROM path the screen goes black for 3 seconds and the process exits.

## ROM files

See `nes_launcher/README.md` for SD card directory layout.
