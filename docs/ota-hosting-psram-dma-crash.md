# OTA Hosting: PSRAM Code Corruption Crash Investigation

## Summary

After the dlmalloc SAFE UNLINK crash was fixed (see `ota-hosting-tls-crash.md`), a new
random crash appeared. The new badge crashes in `event_handler` at `wifi.c:264` with an
`Illegal instruction` exception when the WiFi event handler calls the registered
`s_ap_sta_joined_cb` function pointer.

The callback pointer itself is valid — it correctly points to `ap_sta_joined` at its
runtime address in PSRAM. What is corrupted is **the code at that address**: the first
instruction of `ap_sta_joined` has been overwritten with `0x0000BAD0`, a
`C.FSD` (compressed double-precision float store) instruction that is illegal on
ESP32-P4 because it does not implement the RISC-V `D` extension.

**Current hypothesis:** The hardware MPI accelerator (`CONFIG_MBEDTLS_HARDWARE_MPI=y`),
which remains active even with ECC acceleration disabled, issues a misconfigured GDMA
transfer during mbedTLS big-number operations (used by the P-256 key generation inside
`tls_generate_selfsigned`), writing intermediate computation data to the wrong PSRAM
address — the one occupied by `ap_sta_joined`.

A secondary manifestation is a WDT reset (`rst:0x7`) with no backtrace, which occurs
when the same DMA write corrupts the heap free-list instead of code, causing dlmalloc
to call `abort()`, leaving the heap mutex permanently locked, starving the IDLE task.

---

## Crash Signatures

### Crash 1 — Illegal Instruction in WiFi Event Task

```
Guru Meditation Error: Core  0 panic'ed (Illegal instruction). Exception was unhandled.
MEPC    : 0x4a095ff8  RA      : 0x40034e60
MCAUSE  : 0x00000002  MTVAL   : 0x0000bad0
--- 0x40034e60: event_handler at .../badgevms/drivers/wifi.c:256

#0  0x4a095ff8 in ?? ()
#1  0x40034e60 in event_handler (...) at .../badgevms/drivers/wifi.c:264
```

`wifi.c:264` is `s_ap_sta_joined_cb(ev->mac, ip_str)` inside the
`IP_EVENT_AP_STAIPASSIGNED` handler — called when the old badge receives a DHCP lease.

### Crash 2 — HP System WDT Reset (no backtrace)

```
rst:0x7 (HP_SYS_HP_WDT_RESET)
--- Error: device reports readiness to read but returned no data
```

Occurs after a variable delay (seconds to minutes). No panic log is produced; the device
simply stops feeding the WDT and resets.

---

## Root Cause Analysis

### Locating `ap_sta_joined` at runtime

The OTA app ELF contains:

```
$ nm build/app_elfs/why2025_ota.elf | grep ap_sta_joined
00004b58 t ap_sta_joined
```

`ap_sta_joined` is at compile-time offset `0x4b58`. The crash address is `0x4a095ff8`.
Subtracting the offset gives the runtime load base:

```
0x4a095ff8 - 0x4b58 = 0x4a0914a0  (approximate runtime base of why2025_ota)
```

The callback pointer `s_ap_sta_joined_cb` was correctly registered to `0x4a095ff8`
by `ota_host_server_thread` at startup. It is **not** corrupted; gdb shows `?? ()` at
that address only because gdb loaded the ELF with the compile-time base, not the
runtime base.

### `0x4a095ff8` is in PSRAM

The linker-generated `memory.ld` places PSRAM at:

```
extern_ram_seg(RWX) : org = 0x48000000, len = (0x10000 << 10)
```

`0x4a095ff8` falls in this region. Both the system heap and app code are allocated from
PSRAM on this build (`CONFIG_SPIRAM=y`).

### The faulting instruction is `0x0000BAD0` (`C.FSD`)

On RISC-V with the `C` extension, `0xBAD0` (bits `[1:0] = 00`, bits `[15:13] = 101`)
encodes `C.FSD` (compressed double-precision float store), which requires the `D`
(double-precision float) extension. ESP32-P4 does not implement `D`, so executing this
instruction raises an Illegal Instruction exception. The legitimate first instruction of
`ap_sta_joined` (a standard `addi sp, sp, -N` or `sw ra, N(sp)` prologue) is
completely different; the byte pattern `D0 BA` has no plausible origin in hand-compiled
RISC-V.

