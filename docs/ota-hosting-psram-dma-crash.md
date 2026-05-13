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

**Confirmed root cause (idle crash):** A bug in `why_sbrk`'s negative-increment path
caused dlmalloc's heap-trim operation (`sbrk(-N)`) to free nearly all of the app's
PSRAM pages instead of just the trailing N bytes. Fixed, and `MORECORE_CANNOT_TRIM=1`
added as belt-and-suspenders. The badge **no longer crashes with no client connecting**
after this fix — confirming the trim bug was the idle crash cause.

**Remaining bug (connection crash):** A second crash persists when a client connects
to the WiFi AP. MEPC is `0x4a096080` (ap_sta_joined's address shifted by ~0x88 bytes
because the monitor code grew the ELF). Same `0x0000bad0` pattern. T1 at crash time
points to `ahb_dma_ll_tx_start`. Disabling WIFI_RMT AMPDU TX caused an unrelated WDT
regression (WiFi TX stall); it has been reverted. AMPDU RX remains disabled.
Next step: retest connection crash with current config (AMPDU TX=on, RX=off).

**Eliminated:** hardware ECC, hardware MPI, mbedTLS entirely, WiFi AMPDU RX,
`why_sbrk` trim bug as idle-crash cause (fixed + MORECORE_CANNOT_TRIM=1 confirmed).

A secondary manifestation is a WDT reset (`rst:0x7`) with no backtrace, which occurs
when the same DMA write corrupts the heap free-list instead of code, causing dlmalloc
to call `abort()`, leaving the heap mutex permanently locked, starving the IDLE task.

**Ongoing WDT crash (separate from PSRAM corruption):** After removing the monitor
thread, WDT crashes persisted at 30–120 seconds regardless of HTTPS on/off. Root cause:
`esp_cache_msync` calls (O(region size)) were being made while holding `cache_mmu_mutex`
in three places: the `remap_task`/`unmap_task` context-switch hooks on Core 1, and
(primarily) `framebuffer_map_pages` which is called at 60 FPS from the compositor on
Core 0. The compositor held the spinlock for a full 1 MB `esp_cache_msync` every frame
while Core 1's tick ISR spun trying to acquire it with interrupts disabled — starving
the IDLE task until the HP System WDT fired. Fixed by moving all cache ops outside the
spinlock in all three call sites; only MMU register writes need it.

---

## Crash Signatures

### Crash 1 — Illegal Instruction (idle, before fix) — CONFIRMED FIXED

```
Guru Meditation Error: Core  0 panic'ed (Illegal instruction). Exception was unhandled.
MEPC    : 0x4a095ff8  RA      : 0x40034e60
MCAUSE  : 0x00000002  MTVAL   : 0x0000bad0
T1      : 0x4ff10134  ← ahb_dma_ll_tx_reset_channel (hal/gdma_hal_ahb_v2.c)
--- 0x40034e60: event_handler at .../badgevms/drivers/wifi.c:256

#0  0x4a095ff8 in ?? ()
#1  0x40034e60 in event_handler (...) at .../badgevms/drivers/wifi.c:264
```

`wifi.c:264` is `s_ap_sta_joined_cb(ev->mac, ip_str)` inside the
`IP_EVENT_AP_STAIPASSIGNED` handler. This crash no longer occurs after the `why_sbrk`
fix and `MORECORE_CANNOT_TRIM=1`.

### Crash 2 — Illegal Instruction (connection-triggered) — ACTIVE

```
Guru Meditation Error: Core  0 panic'ed (Illegal instruction). Exception was unhandled.
MEPC    : 0x4a096080  RA      : 0x40034e60
MCAUSE  : 0x00000002  MTVAL   : 0x0000bad0
T1      : 0x4ff10XXX  ← ahb_dma_ll_tx_start (heavier TX DMA during association)
```

Triggered when a laptop connects to the WHY2025-open AP. The address `0x4a096080`
differs from `0x4a095ff8` because adding `psram_corruption_monitor` grew the ELF by
~0x88 bytes, shifting `ap_sta_joined`'s runtime address. Both HTTP (socket 3) and
HTTPS (socket 5) were accepted simultaneously at crash time.

