# Gaming & Emulation System Design

Design for NES, Game Boy / Game Boy Color, and Sega Master System emulation on the WHY2025 badge (ESP32-P4 / BadgeVMS).

## Goals

- Run NES, GB/GBC, and SMS games from ROMs stored on the SD card.
- One visible launcher app per gaming system in the Games folder.
- Separate emulator apps that accept a ROM path via `argv[1]`, enabling composition (random game picker, future systems, etc.).
- No audio (badge has no audio hardware).
- Open source throughout; licence compatible with BadgeVMS (GPLv3).
- Arcade mini-games (Pong, Breakout, etc.) are out of scope for this release.

---

## Emulator Cores

| System | Core | Licence | Key properties |
|--------|------|---------|----------------|
| NES | nofrendo (retro-go fork, ducalex/retro-go) | GPLv2 | C, ~10 KB BSS singletons, 63 mappers, proven on ESP32-C3 RISC-V at 60 fps |
| GB / GBC | peanut-gb (deltabeard/Peanut-GB) | MIT | Single-header C, zero globals (struct-based), native GBC support, audio omitted at compile time |
| SMS | TotalSMS (ITotalJustice/TotalSMS) | MIT | C, zero globals (struct-based), ~60–80 KB heap, APU callback simply not registered |

Research notes for each core: see `docs/emulator-core-nes-nofrendo.md`, `docs/emulator-core-gb-peanutgb.md`, `docs/emulator-core-sms-totalsms.md`.

---

## Architecture

### Six apps total

Three launcher apps (user-facing, visible in Games folder) and three emulator apps (visible for now; to be hidden in a future release once the hide-apps feature lands).

| App | Visible | Role |
|-----|---------|------|
| `nes_launcher` | yes | Scans `SD0:[ROMS.NES]`, presents ROM list, spawns `nes_emu` |
| `nes_emu` | yes (for now) | Accepts ROM path via `argv[1]`, runs nofrendo |
| `gb_launcher` | yes | Scans `SD0:[ROMS.GB]`, presents ROM list, spawns `gb_emu` |
| `gb_emu` | yes (for now) | Accepts ROM path via `argv[1]`, runs peanut-gb |
| `sms_launcher` | yes | Scans `SD0:[ROMS.SMS]`, presents ROM list, spawns `sms_emu` |
| `sms_emu` | yes (for now) | Accepts ROM path via `argv[1]`, runs TotalSMS |

### Shared ROM browser code

A `shared/rom_browser.c` component is compiled into all three launcher apps (following the same shared-sources pattern already used in the repo). It handles SD scanning, the scrollable list UI, and returns the selected VMS path. Each launcher configures it with its ROM directory, file extensions, and title. It also exposes a scan-only helper (no UI) for future use by the "Start Random Game" app.

### ROM directories on SD card

| System | Directory | Extensions |
|--------|-----------|------------|
| NES | `SD0:[ROMS.NES]` | `.nes` |
| GB / GBC | `SD0:[ROMS.GB]` | `.gb`, `.gbc` |
| SMS | `SD0:[ROMS.SMS]` | `.sms`, `.bin` |

### Data flow for playing a game

