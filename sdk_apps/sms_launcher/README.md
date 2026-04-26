# Sega Master System Launcher

User-facing app that scans the SD card for Sega Master System ROMs and lets you pick one to play.

## How it works

1. Opens a fullscreen ROM browser listing every `.sms` and `.bin` file found in `SD0:[ROMS.SMS]`.
2. Use **Up / Down** (or **W / S**) to navigate, **Enter** to launch, **ESC** to exit.
3. On selection it spawns **`sms_emu`** with the ROM path as `argv[1]`, then waits for it to exit.
4. When the emulator quits the launcher's window reappears and you return to the ROM list.

## Connected apps

| App | Role |
|-----|------|
| `sms_emu` | Runs the selected ROM — launched as a child process |
| `shared/rom_browser` | Shared ROM-picker UI compiled into this launcher |

## ROM files

Copy your `.sms` or `.bin` ROM files to the SD card:

```
SD card root/
└── ROMS.SMS/
    ├── Sonic the Hedgehog.sms
    ├── Alex Kidd in Miracle World.sms
    ├── somegame.bin
    └── ...
```

> The directory is `ROMS.SMS` at the root of the SD card (VMS path `SD0:[ROMS.SMS]`).
> Both `.sms` and `.bin` extensions are listed together, sorted alphabetically.

Note: only Sega Master System ROMs are supported. Game Gear ROMs are not supported in this release.
