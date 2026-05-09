# OTA Hosting: TLS Server Crash Investigation

## Summary

The new badge (ESP32-P4) acts as a WiFi access point and OTA update server for
old badges. The HTTPS server thread (Task 6) crashes with an "unhandled
exception" immediately after the TLS server context is initialised, before a
single client has connected.

The root cause is a dlmalloc **SAFE UNLINK** failure triggered by freeing two
buffers with a `printf` call in between. `cert_der` (311 bytes, fitting a
320-byte large chunk) is freed first, which inserts it into dlmalloc's
large-chunk tree and writes tree pointers (`fd`/`bk`/`parent`/`child[]`) into
the first bytes of the freed buffer. The `printf` between the two frees
allocates from the heap (BadgeVMS printf uses heap internally), which is
satisfied from the just-freed cert_der block — overwriting those tree pointers
with the format string. When `free(key_der)` runs, dlmalloc tries to
forward-coalesce with cert_der by calling `unlink_large_chunk`, which checks
`F->bk == cert_der && BK->fd == cert_der`. Both pointers are now garbage
(format string bytes), so the SAFE UNLINK check fails →
`USAGE_ERROR_ACTION(fm, p)` → `abort()` → "unhandled exception" in Task 6.

A secondary crash follows ~3 seconds later: when `abort()` fires inside
`USAGE_ERROR_ACTION`, dlmalloc's `PREACTION` heap mutex is left permanently
locked. The HTTP server thread's next `cJSON` call blocks waiting for the
mutex, starves the FreeRTOS IDLE task, and the HP System WDT fires.

**Fix**: `tls_server_ctx_create` now takes ownership of the cert and key
buffers and frees each one immediately after the corresponding mbedTLS parse
call (mbedTLS copies the DER data internally). No `printf` or other allocation
occurs between the two frees, so the large-chunk tree pointers are never
overwritten.

---

## What We Are Trying to Solve

BadgeVMS on the new badge must be able to:

1. Start a WiFi soft-AP (`WHY2025-open`).
2. Serve the old badge's firmware and apps over HTTP on port 80.
3. Serve the same content over HTTPS on port 443 with a self-signed certificate
   (generated at boot) so that the old badge's `esp_http_client` has a TLS
   endpoint to connect to.
4. Run a captive-DNS server on port 53 that resolves `badge.why2025.org` to
   `192.168.4.1`.

The three servers run as separate threads inside the `why2025_ota` app process.
Crash-free startup and sustained operation are required; the old badge may take
several minutes to complete a full firmware update over the local link.

---

## Architecture

```
New badge (ESP32-P4)
  ota_host_server_thread      port 80   plain HTTP  (16 KB stack)
  ota_host_tls_server_thread  port 443  HTTPS       (32 KB stack)
  ota_dns_server_thread       port 53   UDP DNS      (8 KB stack)

Old badge connects via WHY2025-open, is assigned 192.168.4.1/24 via DHCP,
resolves badge.why2025.org → 192.168.4.1, then downloads firmware over HTTP.
```

TLS uses raw mbedTLS (not `esp_tls`). A self-signed P-256 / SHA-256 certificate
is generated at startup by `tls_generate_selfsigned` and then loaded into a
`tls_server_ctx_t` by `tls_server_ctx_create`. Custom BIO callbacks (`bio_send`,
`bio_recv`) wire mbedTLS I/O to BadgeVMS fd `read`/`write` calls. The server
context is shared across all HTTPS connections; a per-connection `tls_conn_t` is
created for each accepted socket.

File descriptors in BadgeVMS are a two-level abstraction: a per-task fd (0 –
MAXFD-1) maps to a device fd (the lwIP socket fd). `setsockopt` in app code goes
through the wrapped version, which translates the task fd to the device fd before
calling the real `setsockopt`. The TLS BIO callbacks operate on the device fd
directly via `get_dev_fd()`.

All heap allocations — BadgeVMS kernel code, app code, and mbedTLS — share a
single system heap (`CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y` means mbedTLS uses
`calloc`/`free` from the same pool, not a private buffer).

---

## Observed Crashes

### Crash 1 — HP System WDT Reset (rst:0x7)

Occurred after varying durations (64 – 537 seconds) of normal operation. The
watchdog fires on the HP core with no obvious trigger in the log.

**Root cause (confirmed)**: Task 6's `abort()` (see Crash 2 below) fires inside
dlmalloc's `USAGE_ERROR_ACTION`, which is reached before the `POSTACTION` macro
releases the heap mutex. The mutex is left permanently locked. When the HTTP
server thread subsequently processes a request, `cJSON_CreateArray()` calls
`malloc()`, which blocks on the locked mutex indefinitely. This starves the
FreeRTOS IDLE task, and the HP System Watchdog fires.

