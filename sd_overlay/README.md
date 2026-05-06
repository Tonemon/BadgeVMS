# SD Overlay — TheNewImprovedGalacticPatcher

## What this is

Old WHY2025 badges ship with a `why2025_ota` app that validates SSL certificates. When hosting an OTA server with a self-signed cert, the old OTA app refuses to connect.

This SD overlay provides **TheNewImprovedGalacticPatcher** — a separate app with a unique identifier that lives alongside `why2025_ota` rather than replacing it.

### Why a different app name

If the SD card carried a `why2025_ota` overlay, inserting it into a **new** badge would shadow the updated OTA app on flash. By using a distinct identifier, the patcher:

- **On a new badge:** appears as an unknown app in the launcher, will not load (ABI incompatibility with new firmware runtime) — that is fine, it is not needed there
- **On an old badge:** loads correctly from SD, provides full OTA functionality with SSL verification disabled

### The patch

`CURLOPT_SSL_VERIFYPEER, 0L` added to both `do_http()` and `do_firmware_http()` in `ota_update.c`, so the OTA client accepts any certificate including self-signed ones.

## SD card layout

```
(SD root)/
└── BADGEVMS/
    └── APPS/
        ├── thenewimprovedgalacticpatcher.json
        └── thenewimprovedgalacticpatcher/
            └── thenewimprovedgalacticpatcher.elf
```

## How to use

1. Format an SD card as FAT32.
2. Copy the `BADGEVMS/` directory from `sd_overlay/BADGEVMS/` to the SD card root.
3. **Old badge workflow:** insert SD card into old badge, open the launcher — TheNewImprovedGalacticPatcher appears as a new app, launch it and run the OTA update against your self-signed-cert server, remove the SD card after the update completes.
4. **New badge:** insert the same SD card without worry — the patcher app will appear in the launcher but will not overwrite or interfere with `why2025_ota`.

## Rebuilding the ELF

See `docs/working-with-old-apps.md` for the full build environment setup. The quick rebuild command (after activating the 5.5.0-era IDF):

```bash
. ~/esp/esp-idf-5.5.1/export.sh
SRC=~/Git/BadgeVMS/sd_overlay/src/thenewimprovedgalacticpatcher
riscv32-esp-elf-gcc \
    -O2 -fPIC -flto -fdata-sections -ffunction-sections \
    -fno-builtin -fno-builtin-function -fno-jump-tables \
    -fno-tree-switch-conversion -fstrict-volatile-bitfields \
    -fvisibility=hidden -g3 \
    -mabi=ilp32f -march=rv32imafc_zicsr_zifencei \
    -nostartfiles -nostdlib -shared \
    -Wl,--strip-debug -Wl,--gc-sections -e main \
    -isystem ~/Git/BadgeVMS-original/sdk_include \
    -isystem ~/Git/BadgeVMS-original/badgevms/include \
    -I $SRC \
    -o ~/Git/BadgeVMS/sd_overlay/BADGEVMS/APPS/thenewimprovedgalacticpatcher/thenewimprovedgalacticpatcher.elf \
    $SRC/main.c $SRC/window.c $SRC/ota_update.c $SRC/thirdparty/cJSON.c \
    -L ~/Git/BadgeVMS/build/sdk_staging/lib \
    -lsdl3 -Wl,--exclude-libs,libsdl3.a
riscv32-esp-elf-strip \
    ~/Git/BadgeVMS/sd_overlay/BADGEVMS/APPS/thenewimprovedgalacticpatcher/thenewimprovedgalacticpatcher.elf
```

## Source

Source lives at `sd_overlay/src/thenewimprovedgalacticpatcher/`. Original firmware source at `~/Git/BadgeVMS-original/` (commit `a548d82`, version 12). Only change vs. the original: the two `CURLOPT_SSL_VERIFYPEER, 0L` lines.
