# OTA development environment — two badges, two monitors

This document describes how to connect both a new badge (running the current firmware from `~/Git/BadgeVMS/`) and an old badge (original WHY2025 firmware) at the same time, each with its own `idf.py monitor` instance in a separate terminal.

## Overview

Each badge connects via USB and exposes a serial port. Because the two badges run different firmware built with different IDF versions, each monitor must be launched from the correct project directory with the correct IDF environment sourced. The two instances are fully independent.

## Identifying which serial port belongs to which badge

When a badge is connected, the kernel assigns it a device node. Plug in one badge at a time and note which device appears:

```bash
# Watch for new device nodes as you plug each badge in
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null

# Or watch dmesg for the moment of connection
dmesg | tail -5
```

The badge uses a CH340-series USB-serial chip and appears as `/dev/ttyUSB*`. If both badges are already connected you can identify which is which by unplugging one and seeing which node disappears.

For a stable reference that survives reboots and re-plugging, use the by-id symlinks:

```bash
ls -la /dev/serial/by-id/
```

Each entry encodes the USB vendor/product ID and serial number, so two badges of the same hardware will have different serial number suffixes if they have unique serial numbers burned in.

## Terminal 1 — new badge (ESP-IDF 5.5.4)

Open a fresh terminal. Do not source any IDF environment beforehand.

```bash
. ~/esp/esp-idf/export.sh
cd ~/Git/BadgeVMS
idf.py monitor -p /dev/ttyUSB0   # replace with the actual port for the new badge
```

`idf.py monitor` will use the ELF from `build/badgevms.elf` to decode addresses in panic traces and log output. If the build directory is missing, run `idf.py build` first.

## Terminal 2 — old badge (ESP-IDF 5.5.0-era)

Open a second fresh terminal.

```bash
. ~/esp/esp-idf-5.5.1/export.sh
cd ~/Git/BadgeVMS-original
idf.py monitor -p /dev/ttyUSB1   # replace with the actual port for the old badge
```

`~/Git/BadgeVMS-original/` does not currently have a complete build (the full firmware build fails due to an IDF internal inconsistency at the `9c4aa443b` commit — see `docs/working-with-old-apps.md`). Without a matching ELF, `idf.py monitor` will still show all serial output but will not decode addresses in stack traces. That is fine for observing OTA progress and log messages.

If address decoding is needed in the future, an alternative is to point `--elf-file` at a locally obtained original firmware ELF, or use a plain serial terminal:

```bash
# Minimal alternative — no IDF needed, raw serial output
screen /dev/ttyUSB1 115200
# Exit screen with: Ctrl-A then K
```

## Workflow for OTA testing

1. Connect both badges via USB.
2. Identify which port is which (e.g. new badge = `/dev/ttyUSB0`, old badge = `/dev/ttyUSB1`).
3. Open Terminal 1, source 5.5.4, start monitor on the new badge port.
4. Open Terminal 2, source 5.5.0-era, start monitor on the old badge port.
5. On the old badge, insert the SD card with `TheNewImprovedGalacticPatcher` and launch it from the launcher.
6. Watch Terminal 2 for connection attempts and OTA progress from the old badge.
7. Watch Terminal 1 for output from the OTA server side (once a server app exists on the new badge).

## Important: always open a fresh terminal per badge

Sourcing one IDF's `export.sh` and then the other in the same shell will leave PATH in an unpredictable state. Each terminal should source exactly one `export.sh` for its intended badge and never mix environments.

If you use a terminal multiplexer like tmux, create a new window or pane and source the environment there rather than sourcing in an existing pane that already has one active.
