# Sega Master System Emulator

Headless Sega Master System emulator app. Normally launched by **`sms_launcher`**; can also be started directly for testing.

## Emulator core

**TotalSMS** by ITotalJustice  
Source: `core/` (vendored from [ITotalJustice/TotalSMS](https://github.com/ITotalJustice/TotalSMS))  
Licence: MIT

Dependencies also vendored into `core/`:
- **scheduler** ([ITotalJustice/scheduler](https://github.com/ITotalJustice/scheduler) v1.1.0) — event scheduler used internally by TotalSMS
- **sn76489** ([ITotalJustice/sn76489](https://github.com/ITotalJustice/sn76489) v1.2.1) — PSG audio chip emulation (compiled in but unused; no audio hardware on the badge)

Supports Sega Master System Mode 4 games. Runs at native 256×192; the ESP32-P4 PPA hardware scaler upscales to 720×720 automatically.

> **Game Gear** ROMs are not supported in this release (requires a different system type and 160×144 crop).

## How it works

1. Receives the ROM path as `argv[1]`.
2. Loads the ROM into a heap buffer.
3. Initialises TotalSMS, sets system type to `SMS_System_SMS`, registers a colour callback that converts the SMS 2-bit-per-channel palette to RGB565.
4. Points TotalSMS directly at the framebuffer pixels via `SMS_set_pixels()` (16bpp, stride = 256 pixels).
5. Runs a `SMS_run()` loop: poll events → `SMS_set_buttons()` → run frame → present framebuffer.
6. **ESC** exits the loop and the process exits.

No audio — `SMS_set_apu_callback` is never called.

## Controls

| Key | SMS button |
|-----|-----------|
| Arrow keys / WASD | D-pad (player 1) |
| Z | Button 1 |
| X | Button 2 |
| Enter | Pause |
| ESC | Quit emulator |

## Connected apps

| App | Role |
|-----|------|
| `sms_launcher` | Scans for ROMs and spawns this app |

## Running directly (testing)

```
sms_emu.elf <path-to-rom>
# e.g.
sms_emu.elf "SD0:[ROMS.SMS]Sonic the Hedgehog.sms"
```

If launched without a ROM path the screen goes black for 3 seconds and the process exits.

## ROM files

See `sms_launcher/README.md` for SD card directory layout.
