# Start Random Game

Picks a random ROM from the SD card across all supported systems and launches the appropriate emulator.

## How it works

1. Scans all three ROM directories (NES, Game Boy, Master System) using `shared/rom_browser`.
2. Builds a list of systems that have at least one ROM.
3. Picks a random system, then a random ROM from that system.
4. Displays the system name and ROM filename for 1 second.
5. Spawns the matching emulator with the ROM path as `argv[1]` and waits for it to exit.
6. If no ROMs are found on any system, shows an error message for 3 seconds and exits.

## Connected apps

| App | Role |
|-----|------|
| `nes_emu` | Launched when a NES ROM is selected |
| `gb_emu` | Launched when a Game Boy ROM is selected |
| `sms_emu` | Launched when a Master System ROM is selected |

## ROM files

ROMs must be in the following directories on the SD card:

```
SD card root/
├── ROMS.NES/
│   ├── Super Mario Bros.nes
│   └── ...
├── ROMS.GB/
│   ├── Tetris.gb
│   ├── Pokemon Red.gb
│   └── ...
└── ROMS.SMS/
    ├── Sonic the Hedgehog.sms
    └── ...
```

Any combination of systems works — if only one system has ROMs, it always picks from that one. Systems with no ROMs are excluded from the random draw.
