# NES Launcher

User-facing app that scans the SD card for NES ROMs and lets you pick one to play.

## How it works

1. Opens a fullscreen ROM browser listing every `.nes` file found in `SD0:[ROMS.NES]`.
2. Use **Up / Down** (or **W / S**) to navigate, **Enter** to launch, **R** to reload the list, **ESC** to exit.
3. On selection it spawns **`nes_emu`** with the ROM path as `argv[1]`, then waits for it to exit.
4. When the emulator quits the launcher's window reappears and you return to the ROM list.

## Connected apps

| App | Role |
|-----|------|
| `nes_emu` | Runs the selected ROM — launched as a child process |
| `shared/rom_browser` | Shared ROM-picker UI compiled into this launcher |

## ROM files

Copy your `.nes` ROM files to the SD card:

```
SD card root/
└── ROMS/
    └── NES/
        ├── Super Mario Bros.nes
        ├── Mega Man 2.nes
        └── ...
```

> The VMS path `SD0:[ROMS.NES]` maps to `ROMS/NES/` on the SD card — the dot is a directory separator in VMS notation.
> Files are listed alphabetically. The launcher ignores subdirectories.