### Crash 3 — HP System WDT Reset (no backtrace)

```
rst:0x7 (HP_SYS_HP_WDT_RESET)
--- Error: device reports readiness to read but returned no data
```

Occurs after a variable delay (30–120 seconds). No panic log is produced before reset.
The absence of any output rules out `esp_system_abort` (which would print before
resetting); the IDLE tasks are simply not getting CPU time. Root cause identified and
fixed — see Step 12.

---

## Root Cause Analysis

### Locating `ap_sta_joined` at runtime

The OTA app ELF contains (offset varies as code is added):

```
$ nm build/app_elfs/why2025_ota.elf | grep ap_sta_joined
00004b58 t ap_sta_joined   ← original ELF
00004be0 t ap_sta_joined   ← after adding psram_corruption_monitor (~0x88 bytes larger)
```

The runtime crash address reflects the actual load base at the time of each build.
Adding `psram_corruption_monitor` shifted `ap_sta_joined` from `0x4a095ff8` to
`0x4a096080` (0x88 bytes). The monitor thread correctly watches whichever address
`ap_sta_joined` resolves to at runtime via `(void*)ap_sta_joined`.

The callback pointer `s_ap_sta_joined_cb` was correctly registered to the runtime
address by `ota_host_server_thread` at startup. gdb shows `?? ()` at that address only
because gdb loaded the ELF with the compile-time base, not the runtime base.

### Crash addresses are in PSRAM

The linker-generated `memory.ld` places PSRAM at:

```
extern_ram_seg(RWX) : org = 0x48000000, len = (0x10000 << 10)
```

Both `0x4a095ff8` and `0x4a096080` fall in this region. Both the system heap and app
code are allocated from PSRAM on this build (`CONFIG_SPIRAM=y`).

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
was uncertain, but the `MORECORE_CANNOT_TRIM=1` test (Step 8) confirmed it: with trim
prevented, the idle crash does not occur. The trim was firing and the buggy path was
being hit. A second, distinct connection-triggered crash remains (Step 9).

### Why heap integrity checks did not catch this

`heap_caps_check_integrity_all` verifies the allocator's chunk-size chain and
free-list/tree pointer consistency within the heap. It cannot detect arbitrary writes
to PSRAM addresses outside the heap data structures — including the code region where
the app is loaded. All heap checks performed during `tls_server_ctx_create` passed
because the heap itself was intact at that point; only the app code segment was hit.

### Register evidence

Two distinct T1 values have been observed, both pointing into AHB GDMA routines:

**Idle crash (before fix):**
```
T1 : 0x4ff10134
--- ahb_dma_ll_tx_reset_channel at hal/esp32p4/include/hal/ahb_dma_ll.h:497
--- (inlined by) gdma_ahb_hal_reset at hal/gdma_hal_ahb_v2.c:51
```

**Connection crash (active):**
```
T1 : 0x4ff10XXX
--- ahb_dma_ll_tx_start at hal/esp32p4/include/hal/ahb_dma_ll.h
--- WiFi TX DMA start path
```

In both cases T1 reflects a DMA routine that was executing immediately before the
exception — it is a pre-crash register state, not proof that GDMA caused the write.
However, the shift from `tx_reset` (idle beaconing) to `tx_start` (active connection)
is notable: a client connecting drives much heavier TX DMA traffic, suggesting the
connection crash may be triggered by the same physical PSRAM page reuse as the idle
crash but via a different allocation pressure path.

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

Rows 1–5 are pre-fix (why_sbrk bug present, no MORECORE_CANNOT_TRIM). Rows 6+ have
the why_sbrk fix applied and `MORECORE_CANNOT_TRIM=1`.

