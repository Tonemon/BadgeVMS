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

**Current hypothesis:** A bug in `why_sbrk`'s negative-increment path causes dlmalloc's
heap-trim operation (`sbrk(-N)`) to free nearly all of the app's PSRAM pages instead
of just the trailing N bytes. The freed physical pages — which still contain live
app code and heap data — are returned to the buddy page allocator and can be
reallocated and overwritten. When the physical page that backs `ap_sta_joined`
(`0x4a095ff8`) is re-mapped to a new physical page containing different content,
the function's first instruction becomes `0x0000BAD0`.

**Eliminated:** hardware ECC (no change), hardware MPI (plain-HTTP test with no
mbedTLS produced identical crash), mbedTLS entirely (TLS off = crash still occurs),
WiFi AMPDU RX (disabled — crash still occurs with no client connecting).

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

### `SPIRAM_USE_MEMMAP` and WiFi buffer placement

`CONFIG_SPIRAM_USE_MEMMAP=y` means standard `malloc()` (and therefore WiFi/lwIP dynamic
RX buffers) uses internal SRAM, not PSRAM. PSRAM is managed exclusively by BadgeVMS's
buddy page allocator. WiFi DMA buffers are therefore in SRAM; they cannot overlap with
the PSRAM region where app code lives. This rules out WiFi DMA as the direct writer to
`0x4a095ff8`.

### `why_sbrk` negative-increment bug

`badgevms/memory.c:why_sbrk(increment)` with a negative `increment` (heap trim) had:

```c
// BUG — decrement_amount = new remaining size, not the amount to free:
int32_t decrement_amount = task_info->thread->size + increment;
```

