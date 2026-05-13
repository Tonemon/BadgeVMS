# Working with old apps (ESP-IDF 5.5.0 vs 5.5.4)

## Background

The WHY2025 badge firmware was built with **ESP-IDF 5.5.0**, distributed only as the Docker image `espressif/idf:v5.5`. No git tag for 5.5.0 exists — 5.5.1 is the first tagged release. The current active firmware in `~/Git/BadgeVMS/` targets **ESP-IDF 5.5.4**.

These two versions are incompatible at the source level: the 5.5.1+ IDF split the ESP32-P4 eFuse field `wafer_version_major` (2-bit) into `wafer_version_major_lo` and `wafer_version_major_hi` (3-bit total). The original badge firmware's component override in `components/hal/esp32p4/include/hal/efuse_ll.h` still uses the old name, so it will not compile against 5.5.1+.

Both IDF installations live side by side:

| Path | Version | Toolchain | Use for |
|------|---------|-----------|---------|
| `~/esp/esp-idf` | 5.5.4 | `esp-14.2.0_20260121` | `~/Git/BadgeVMS/` |
| `~/esp/esp-idf-5.5.1` (checked out to pre-split) | 5.5.0 era | `esp-14.2.0_20241119` | `~/Git/BadgeVMS-original/` |

---

## Setting up the 5.5.0-era IDF alongside 5.5.4

### Step 1 — Clone the IDF at v5.5.1

The 5.5.0 state exists as the parent commit of the v5.5.1 tag. Clone v5.5.1 into a separate directory:

```bash
git clone --branch v5.5.1 --depth 5 https://github.com/espressif/esp-idf.git ~/esp/esp-idf-5.5.1
git -C ~/esp/esp-idf-5.5.1 submodule update --init --recursive
```

### Step 2 — Find and check out the 5.5.0 state

The split of `wafer_version_major` happened in commit `5d946e6ec`. Fetch enough history to reach the commit before that:

```bash
git -C ~/esp/esp-idf-5.5.1 fetch --depth 100 origin v5.5.1
git -C ~/esp/esp-idf-5.5.1 log --oneline components/soc/esp32p4/register/soc/efuse_struct.h
# Look for: "feat(efuse): Adds 3-bit field for wafer major version in ESP32-P4"
# That commit is 5d946e6ec; its parent is 9c4aa443b — the pre-split state.
git -C ~/esp/esp-idf-5.5.1 checkout 9c4aa443b
```

Verify you are at 5.5.0 (PATCH = 0):

```bash
grep "IDF_VERSION_PATCH" ~/esp/esp-idf-5.5.1/components/esp_common/include/esp_idf_version.h
# Should print: #define ESP_IDF_VERSION_PATCH   0
```

### Step 3 — Install the toolchain

The 5.5.0-era IDF uses toolchain `esp-14.2.0_20241119`. Run the installer from within the correct IDF:

```bash
IDF_PATH=~/esp/esp-idf-5.5.1 ~/esp/esp-idf-5.5.1/install.sh
```

This installs the toolchain alongside the existing 5.5.4 toolchain; both live under `~/.espressif/tools/riscv32-esp-elf/`.

---

## Activating each environment

### Old firmware (5.5.0-era) — for `~/Git/BadgeVMS-original/`

```bash
# Confirm the IDF repo is on the pre-split commit
git -C ~/esp/esp-idf-5.5.1 log --oneline -1
# Should show: 9c4aa443b Merge branch 'bugfix/enable_ipv6_if_nan_v5.5' ...

# Source the environment — this sets IDF_PATH and prepends the correct toolchain to PATH
. ~/esp/esp-idf-5.5.1/export.sh

# Verify
which riscv32-esp-elf-gcc   # should be .../esp-14.2.0_20241119/...
echo $IDF_PATH              # should be ~/esp/esp-idf-5.5.1
```

### New firmware (5.5.4) — for `~/Git/BadgeVMS/`

```bash
. ~/esp/esp-idf/export.sh

which riscv32-esp-elf-gcc   # should be .../esp-14.2.0_20260121/...
echo $IDF_PATH              # should be ~/esp/esp-idf
```

> **Important:** always source the export.sh in a fresh shell before building. Shell snapshots used by tools like Claude Code restore the PATH from whatever was active when the tool was invoked, which can silently override your IDF_PATH with the wrong version. Open a new terminal and source the desired export.sh first.

---

## What went wrong during setup

### Problem 1 — Shell snapshot overrides IDF_PATH