### Hardware MPI accelerator and PSRAM

The sdkconfig has:

```
CONFIG_MBEDTLS_HARDWARE_ECC=n  ← disabled after Step 1 test, no effect
CONFIG_MBEDTLS_HARDWARE_MPI=y  ← still active; current primary suspect
CONFIG_SOC_AHB_GDMA_SUPPORT_PSRAM=y
CONFIG_SPIRAM_SPEED=200   # 200 MHz HEX-mode PSRAM
```

`tls_generate_selfsigned` calls `mbedtls_ecp_gen_key` on `MBEDTLS_ECP_DP_SECP256R1`.
Even with the ECC block disabled, software ECC falls back to `mbedtls_mpi_*` operations
for the underlying modular arithmetic, and the hardware MPI accelerator services those
through AHB GDMA transfers.

If the hardware MPI driver computes a DMA destination address incorrectly — even by a
small offset — it can write 32-byte (256-bit) bignum intermediate values to the wrong
PSRAM location. `0x4a095ff8` is a plausible victim: `0xBAD0` as two bytes of a 256-bit
integer is not special, but it happens to land there.

### Why heap integrity checks did not catch this

`heap_caps_check_integrity_all` verifies the allocator's chunk-size chain and
free-list/tree pointer consistency within the heap. It cannot detect arbitrary writes
to PSRAM addresses outside the heap data structures — including the code region where
the app is loaded. All heap checks performed during `tls_server_ctx_create` passed
because the heap itself was intact at that point; only the app code segment was hit.

### Register evidence

In the crash register dump, `T1` contains:

```
T1 : 0x4ff10134
--- ahb_dma_ll_tx_reset_channel at hal/esp32p4/include/hal/ahb_dma_ll.h:497
--- (inlined by) gdma_ahb_hal_reset at hal/gdma_hal_ahb_v2.c:51
```

The AHB GDMA reset routine was in-flight at crash time, consistent with DMA activity
from a hardware accelerator (MPI or ECC block).

### Timing: corruption occurs before the first TCP connection

Every observed crash fires on the **first** `IP_EVENT_AP_STAIPASSIGNED` event — the
moment the old badge finishes DHCP. Log evidence shows only the pre-accept `"Accepting"`
print, never `"Accepted"` or `"Assigned fd"`, confirming no TCP connection was ever
established before the crash. The corrupted word `0x0000bad0` is byte-for-byte identical
across multiple runs, which means the DMA write is deterministic in content but its
timing relative to DHCP varies.

This narrows the corruption window to the HTTP server thread's init sequence:
```
wifi_set_ap_sta_joined_cb → wifi_start_ap → socket → bind → listen
```
and, when TLS is enabled, the concurrent HTTPS thread:
```
tls_generate_selfsigned → tls_server_ctx_create → socket → bind → listen
```

No TLS handshake (ECDH) has ever run at crash time. The source must be in the init
phase — most likely during big-number operations inside `tls_generate_selfsigned`.

### Why hardware ECC elimination changed nothing

Disabling `CONFIG_MBEDTLS_HARDWARE_ECC=n` left the register dump byte-for-byte identical
(same MEPC, MTVAL, T1, T0, A5). The ECC accelerator is not the cause.
`CONFIG_MBEDTLS_HARDWARE_MPI=y` remains active; the MPI accelerator handles the
finite-field arithmetic underlying both RSA and ECC key generation. `tls_generate_selfsigned`
calls `mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1)`, which drives multiple MPI
hardware DMA operations even without the ECC block. The `T1` register pointing into
`gdma_ahb_hal_reset` at crash time remains consistent with an active DMA transfer.

### Secondary WDT manifestation

When the same DMA write lands on heap free-list or tree pointers rather than code,
dlmalloc detects the inconsistency on the next `free()` and calls
`USAGE_ERROR_ACTION` → `abort()`. This leaves the heap mutex permanently locked (same
mechanism as the previous crash documented in `ota-hosting-tls-crash.md`). Any
subsequent `malloc`/`free` call blocks indefinitely, the IDLE task starves, and the HP
System WDT fires.

---