For `sbrk(-64KB)` on a large heap this freed N-1 pages instead of 1, returning
live app code and heap pages to the buddy allocator. A subsequent allocation by any
consumer (including dlmalloc's own next `sbrk(+N)`) would remap those physical pages
with different content, corrupting `ap_sta_joined`. Fixed to:

```c
int32_t decrement_amount = (int32_t)(-increment);
```

Whether dlmalloc's 2MB trim threshold is reached during normal OTA server operation
is uncertain. If the crash persists after `MORECORE_CANNOT_TRIM=1` prevents all trim
calls, the corruption has a different source and the monitor output is needed.

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

### Timing: varies, but always before or at DHCP assignment

Three plain-HTTP (no TLS) runs recorded so far:

**Run A** — laptop connected, multiple `accept()` timeouts printed, laptop made HTTP
requests, then crash on a second `IP_EVENT_AP_STAIPASSIGNED` (laptop reconnected or
renewed lease). Callback was called successfully at least once before corruption struck.

**Run B** — crashed before the laptop connected at all. Only the WiFi AP was running,
sending beacon frames. No client ever associated. Crash fired on the first
`IP_EVENT_AP_STAIPASSIGNED` from some background association (possibly the laptop's OS
doing a passive probe).

**Run C** — crashed the moment the laptop clicked to join WHY2025-open (during the
802.11 association/DHCP exchange).

All three register dumps are byte-for-byte identical (MEPC, MTVAL, T0, T1, A5). The
corruption content is deterministic (`0x0000bad0`) but the timing is not. This is
consistent with a DMA write that triggers on any received WiFi frame — beacons, probe
responses, association frames, DHCP packets — wherever the WiFi DMA engine happens to
misplace its write on that particular run.

### Why mbedTLS elimination changed nothing

Disabling `CONFIG_MBEDTLS_HARDWARE_ECC=n` produced an identical crash. Then disabling
TLS entirely (no cert generation, no HTTPS thread, no mbedTLS calls at all) also produced
an identical crash. The register dump is byte-for-byte the same. mbedTLS is not involved
at any level.

With TLS off, no client connecting, AMPDU RX disabled, and all hardware accelerators
disabled, the crash still occurs after ~1 minute. This rules out all WiFi DMA paths as
the direct cause. The `T1 = 0x4ff10134` (`gdma_ahb_hal_reset`) in the crash dump reflects
WiFi beacon DMA that ran before the exception — it is a pre-crash register state, not
evidence that DMA caused the corruption.

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

`CONFIG_MBEDTLS_HARDWARE_ECC=n` tested. Register dump byte-for-byte identical. Not the cause.

### Step 2 — Test plain HTTP (no TLS) ✅ Crash persists (AMPDU RX was still on)

Three runs with TLS disabled. All identical register dump. mbedTLS not needed for crash. Note: AMPDU RX was still enabled in these runs.

- **Run A**: crash on second DHCP after laptop connected and served HTTP
- **Run B**: crash before laptop connected — AP beaconing alone sufficient (~23 s)
- **Run C**: crash during laptop 802.11 association

### Step 3 — DIAG_CB checkpoints ✅ A+B clean — wifi_start_ap is innocent

```
[OTA diag] ap_sta_joined[0]=0xcc221101 @ ota_host_server_thread:425   ← A
W (37147) wifi: AP started: ssid=WHY2025-open open=yes
[OTA diag] ap_sta_joined[0]=0xcc221101 @ ota_host_server_thread:428   ← B
```

`0xcc221101` = correct prologue (`c.addi sp,-16` + `c.swsp ra,N(sp)`). Corruption happens during ongoing WiFi operation (~23 s), not during init. Run ended as WDT reset (heap metadata hit this time).

### Step 4 — Disable AMPDU RX ⚠️ Crash persists with TLS on

Applied: `# CONFIG_ESP_WIFI_AMPDU_RX_ENABLED is not set`. With TLS enabled, crash still occurs on launch before any client connects. The plain-HTTP runs (Steps 2/3) all had AMPDU RX on, so we cannot confirm AMPDU was their cause.

### Step 4b — Full test matrix

| ECC | MPI | AMPDU RX | TLS | Client | Result |
|-----|-----|----------|-----|--------|--------|
| on  | on  | on  | on  | yes | crash |
| off | on  | on  | on  | yes | crash — ECC eliminated |
| off | on  | on  | off | yes | crash ~23 s (DIAG_CB A+B clean) |
| off | on  | off | on  | no  | crash on launch |
| off | off | off | off | no  | crash ~1 min — ALL hardware disabled, no client |

The last row proves the corruption source is not any hardware accelerator or WiFi AMPDU. It is internal to the BadgeVMS PSRAM management (the `why_sbrk` trim bug, or possibly an as-yet-unknown source if dlmalloc never triggers trim in this workload).

### Step 5 — Disable hardware MPI ✅ Crash persists (with TLS off)

Applied: `# CONFIG_MBEDTLS_HARDWARE_MPI is not set`. Tested with TLS disabled and no client connecting — crash still occurs after ~1 minute. mbedTLS hardware peripherals fully eliminated. The only remaining write source was the WiFi stack itself, but it was already crash-testing without any client connection.

### Step 6 — Discovered `why_sbrk` negative-increment bug (root cause candidate)

In `badgevms/memory.c`, `why_sbrk(increment)` for negative `increment` computes:

```c
// BEFORE FIX (buggy):
int32_t decrement_amount = task_info->thread->size + increment;
//  = thread->size - |increment|  ← this is the NEW remaining size, not the amount to free!
```

For example, `sbrk(-64KB)` on an 8-page (512KB) heap:
- `decrement_amount = 512KB - 64KB = 448KB` (7 pages)
- Loop frees 7 pages starting from the newest (highest vaddrs)
- `thread->size` and `thread->end` end up correctly at 64KB
- But 7 physical PSRAM pages containing live code and heap data have been returned to the page pool

The correct fix (applied in this commit):
```c
int32_t decrement_amount = (int32_t)(-increment);  // amount to free = |increment|
```

`MORECORE_CANNOT_TRIM=1` added to the dlmalloc compile flags in `badgevms/CMakeLists.txt` to prevent dlmalloc from calling `sbrk(-N)` at all, so the buggy path is unreachable until confidence in the fix is established.

**Note:** dlmalloc's default trim threshold is 2MB. Whether the OTA app heap ever grows to 2MB (triggering the bug) is uncertain and depends on TLS cert generation or other large allocations. The trim might not fire in the plain-HTTP/no-client test, suggesting an additional cause is possible.

### Step 7 — Continuous corruption monitor (applied)

Added `psram_corruption_monitor` thread to `ota_server.c`, started immediately after `wifi_start_ap`. It polls `ap_sta_joined[0]` every 50ms and prints the exact millisecond of corruption with before/after values:

```
[OTA monitor] start: ap_sta_joined=0x4a095fa0 paddr=0x02095fa0 word=0xcc221101
[OTA monitor] CORRUPTION at t=58350ms! 0xcc221101 -> 0x0000bad0
```

This tells us whether the corruption is periodic, event-triggered, or one-time.

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
  - [x] All mbedTLS hardware accelerators disabled — no change, fully eliminated
  - [x] WiFi AMPDU RX disabled — no change, eliminated
  - [x] TLS disabled, no client connecting — crash still occurs ~1 min; rules out WiFi DMA
  - [x] `why_sbrk` negative-increment bug found and fixed (`badgevms/memory.c`)
  - [x] `MORECORE_CANNOT_TRIM=1` added to prevent dlmalloc from calling `sbrk(-N)`
  - [x] Continuous corruption monitor added (`psram_corruption_monitor` thread in `ota_server.c`)
  - [ ] Pending: rebuild and flash; observe monitor output to determine corruption timing and source