1. User opens "NES Launcher" from the Games folder.
2. ROM browser scans `SD0:[ROMS.NES]` and displays a sorted, scrollable file list.
3. User selects a ROM with Enter (ESC exits the launcher).
4. Launcher calls `process_create` on `nes_emu` with the ROM path as `argv[1]`, then `wait()`s.
5. `nes_emu` creates a fullscreen window (which appears on top of the launcher's maximized window) and runs the game.
6. User presses ESC → emulator exits, its window is destroyed.
7. `wait()` returns in the launcher → launcher's window is visible again → back to ROM list.

Launchers use `WINDOW_FLAG_MAXIMIZED` (not fullscreen) so the emulator's `WINDOW_FLAG_FULLSCREEN` window can appear on top without conflict. The compositor allows only one fullscreen window at a time.

---

## Shared ROM Browser

The ROM browser component has two entry points:

- **`rom_browser_run()`** — full UI: scan, draw list, handle keyboard, return selected path. Returns `NULL` if user pressed ESC. Caller frees the returned string.
- **`rom_browser_scan()`** — scan only, no UI. Returns a list of matching file paths. Used by the future "Start Random Game" app.

Behaviour:
- Scans the configured ROM directory using `opendir`/`readdir` from the BadgeVMS VFS.
- Filters by file extension (case-insensitive).
- Displays a scrollable list in the CDE visual style matching `badgevms_launcher`.
- Keyboard: Up/Down arrows scroll; Enter selects; ESC cancels.
- If SD card absent or no matching files found: shows a "No ROMs found" message with instructions.
- File list sorted alphabetically.

---

## Emulator Apps

### Window and framebuffer pattern

Each emulator creates a fullscreen window at 720×720, then creates a native-resolution framebuffer via `window_framebuffer_create`. The ESP32-P4 PPA hardware scaler upscales the native framebuffer to the window size automatically — the same approach Doom uses. No manual scaling code needed in the platform shim.

| System | Native resolution | PPA quirk safe? |
|--------|-------------------|-----------------|
| NES | 256×240 | Yes — 240 % 32 = 16 |
| GB / GBC | 160×144 | Yes — 144 % 32 = 16 |
| SMS | 256×192 | Yes — 192 % 32 = 0 |

The PPA hardware crashes if `height > 32 && (height % 32) == 1`. All three native resolutions are safe. The SMS 224-line extended display mode (224 % 32 = 0) is also safe.

### No-ROM error case

When an emulator app is launched directly from the main launcher without a ROM path (i.e. `argv[1]` is absent), it shows a brief error screen — "No ROM specified. Please use NES Launcher." — waits a few seconds, and exits cleanly.

### Key mapping (all systems)

| Key | NES | GB | SMS |
|-----|-----|----|-----|
| Arrow keys | D-pad | D-pad | D-pad |
| WASD | D-pad (alt) | D-pad (alt) | D-pad (alt) |
| Z | A | A | Button 1 |
| X | B | B | Button 2 |
| Enter | Start | Start | Start |
| Right Shift | Select | Select | — |
| ESC | Exit | Exit | Exit |

### `nes_emu` — nofrendo (retro-go fork)

- Platform shim implements the `osd.h` interface (6 functions).
- Points nofrendo's video output at the native-resolution framebuffer (RGB565).
- Drives the emulator tick inside a main loop — same pattern as doomgeneric.
- Audio never registered (`osd_setsound(NULL)`); APU code compiles in but never executes.
- ROM loaded from `argv[1]` via standard file I/O; mapper auto-detected from iNES header.

### `gb_emu` — peanut-gb

- Platform shim implements the peanut-gb callback interface.
- `lcd_draw_line` callback writes each of the 144 scanlines into the native framebuffer.
- Battery save RAM backed by a `.sav` file alongside the ROM on SD.
- `ENABLE_SOUND` not defined — zero audio code compiled in.
- GB and GBC both handled automatically from the ROM header.

### `sms_emu` — TotalSMS

- Platform shim points TotalSMS directly at the native framebuffer (RGB565).
- Colour callback converts SMS palette entries to RGB565 at init.
- APU callback not registered — no audio.
- Extensible to Game Gear (`.gg`) later by changing the system type passed to `SMS_loadrom`.

---

## Launcher App Updates

### `apps.json`

All six new apps added to the Games folder alongside doomgeneric.

### `init.toml`

No changes. The emulator apps are not auto-started at boot.

---

## Future: "Start Random Game" App

A seventh app can be added with zero changes to any existing emulator app. It uses the `rom_browser_scan()` helper to collect all ROMs across all three directories, picks one at random, and launches the corresponding emulator with the ROM path as `argv[1]`. The clean `argv[1]` interface makes this trivially composable.

---

## Future: Hide Apps Feature

Add a `"hidden": true` field to `manifest.json`. The launcher's scan thread skips apps with this flag set. Emulator app manifests are written now without the flag; the future release flips it to `true` for `nes_emu`, `gb_emu`, and `sms_emu` — one-line change per manifest.

---

## Out of Scope (this release)

- Audio.
- SNES, GBA, Sega Genesis emulation.
- Arcade mini-games — future standalone apps.
- Save states (beyond GB battery save RAM).
- In-emulator pause menu.
- Hide-apps launcher feature.