## Diagnostic Steps

### Step 1 — Disable hardware ECC ✅ Eliminated

`CONFIG_MBEDTLS_HARDWARE_ECC=n` was tested. The crash register dump was byte-for-byte
identical (same MEPC, MTVAL, T1, T0, A5). Hardware ECC is not the cause.

### Step 2 — Test plain HTTP (no TLS)

On the OTA host settings screen, uncheck "Generate self-signed certificate" and start
hosting. This prevents `ota_host_tls_server_thread` from launching, so
`tls_generate_selfsigned` never runs and no MPI/ECC hardware ops occur.

- **If the crash disappears:** mbedTLS (hardware MPI or software big-number ops) is the
  cause — proceed to Step 3.
- **If the crash persists:** the corruption is unrelated to mbedTLS; the cause is in
  WiFi AP init or PSRAM setup — hardware watchpoint on `0x4a095ff8` would be the next
  step.

### Step 3 — Disable hardware MPI

If Step 2 confirms mbedTLS is the cause:

```
CONFIG_MBEDTLS_HARDWARE_MPI=n
```

Rebuild with TLS enabled. If the crash disappears, the hardware MPI DMA is confirmed.
Software big-number arithmetic is slower but correct; for a single-client OTA server it
is acceptable.

### Step 4 — Read diagnostic prints (code integrity checkpoints)

`DIAG_CB()` prints are already inserted in `ota_server.c` at four checkpoints:

| Checkpoint | Thread | Location |
|---|---|---|
| A | HTTP | before `wifi_set_ap_sta_joined_cb` |
| B | HTTP | after `wifi_start_ap` |
| C | HTTPS | before `tls_generate_selfsigned` |
| D | HTTPS | after `tls_generate_selfsigned` |
| E | HTTPS | after `tls_server_ctx_create` |

