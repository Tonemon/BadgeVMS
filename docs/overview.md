# BadgeVMS — WHY2025 Badge Firmware

BadgeVMS is the badge OS for the WHY2025 hacker camp. It runs on an ESP32P4-based badge and provides a multi-process environment where each program gets its own isolated virtual address space.

**License**: GPL v3  
**Build system**: ESP-IDF 5.5 + CMake (target: esp32p4)  
**Build command**: `idf.py build flash monitor`  
**After `git pull`**: always run `idf.py fullclean` to pick up sdkconfig.defaults changes.

## Hardware

| Device | Chip/Part | BadgeVMS name |
|--------|-----------|---------------|
| Main SoC | ESP32P4 | — |
| WiFi coprocessor | ESP32C6 (runs esp-hosted slave) | WIFI0 |
| Display | ST7703 720×720 MIPI DSI, 60 Hz, RGB565 | PANEL0 |
| Keyboard controller | TCA8418 (I2C) | KEYBOARD0 |
| IMU | Bosch BMI270 6-axis (I2C) | ORIENTATION0 |
| Gas/env sensor | Bosch BME690 (I2C) | GAS0 |
| I2C bus | I2C_NUM_0 @ 100 kHz | I2CBUS0 |
| SPI Flash (FAT) | ~11.6 MB FAT partition | FLASH0 |
| SD card (optional) | SD card | SD0 |
| TTY (serial) | UART/USB | TT01 |
| Sockets | Via WIFI0/esp-hosted | SOCKET0 |

PSRAM: 32 MB total. Note: PSRAM runs at 200 MHz but some units get single-bit errors under heavy load — no workaround.

## Repo Layout

```
firmware/
├── badgevms/           # Main kernel — ESP-IDF component (the OS itself)
│   ├── include/badgevms/   # Public API headers (for kernel + apps)
│   ├── compositor/     # Windowing system
│   ├── drivers/        # Hardware drivers (st7703, tca8418, wifi, fatfs, etc.)
│   └── thirdparty/     # cJSON, dlmalloc, khash, tomlc17
├── components/         # Custom ESP-IDF components (bme690, bmi270, elf_loader, etc.)
├── connectivity_esp_hosted/slave/  # ESP32C6 WiFi firmware (built separately)
├── flash_storage/      # Files burned to FAT flash partition
│   └── skel/init.toml  # Default init config (embedded in firmware binary)
├── sdk_apps/           # Example & official userspace apps
├── sdk_include/        # Newlib libc + SDL2 + SDL3 headers (for app devs)
├── sdk_libs/           # CMake wrappers for SDL2/SDL3 libraries
├── host_tests/         # Linux native unit tests (run as ExternalProject)
├── partitions.csv      # Flash partition table
└── sdkconfig.defaults  # Default Kconfig values
```

## Flash Partition Table

| Partition | Type | Size | Offset |
|-----------|------|------|--------|
| nvs | NVS data | 16 KB | 0x9000 |
| otadata | OTA data | 8 KB | 0xd000 |
| ota_0 | App firmware | 2 MB | auto |
| ota_1 | App firmware | 2 MB | auto |
| storage | FAT data | ~11.6 MB | auto |

Dual OTA partitions allow safe over-the-air updates; the init system marks the partition valid after successful boot.

## Build Environment Setup

The ESP-IDF toolchain is installed at `~/esp/esp-idf` but is **not on the default PATH**. Before running any `idf.py` command in a fresh shell, source the export script:

```bash
. $HOME/esp/esp-idf/export.sh
```

This adds `idf.py`, the RISC-V GCC cross-compiler, and all ESP-IDF Python tools to your session. You only need to do this once per shell session.

## Building SDK Apps Without Full Firmware Rebuild

The full `idf.py build` compiles the ESP32P4 firmware, the ESP32C6 WiFi slave firmware, all SDK libraries, and all SDK apps in one shot. If Espressif-side components have build errors (e.g. `hal/efuse_ll.h` referencing removed struct members after an IDF update), the full build will fail before reaching the SDK apps.

Since SDK apps are compiled as independent RISC-V ELF shared libraries with their own compiler flags, they can be built in isolation via ninja without touching the main firmware:

```bash
ninja -C /path/to/firmware/build \
  /path/to/firmware/build/sdk_apps/CMakeFiles/build_app_<appname>
```

For example, to build just `badgevms_launcher`:

```bash
ninja -C ~/Git/firmware/build \
  ~/Git/firmware/build/sdk_apps/CMakeFiles/build_app_badgevms_launcher
```

**When to use this:** Any time you are iterating on an SDK app and the full `idf.py build` fails due to errors in Espressif components that are unrelated to your changes. The ninja target builds only the app and its SDK library dependencies.

**Prerequisite:** The build directory must have been configured at least once with `idf.py build` (even if it failed partway). If the build directory is missing or corrupted, run `idf.py build` once to regenerate the CMake/ninja files before using the targeted ninja command.

**Warning:** Do not run `cmake` directly in the build directory — the project uses the Ninja generator, and invoking cmake with a different generator (e.g. Unix Makefiles) will corrupt the build directory state.

## Path Conventions

BadgeVMS uses **VMS-style paths**, NOT Unix paths:
- Format: `DEVICE:[directory.subdirectory]filename.ext`
- Example: `FLASH0:[BADGEVMS.APPS]hello.elf`
- Logical names act as aliases: `APPS:` → `FLASH0:[BADGEVMS.APPS]` (or SD0 if present)
- `STORAGE:` resolves to SD0 first, then FLASH0
- **Unix paths do not work** anywhere in BadgeVMS
