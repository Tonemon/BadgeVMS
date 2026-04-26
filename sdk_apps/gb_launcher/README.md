# Game Boy Launcher

User-facing app that scans the SD card for Game Boy ROMs and lets you pick one to play.

## How it works

1. Opens a fullscreen ROM browser listing every `.gb` and `.gbc` file found in `SD0:[ROMS.GB]`.
2. Use **Up / Down** (or **W / S**) to navigate, **Enter** to launch, **ESC** to exit.
3. On selection it spawns **`gb_emu`** with the ROM path as `argv[1]`, then waits for it to exit.
4. When the emulator quits the launcher's window reappears and you return to the ROM list.

## Connected apps

| App | Role |
|-----|------|
| `gb_emu` | Runs the selected ROM — launched as a child process |
| `shared/rom_browser` | Shared ROM-picker UI compiled into this launcher |

## ROM files

Copy your `.gb` (Game Boy) or `.gbc` (Game Boy Color) ROM files to the SD card:

```
SD card root/
└── ROMS.GB/
    ├── Tetris.gb
    ├── Pokemon Red.gb
    ├── Pokemon Crystal.gbc
    └── ...
```

> The directory is `ROMS.GB` at the root of the SD card (VMS path `SD0:[ROMS.GB]`).
> Both `.gb` and `.gbc` extensions are listed together, sorted alphabetically.

Note: only original DMG (Game Boy) games are supported in this release — GBC colour mode is not yet implemented.