Each print logs `ap_sta_joined[0]` (first 4 bytes of the function's code). The first
run where the value changes from the expected prologue (`addi sp,sp,-N` or `sw ra,N(sp)`)
to `0x0000bad0` (or any other unexpected value) identifies which operation triggered the
DMA write.

Expected output (no corruption):
```
[OTA diag] ap_sta_joined[0]=0x1141xxxx @ ota_host_server_thread:NNN    ← A
[OTA diag] ap_sta_joined[0]=0x1141xxxx @ ota_host_server_thread:NNN    ← B
[OTA diag] ap_sta_joined[0]=0x1141xxxx @ ota_host_tls_server_thread:NNN ← C
[OTA diag] ap_sta_joined[0]=0x1141xxxx @ ota_host_tls_server_thread:NNN ← D
[OTA diag] ap_sta_joined[0]=0x1141xxxx @ ota_host_tls_server_thread:NNN ← E
```

If checkpoint D prints `0x0000bad0` (after `tls_generate_selfsigned`) but C was clean,
the MPI key-gen DMA is the culprit. If B is already corrupt, it's WiFi AP init.

### Step 5 — Heap integrity alongside code corruption (supplementary)

To determine whether heap corruption accompanies the code corruption, add temporarily
to `wifi.c` before `s_ap_sta_joined_cb` is called:

```c
// wifi.c — inside IP_EVENT_AP_STAIPASSIGNED handler, before the callback
#include "esp_heap_caps.h"
if (!heap_caps_check_integrity_all(true)) {
    ESP_LOGE(TAG, "Heap corruption detected before ap_sta_joined_cb call");
}
if (s_ap_sta_joined_cb)
    s_ap_sta_joined_cb(ev->mac, ip_str);
```

If the heap check also fails, the same DMA write hit both code and heap. If it passes,
only the code region is affected (the DMA destination happened to fall on executable
PSRAM outside the heap's bookkeeping structures).

---

## Reproducing the OTA Request Sequence from a Linux Machine

Connect your laptop to the **WHY2025-open** WiFi network the hosting badge creates.
The badge is always `192.168.4.1`. All commands below use plain HTTP (port 80); append
`-k https://192.168.4.1/...` for the HTTPS/443 path (the cert is self-signed).

### Step 0 — Sanity check

```bash
curl http://192.168.4.1/api/v3/ping
# expected: pong
```

### Step 1 — List apps the badge has installed

```bash
curl http://192.168.4.1/api/v3/project-summaries
# expected: [{"slug":"some_app","name":"Some App"}, ...]
```

### Step 2 — Firmware update flow

The old badge's OTA client uses the slug `why2025_firmware` for the firmware image.

```bash
# 2a. What revision does the server have?
curl http://192.168.4.1/api/v3/project-latest-revisions/why2025_firmware
# expected: 1

# 2b. What version string is that revision?
curl http://192.168.4.1/api/v3/projects/why2025_firmware/rev1/files/version.txt
# expected: <running firmware version string>

# 2c. Download the firmware binary (large — the full running partition)
curl -o badgevms.bin http://192.168.4.1/api/v3/projects/why2025_firmware/rev1/files/badgevms.bin
```

### Step 3 — App update flow

Replace `SLUG` with a slug from Step 1 (e.g. `some_app`).

```bash
# 3a. Latest revision number
curl http://192.168.4.1/api/v3/project-latest-revisions/SLUG
# expected: 1

# 3b. Revision metadata — returns JSON with a "files" array whose "url" fields
#     point back at this server at http://192.168.4.1/...
curl http://192.168.4.1/api/v3/projects/SLUG/rev1
# expected:
# {"version":{"app_metadata":{"name":"...","application":[{"executable":"..."}]},
#             "files":[{"url":"http://192.168.4.1/api/v3/projects/SLUG/rev1/files/NAME",
#                       "full_path":"NAME"}, ...]}}

# 3c. Download each file listed in the "files" array above
curl -o NAME http://192.168.4.1/api/v3/projects/SLUG/rev1/files/NAME
```

### Simulating the full update sequence in one script

```bash
HOST=http://192.168.4.1
SLUG=some_app   # replace with a real slug from /api/v3/project-summaries

echo "=== ping ==="
curl -s $HOST/api/v3/ping

echo "=== summaries ==="
curl -s $HOST/api/v3/project-summaries | python3 -m json.tool

echo "=== firmware revision ==="
REV=$(curl -s $HOST/api/v3/project-latest-revisions/why2025_firmware)
echo "revision: $REV"

echo "=== firmware version ==="
curl -s $HOST/api/v3/projects/why2025_firmware/rev${REV}/files/version.txt

echo "=== app revision JSON ==="
curl -s $HOST/api/v3/projects/$SLUG/rev1 | python3 -m json.tool
```

This is useful both as a diagnostic (checking the server responds correctly before
attaching a real badge) and as a way to reproduce the crash without a second badge:
start the hosting app, connect the laptop to the AP, run the ping — this exercises
the WiFi init path and gets a DHCP lease, triggering `IP_EVENT_AP_STAIPASSIGNED` and
the `ap_sta_joined` callback exactly as the old badge would.

---

## Files Involved

| File | Role |
|---|---|
| `badgevms/drivers/wifi.c` | WiFi event handler; crash site is `s_ap_sta_joined_cb` at line 264 |
| `sdk_apps/why2025_ota/ota_server.c` | Registers `ap_sta_joined` callback; calls `tls_generate_selfsigned` and `tls_server_ctx_create` |
| `badgevms/tls_server.c` | `tls_generate_selfsigned` (hardware ECC key gen) and `tls_server_accept_fd` (hardware ECDH handshake) |
| `sdkconfig` | `CONFIG_MBEDTLS_HARDWARE_ECC`, `CONFIG_MBEDTLS_HARDWARE_MPI` |
| `build/esp-idf/esp_system/ld/memory.ld` | Confirms PSRAM at `0x48000000` |

---

## Status

- [x] Previous crash (dlmalloc SAFE UNLINK in `tls_server_ctx_create`) — **fixed**
- [ ] Current crash (PSRAM code corruption at `0x4a095ff8`) — **investigating**
  - [x] `CONFIG_MBEDTLS_HARDWARE_ECC=n` tested — no change, eliminated
  - [ ] Pending: plain HTTP test (Step 2) — determines whether mbedTLS is involved at all
  - [ ] Pending: `CONFIG_MBEDTLS_HARDWARE_MPI=n` test (Step 3) — if Step 2 implicates TLS
  - [ ] Pending: read `DIAG_CB()` checkpoint output (Step 4) — pinpoints exact operation