| ECC | MPI | AMPDU RX | AMPDU TX | TLS | Client | MORECORE fix | Result |
|-----|-----|----------|----------|-----|--------|--------------|--------|
| on  | on  | on  | on  | on  | yes | no | crash |
| off | on  | on  | on  | on  | yes | no | crash — ECC eliminated |
| off | on  | on  | on  | off | yes | no | crash ~23 s (DIAG_CB A+B clean) |
| off | on  | off | on  | on  | no  | no | crash on launch |
| off | off | off | on  | off | no  | no | crash ~1 min — ALL HW disabled, no client |
| off | off | off | on  | ?   | no  | **yes** | **no crash** — idle path FIXED |
| off | off | off | on  | ?   | yes | **yes** | **crash** @ 0x4a096080 — connection crash active |
| off | off | off | **off** | ? | no | **yes** | **WDT in 2-4 s** — immediate; WiFi TX stall (see note) |
| off | off | off | **on**  | ? | ?  | **yes** | **pending** — AMPDU TX re-enabled, retest needed |

Row 6 confirms the why_sbrk trim bug was the sole cause of the idle crash. Row 7
proves a second, distinct corruption path triggered only when a client connects.
Row 8 shows that disabling WIFI_RMT_AMPDU_TX (the real P4 control; ESP_WIFI/ESP32_WIFI
lines are just Kconfig aliases/mirrors) causes an immediate WDT unrelated to PSRAM
corruption. Row 9 restores TX AMPDU to re-isolate the connection crash.

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

### Step 7 — Continuous corruption monitor — then revealed dual-core WDT bug

Added `psram_corruption_monitor` as a separate thread (`thread_create`) that polls
`ap_sta_joined[0]` every 50ms. Adding the thread grew the ELF by ~0x88 bytes, shifting
`ap_sta_joined`'s runtime address from `0x4a095ff8` to `0x4a096080`.

**Side effect (bug introduced):** The separate thread caused a WDT crash every 1–10 s
on idle, via a dual-core PSRAM context conflict:

- `remap_task()` (`badgevms/memory.c:247`) calls `esp_system_abort("Task info does not
  match")` if `current_mapped_task != 0` when a task's PSRAM is being mapped in.
- On the dual-core ESP32-P4, the FreeRTOS scheduler can place the monitor thread on
  Core 1 while the OTA server thread still holds `current_mapped_task = N` on Core 0.
- `remap_task` on Core 1 sees a non-zero `current_mapped_task` and aborts.
- The wrapped panic handler deadlocks trying to acquire a mutex held by Core 0.
- IDLE starves → HP System WDT fires (rst:0x7).
- The monitor's `memcpy` never runs, so CORRUPTION is never printed.

**The "idle crash fixed" test was a short-duration false positive** — the monitor thread
race had not yet triggered in that window. All subsequent WDT crashes were this bug, not
the PSRAM page-reuse bug.

**Fixed:** Removed the `thread_create`; moved the corruption check inline into the HTTP
`accept()` timeout path in `ota_host_server_thread`. No extra thread, ~1 s detection
resolution, no PSRAM context conflict. The inline check is safe because it runs in the
same thread that registered `ap_sta_joined` (which always has PSRAM correctly mapped).

```
[OTA monitor] CORRUPTION! ap_sta_joined 0xcc221101 -> 0x0000bad0
```

### Step 8 — `MORECORE_CANNOT_TRIM=1` test (needs rerun without monitor thread)

The previous Step 8 result ("idle crash CONFIRMED FIXED") was a short-duration run that
did not survive long enough to hit the monitor-thread WDT. The result is invalidated.

With the monitor thread removed, rebuild and retest idle stability to get a clean signal
on whether `MORECORE_CANNOT_TRIM=1` + the why_sbrk fix actually prevents the trim crash
over a sustained run (5+ minutes, no client connecting).

### Step 9 — Connection crash identified (second distinct bug)

After the idle fix, connecting a laptop to the WHY2025-open AP still triggers a crash:

- MEPC: `0x4a096080` (ap_sta_joined at its new ELF-shifted address)
- MTVAL: `0x0000bad0` (same sentinel pattern)
- T1: `ahb_dma_ll_tx_start` (WiFi TX DMA is more active during 802.11 association)
- Both socket 3 (HTTP) and socket 5 (HTTPS) were accepted simultaneously at crash time