Claude Code caches the shell environment at session start. Any subsequent command inherits the cached PATH. If the 5.5.4 `export.sh` was sourced before the session started, every command in that session would find `esp-14.2.0_20260121` first in PATH even if the command explicitly set `IDF_PATH=~/esp/esp-idf-5.5.1`.

**Fix:** Start the Claude Code session (or terminal) without sourcing either `export.sh` first. Then source the correct one explicitly as needed. When running a build as a shell command, prefix it with `. ~/esp/esp-idf-5.5.1/export.sh &&` in the same subshell.

### Problem 2 — Toolchain version mismatch error

When the wrong toolchain version is on PATH, CMake will abort with:

```
CMake Error: Tool doesn't match supported version from list ['esp-14.2.0_20241119']:
  /home/.../.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/.../riscv32-esp-elf-gcc
```

This is CMake's toolchain version guard. The mismatch can go either direction depending on which IDF is active vs. which toolchain is on PATH.

**Fix:** ensure the correct `export.sh` is sourced before invoking `idf.py`.

### Problem 3 — Stale CMake cache

If a previous build ran with a different IDF and left a `build/` directory, CMake will pick up cached paths from that earlier run.

**Fix:**

```bash
cd ~/Git/BadgeVMS-original
rm -rf build
idf.py build
```

### Problem 4 — `env -i` strips the Python virtual environment

Attempting to use `env -i` to create a clean environment for `idf.py` strips the Python venv paths that `esp_idf_monitor` and other IDF Python tools require, causing:

```
No module named 'esp_idf_monitor'
```

**Fix:** do not use `env -i`. Source `export.sh` in a normal shell instead.

### Problem 5 — Internal IDF inconsistencies at some `release/v5.5` commits

The `release/v5.5` branch is a merge-integration branch. At certain merge commits, feature branches are partially integrated, producing a state where one component calls a function that hasn't been merged yet. For example, at `9c4aa443b`, `parlio_tx.c` calls `gdma_link_get_buffer()` which is declared in a branch merged later at `3c39b3219`.

This means a full `idf.py build` of `~/Git/BadgeVMS-original/` against `9c4aa443b` fails around step 585/1387 with:

```
error: implicit declaration of function 'gdma_link_get_buffer'
```

**Fix (used):** skip the full IDF build and compile only the app ELF directly with the cross-compiler. The BadgeVMS app ELF does not depend on the IDF firmware build at all — it only needs the cross-compiler, the C runtime headers (`sdk_include/`), the BadgeVMS SDK headers (`badgevms/include/`), and the pre-built `libsdl3.a`.

---

## Compiling an old-firmware app ELF directly

This is how `TheNewImprovedGalacticPatcher` was built. No full IDF firmware build required.

### Prerequisites

- 5.5.0-era IDF sourced (for the `esp-14.2.0_20241119` cross-compiler)
- `~/Git/BadgeVMS-original/` cloned from `https://gitlab.com/why2025/team-badge/firmware`
- A pre-built `libsdl3.a` from a successful `~/Git/BadgeVMS/` build at `build/sdk_staging/lib/libsdl3.a`

### Compile command

```bash
. ~/esp/esp-idf-5.5.1/export.sh   # activate 5.5.0-era toolchain

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

### Why this works

BadgeVMS app ELFs are position-independent RISC-V shared objects. The ELF loader in the firmware resolves symbols at runtime against the firmware's own export table (the BadgeVMS API). The compile-time ABI contract is defined entirely by:

- The RISC-V ISA (`rv32imafc_zicsr_zifencei`) and calling convention (`ilp32f`) — identical between old and new toolchain versions
- The BadgeVMS API headers (`badgevms/include/`) — same interface in old and new firmware
- The SDL3 library — compiled from the same source, same ABI

So an ELF compiled against the old headers with the old toolchain is compatible with the old badge firmware's runtime.

---

## Running `idf.py build` for each repo

If you ever do need to build the full firmware (not just an app ELF):

### `~/Git/BadgeVMS/` (5.5.4)

```bash
cd ~/Git/BadgeVMS
. ~/esp/esp-idf/export.sh
idf.py build
```

### `~/Git/BadgeVMS-original/` (5.5.0-era)

> **Note:** a full build of `BadgeVMS-original` currently fails at step ~585/1387 due to the `gdma_link_get_buffer` inconsistency described above. For building old apps, use the direct compile command instead.

If a future IDF commit resolves the inconsistency, the full build would be:

```bash
cd ~/Git/BadgeVMS-original
git -C ~/esp/esp-idf-5.5.1 checkout 9c4aa443b   # ensure pre-split commit
. ~/esp/esp-idf-5.5.1/export.sh
rm -rf build
idf.py build
```