A contributing factor to earlier WDT occurrences (before the send-timeout fix)
was `bio_send`'s blocking `write()` with no `SO_SNDTIMEO` on the accepted
socket, which could stall the HTTPS thread if the client's TCP receive window
filled up. This was fixed independently.

### Crash 2 — Task 6 Unhandled Exception after TLS Init

Occurs reproducibly on every run, immediately after `tls_server_ctx_create`
returns.

```
[OTA host] Certificate generated (311 bytes)
[TLS ctx] seeding RNG
[TLS ctx] parsing cert (311 bytes)
[TLS ctx] parsing key (121 bytes)
[TLS ctx] ssl config defaults
[TLS ctx] binding cert to config
[TLS ctx] context ready
Task 6 caused an unhandled exception, Cerberos will deal with it
W (43417) HADES: Stripping PID 6 of its worldy possessions
```

Three seconds later, the cascade:

```
Crashing in ESP-IDF task
abort() was called at PC 0x40024227 on core 1
--- 0x40024227: dlfree at /home/tonyy/Git/BadgeVMS/badgevms/thirdparty/dlmalloc.c:4004
#1  event_handler at /home/tonyy/Git/BadgeVMS/badgevms/drivers/wifi.c:264
```

`wifi.c:264` is the `IP_EVENT_AP_STAIPASSIGNED` handler — triggered when the old
badge obtains a DHCP lease. The `ESP_LOGI` inside that handler calls `free()` on
a log buffer. Because the heap mutex is already permanently locked at this point
(from Task 6's abort; see Crash 1), this `free()` deadlocks, starving the IDLE
task and triggering another WDT — which is reported as "Crashing in ESP-IDF
task" by the ESP panic handler. The `dlfree` address is a red herring: the panic
occurred in a different task, not necessarily at the same corruption site.

---

## Root Cause Analysis

### Why "unhandled exception" is `dlfree` calling `abort()`

In BadgeVMS, `dlmalloc.h` maps `dlfree → free`. When dlmalloc detects an
inconsistency during a free or coalescing operation it calls
`USAGE_ERROR_ACTION(fm, p)`, which expands to `abort()`. For a BadgeVMS task,
`abort()` is seen by the exception dispatcher as an "unhandled exception". Both
crashes bottom out in the same mutex-locked heap state.

### Precise crash location

Diagnostic `printf` calls were added around the free sequence in
`ota_host_tls_server_thread` to narrow down the crash:

```c
tls_server_ctx_t tls_ctx = tls_server_ctx_create(cert_der, cert_len, key_der, key_len);
printf("[OTA host] freeing cert_der=%p key_der=%p\n", cert_der, key_der);
free(cert_der);
printf("[OTA host] cert_der freed\n");   // <-- this printed
free(key_der);                            // <-- crash here
printf("[OTA host] key_der freed\n");    // <-- never reached
```

The log confirmed `cert_der freed` printed but `key_der freed` did not, placing
the crash precisely in `free(key_der)`.

### dlmalloc large-chunk tree corruption

`cert_der` held a 311-byte buffer, which dlmalloc rounds up to a 320-byte chunk.
Chunks ≥ 256 bytes (`MIN_LARGE_SIZE`) are managed in a red-black tree. When a
large chunk is freed, dlmalloc writes five tree-management pointers
(`fd`, `bk`, `parent`, two `child[]`) into the first bytes of the freed data
area — the same memory that `cert_der` pointed to.

Immediately after `free(cert_der)`, the `printf("[OTA host] cert_der freed\n")`
call internally allocates a temporary buffer from the heap. Because the 320-byte
cert_der block is the most recently freed chunk of the right size, dlmalloc
satisfies this allocation from it — overwriting those tree pointers with the
format string content.

When `free(key_der)` runs, dlmalloc forward-coalesces with cert_der (the
adjacent chunk) and calls `unlink_large_chunk`. The SAFE UNLINK check verifies:

```
F->bk == cert_der   (F = cert_der->fd, which now points into the format string)
BK->fd == cert_der  (BK = cert_der->bk, same)
```

Both pointers are garbage (format-string bytes, not valid heap addresses), so
the check fails → `USAGE_ERROR_ACTION` → `abort()`.

### Why heap integrity checks showed no corruption

`heap_caps_check_integrity_all` was added between every mbedTLS step inside
`tls_server_ctx_create` as a diagnostic. All checks passed. This correctly ruled
out the hardware ECC DMA accelerator (`CONFIG_MBEDTLS_HARDWARE_ECC=y`) as the
cause — the heap was valid throughout `tls_server_ctx_create`. The corruption
only appeared after `free(cert_der)` returned, when the tree pointers were
written, and was immediately destroyed by the subsequent `printf`.
`heap_caps_check_integrity_all` verifies chunk size chains but does not verify
free-list or tree pointer consistency, so it cannot detect this class of
corruption.

### Why the WDT follows 3 seconds later

The `abort()` in Task 6 fires inside `USAGE_ERROR_ACTION`, which is reached
before dlmalloc's `POSTACTION` macro releases the heap mutex. The mutex remains
locked. The HTTP server thread (and any other thread calling `malloc`/`free`)
blocks indefinitely the next time it touches the heap. The FreeRTOS IDLE task is
starved, and the HP System Watchdog fires. The 3-second gap corresponds to the
time between Task 6's death and the old badge completing its DHCP exchange, at
which point the WiFi event handler calls `ESP_LOGI` → `free()` → mutex deadlock.

---

## Fixes Applied

### 1. Ownership-transfer in `tls_server_ctx_create` (Crash 2 + Crash 1 root fix)

`tls_server_ctx_create` now takes ownership of `cert_der` and `key_der` and
frees each buffer immediately after the corresponding parse call. mbedTLS copies
the DER data internally during `mbedtls_x509_crt_parse_der` and
`mbedtls_pk_parse_key`, so the caller's buffers are no longer needed after those
calls return.

```c
// badgevms/tls_server.c — AFTER fix
int cert_rc = mbedtls_x509_crt_parse_der(&s->cert, cert_der, cert_len);
free(cert_der);  /* mbedTLS copies the DER; release before key parsing */
if (cert_rc != 0) goto fail_key;

int key_rc = mbedtls_pk_parse_key(&s->key, key_der, key_len, NULL, 0,
                                   mbedtls_ctr_drbg_random, &s->ctr_drbg);
free(key_der);   /* mbedTLS copies the DER; release before any further allocs */
if (key_rc != 0) goto fail_none;
```

No `printf` or other heap allocation occurs between the two frees. The cert_der
large-chunk tree pointers are written by `free(cert_der)` and remain intact when
`free(key_der)` runs forward-coalescing, so `unlink_large_chunk`'s SAFE UNLINK
check passes.

The caller (`ota_host_tls_server_thread`) no longer frees the buffers. The
function signature changed from `const uint8_t *` to `uint8_t *` with a doc
comment indicating ownership transfer.

### 2. `SO_SNDTIMEO` on accepted connections

Both the HTTP and HTTPS accept loops now set a 10-second send timeout alongside
the existing 10-second receive timeout:

```c
// sdk_apps/why2025_ota/ota_server.c
struct timeval rtv = {.tv_sec = 10, .tv_usec = 0};
setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &rtv, sizeof(rtv));
```

Without `SO_SNDTIMEO`, a blocked `write()` in `bio_send` can stall the HTTPS
thread indefinitely if the old badge's TCP receive window fills up or the
connection goes half-open.

### 3. `tls_conn_write` WANT_WRITE yield

The tight spin in the TLS write loop now yields 1 ms before retrying:

```c
// badgevms/tls_server.c
if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) { usleep(1000); continue; }
```

With a blocking BIO, `WANT_WRITE` is rare, but the spin previously prevented
FreeRTOS from scheduling other tasks when it did occur.

---

## Diagnostic Instrumentation (Removed)

During investigation, `heap_caps_check_integrity_all` was called between each
mbedTLS step inside `tls_server_ctx_create`, and `printf` calls were added
around the `free(cert_der)` / `free(key_der)` sequence in the caller. Key
findings:

- All heap checks inside `tls_server_ctx_create` passed — ruling out hardware
  ECC DMA writes as the source of corruption.
- The diagnostic printf between the two frees confirmed `cert_der freed` printed
  but `key_der freed` did not, placing the crash in `free(key_der)`.

Both the heap checks and the diagnostic prints have been removed from the final
code.

---

## Files Involved

| File | Role |
|---|---|
| `sdk_apps/why2025_ota/ota_server.c` | HTTP + HTTPS server threads, DNS callback |
| `sdk_apps/why2025_ota/window.c` | Thread launch sequencing |
| `badgevms/tls_server.c` | mbedTLS wrapper (generate, ctx create/free, accept, read/write) |
| `badgevms/include/badgevms/tls_server.h` | TLS server public API |
| `badgevms/drivers/wifi.c` | WiFi event handler (secondary crash site) |
| `badgevms/thirdparty/dlmalloc.c` | Heap allocator (`dlfree` / SAFE UNLINK at `dlmalloc.c:4004`) |