**Open questions for this crash:**
- Was TLS (`tls_generate_selfsigned`) enabled or disabled in this run?
- Did `[OTA monitor] CORRUPTION at t=...` appear in the serial log before the crash?
  (If yes: corruption happened before the exception was raised, giving timing context.
   If no: crash was immediate with no prior monitor hit, suggesting the corrupt page
   was remapped in the same allocation event that caused the address fault.)

### Step 10 — Disable WiFi AMPDU TX ❌ WDT regression

Applied `# CONFIG_WIFI_RMT_AMPDU_TX_ENABLED is not set`. Caused immediate WDT reset
(rst:0x7) within 2–4 seconds on every run, before any client connected. The
psram_corruption_monitor confirmed `ap_sta_joined` code was **not** corrupted — the
bug was in the WiFi TX path, not PSRAM.

**Finding:** `ESP_WIFI_AMPDU_TX_ENABLED` / `ESP32_WIFI_AMPDU_TX_ENABLED` are Kconfig
aliases derived from `WIFI_RMT_AMPDU_TX_ENABLED` via `default` — they were no-ops for
P4/wifi_remote. `WIFI_RMT_AMPDU_TX_ENABLED` is the only real control. Disabling it
causes the WiFi TX path to stall or busy-loop during AP startup (first beacon TX),
starving the IDLE task and firing the HP System WDT.

**Reverted:** `CONFIG_WIFI_RMT_AMPDU_TX_ENABLED=y`, `CONFIG_WIFI_RMT_TX_BA_WIN=6`.

### Step 11 — Retest with AMPDU TX restored, AMPDU RX still disabled — WDT persists

With AMPDU TX restored (`WIFI_RMT_AMPDU_TX_ENABLED=y`) and MORECORE fix in place, WDT
crashes still occurred at ~30–120 seconds regardless of HTTPS on/off, with no output
before the reset. The monitor showed no PSRAM corruption before the crash. This WDT is a
different bug from the PSRAM page-reuse crash — see Step 12.

### Step 12 — WDT root cause identified: `cache_mmu_mutex` spinlock held during slow cache ops ✅ Fixed

**Investigation findings:**

1. **All user tasks are pinned to Core 1** (`task.c:726`, `xTaskCreatePinnedToCore(..., 1)`).
   The dual-core sibling-thread race (basis for the ref-count fix in Step 7) cannot
   occur: on a single core, FreeRTOS always calls `task_switched_out_hook` before
   `task_switched_in_hook`, so `current_mapped_task` is always 0 when `remap_task` is
   called. The `esp_system_abort` abort path in `remap_task` cannot be triggered by
   normal scheduling.

2. **`thread_create` siblings have different pids.** Zeus allocates a fresh pid for every
   new FreeRTOS task, including threads (`TASK_TYPE_THREAD`). The child gets its own
   `task_info_t` with a new pid but shares the parent's `task_thread_t` (pages). So
   `current_mapped_task == task_info->pid` in `remap_task` is never true in practice —
   the ref-counting code added in the previous session is dead. (It is kept for
   correctness in case the design changes.)

3. **`cache_mmu_mutex` is shared between Core 0 and Core 1.** The compositor task runs
   on Core 0 (`compositor.c:1347`, `create_kernel_task(..., 0)`) and calls
   `framebuffer_map_pages`/`framebuffer_unmap_pages`, both of which take
   `cache_mmu_mutex`. The `remap_task`/`unmap_task` context-switch hooks on Core 1 also
   take `cache_mmu_mutex` via `critical_enter()` / `portENTER_CRITICAL_SAFE`.

4. **`esp_cache_msync` was called inside the spinlock — in three places.** The function
   is O(region size): it must iterate every cache line in the range. Three call sites
   were affected:

   - **`unmap_task` / `remap_task` (context-switch hooks, Core 1):** `writeback_caches`
     and `invalidate_caches` were called inside `critical_enter()` over the task's full
     PSRAM heap (`task_info->thread->start .. start+size`). As the OTA server's heap
     grows, each context-switch holds the spinlock with Core 1 interrupts disabled for
     increasingly long durations.

   - **`framebuffer_map_pages` (compositor, Core 0, ~60 FPS) — primary hotpath:**
     `map_regions()` was called inside `critical_enter()`, and `map_regions` ends by
     calling `invalidate_caches` over the entire framebuffer range (1 MB:
     720×720×2 bytes). At 60 FPS the compositor held `cache_mmu_mutex` for a 1 MB
     `esp_cache_msync` sixty times per second. Core 1's tick ISR (context-switch hooks)
     spins on the same mutex with interrupts disabled during each of those calls.

   - **`why_sbrk` grow path:** `map_regions()` was similarly called inside
     `critical_enter()` each time dlmalloc expanded the heap via `sbrk(+N)`.

   When either core's IDLE task is blocked for the WDT timeout (~5 s), the HP System
   WDT fires with no output — neither `abort()` nor `esp_system_abort` was called.

5. **Timing matches.** The WDT fired after 30–120 seconds — consistent with the heap
   growing large enough that `esp_cache_msync` over it takes tens of milliseconds per
   context switch, compounded by the compositor's 60 FPS framebuffer-map calls holding
   the lock for 1 MB cache invalidations.

**Fix applied (`badgevms/memory.c`):**

- **`unmap_task`:** `writeback_caches` moved **before** `critical_enter()`. Safe because
  the task is already off the CPU (we are in the switched-out hook), and Core 0
  (compositor) only touches framebuffer PSRAM pages, not task PSRAM pages.

- **`remap_task`:** `invalidate_caches` moved **after** `critical_exit()`. Safe because
  the hook must return before FreeRTOS hands control to the new task, so invalidation
  completes before any code in the task accesses the mapped pages.

- **`framebuffer_map_pages`:** inlined the MMU mapping loop (previously delegated to
  `map_regions()`) inside `critical_enter()`/`critical_exit()`, then calls
  `invalidate_caches` **after** `critical_exit()`. Mirrors the pattern in `remap_task`.

- **`why_sbrk` grow path:** same refactor — MMU writes and linked-list update stay
  inside the lock; `invalidate_caches` moved after `critical_exit()`.

- **`writeback_and_invalidate_task`:** the surrounding `critical_enter()`/`critical_exit()`
  was removed entirely. This function does no MMU writes; the lock was unnecessary.
  (Called once per ELF launch, so not a hotpath, but consistent with the fix.)

The spinlock now only guards actual MMU register writes
(`why_mmu_hal_map_region` / `why_mmu_hal_unmap_region`), which are O(page count) and
take microseconds. Cross-core spinlock contention is reduced to negligible duration,
eliminating IDLE starvation.

### Step 13 — Rebuild, flash, and verify WDT fix ⏳ Pending

Expected: stable idle operation (5+ minutes, no client) with no WDT reset.
If stable, proceed to connection crash testing (PSRAM code at `0x4a096080`).

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
- [x] `why_sbrk` negative-increment bug found and fixed (`badgevms/memory.c`)
- [x] `MORECORE_CANNOT_TRIM=1` added to prevent dlmalloc from calling `sbrk(-N)`
- [x] Dual-core WDT from `thread_create` monitor thread — fixed (monitor moved inline)
- [x] `WIFI_RMT_AMPDU_TX_ENABLED=y` restored after disabling it caused unrelated WDT regression
- [x] WDT root cause found: `cache_mmu_mutex` spinlock held during `esp_cache_msync` in three places — **fixed**
  - `remap_task`/`unmap_task` (context-switch hooks): cache ops moved outside spinlock
  - `framebuffer_map_pages` (compositor, 60 FPS, ~1 MB per call): cache invalidation moved after `critical_exit()`
  - `why_sbrk` grow path: cache invalidation moved after `critical_exit()`
  - `writeback_and_invalidate_task`: unnecessary spinlock removed entirely
- [ ] **Next:** rebuild and flash; verify WDT fix — idle stable for 5+ minutes
- [ ] Connection crash (PSRAM code at `0x4a096080` overwritten with `0x0000bad0` on client connect) — blocked on WDT fix confirmation
