# Badge-to-Badge OTA Hosting: Architecture

Allows a new badge (running BadgeVMS) to act as a local BadgeHub mirror.
An old badge running
[TheNewImprovedGalacticPatcher](../sd_overlay/src/thenewimprovedgalacticpatcher/)
connects to the new badge's WiFi network and pulls app updates with no internet
access and no router configuration.

---

## Implementation status

| Feature | Status |
|---|---|
| WiFi AP (`WHY2025-open`, open, 192.168.4.1) | ✅ Implemented |
| DHCP (auto-assigns IPs, DNS option = self) | ✅ Implemented (lwIP automatic) |
| DNS server — resolves `badge.why2025.org` → `192.168.4.1` | ✅ Implemented |
| HTTP server — BadgeHub API for app ELFs and version.txt | ✅ Implemented |
| New-install detection (`-1` version dummies) | ✅ Implemented |
| Main firmware binary (`badgevms.bin`) | ❌ Not yet implemented |
| ESP32-C6 network adapter firmware | ❌ Not yet implemented |
| TLS / HTTPS | ❌ Not implemented — old badge patched to use HTTP |

---

## How it works end-to-end

```
Old badge (patcher)                 New badge (OTA host)
─────────────────────               ─────────────────────────────────
Connects to WHY2025-open  ────────▶ wifi_start_ap("WHY2025-open", "")
Gets DHCP lease                       192.168.4.1, DHCP server on AP netif
  DNS server = 192.168.4.1 ◀────────  (advertised automatically via DHCP opt 6)

Queries badge.why2025.org ────────▶ ota_dns_server_thread  (UDP :53)
  ← A record: 192.168.4.1 ◀────────  answers badge.why2025.org only; NXDOMAIN else

GET /api/v3/ping          ────────▶ ota_host_server_thread (TCP :80)
GET /api/v3/project-summaries         → lists all APPLICATION_SOURCE_BADGEHUB apps
GET /api/v3/project-latest-revisions/{slug}  → always "1"
GET /api/v3/projects/{slug}/rev1      → JSON with file URLs at http://192.168.4.1/...
GET /api/v3/projects/{slug}/rev1/files/{file}
  version.txt             ◀────────  from app->version in memory
  *.elf                   ◀────────  fopen/fread from VFS via application_create_file_string()
```

The old patcher is **not modified**. DNS interception makes
`badge.why2025.org` resolve to the new badge transparently.

---

## Network topology

```
New badge (AP mode)
│
├── SSID: WHY2025-open  (open, no password)
│   AP IP: 192.168.4.1
│
├── DHCP server  (lwIP built-in)
│   Pool: 192.168.4.2 – 192.168.4.11
│   DNS option: 192.168.4.1
│
├── DNS server  (ota_dns.c, UDP :53)
│   badge.why2025.org → 192.168.4.1
│   everything else  → NXDOMAIN
│
└── HTTP server  (ota_server.c, TCP :80)
    Mimics BadgeHub API v3
```

Uses `WIFI_MODE_APSTA` so an existing STA connection is preserved. If the
new badge is not connected to any external WiFi, that is fine — hosting works
entirely in isolation.

---

## Component details

### WiFi AP — `badgevms/drivers/wifi.c`

`wifi_start_ap(ssid, password)`:
- Creates AP netif once via `esp_netif_create_default_wifi_ap()` (one-shot,
  tracked in `s_ap_netif`).
- Stops DHCP server, applies config, restarts DHCP server.
- Sets mode to `WIFI_MODE_APSTA`; `hermes_do_connect` also uses APSTA when
  `s_ap_netif != NULL` so a reconnect does not kill the AP.

`wifi_stop_ap()`:
- Stops DHCP server, sets mode back to `WIFI_MODE_STA`.

Exported to SDK apps via `badgevms/include/badgevms/wifi.h` and `symbols.yml`.
Required Kconfig: `CONFIG_LWIP_DHCPS=y`, `CONFIG_WIFI_RMT_SOFTAP_SUPPORT=y`
(both set in `sdkconfig.defaults`).

### DNS server — `sdk_apps/why2025_ota/ota_dns.c`

- Binds `SOCK_DGRAM` to `INADDR_ANY:53`.
- Parses the question section (QNAME comparison against wire-encoded
  `badge.why2025.org`), type A only.
- Builds a minimal authoritative response: 12-byte header + copied question +
  answer RR with `0xC00C` name pointer and 4-byte RDATA.
- 1-second `SO_RCVTIMEO` for `stop_requested` polling.

### HTTP server — `sdk_apps/why2025_ota/ota_server.c`

Single-threaded HTTP/1.0 on port 80. Per-connection flow:

1. `accept()` with 1-second timeout (polls `stop_requested`).
2. Read headers into 2 KiB buffer until `\r\n\r\n`.
3. `sscanf` method + path; strip query string.
4. Dispatch to route handler; increment `requests_served`.
5. Close connection.

Route handlers call `application_get()` / `application_list()` to look up
installed apps. Files are served by constructing the VFS path with
`application_create_file_string()` and streaming in 1 KiB chunks.

### Thread lifecycle — `sdk_apps/why2025_ota/window.c`

On entering `UI_STATE_HOSTING`:
1. HTTP thread launched immediately (stack: 16 KiB).
2. DNS thread launched once `host_state.running` is `true` (IP is then known).
3. Both threads share the same `ip[32]` field (`192.168.4.1` in AP mode).
4. ESC sets `stop_requested` on both and returns to settings menu.

---

## What is not a complete replica — and how to fix it

The old patcher calls three extra endpoints that the current HTTP server does
not handle:

```
GET /api/v3/project-latest-revisions/why2025_firmware
GET /api/v3/projects/why2025_firmware/rev{N}/files/version.txt
GET /api/v3/projects/why2025_firmware/rev{N}/files/badgevms.bin
```

### Main firmware binary (`badgevms.bin`)

The running firmware lives in an ESP32-P4 OTA flash partition. It is not
exposed as a VFS file by the SDK. Two approaches:

**Option A — SDK function to read the OTA partition (preferred)**

Add to `badgevms/ota.c`:
```c
size_t ota_get_firmware_size(void);
bool   ota_read_firmware(size_t offset, void *buf, size_t len);
```
backed by `esp_ota_get_running_partition()` + `esp_partition_read()`.
Export via `badgevms/include/badgevms/ota.h` and `symbols.yml`.

In `ota_server.c`, add a special-case slug `why2025_firmware`:
- `handle_latest_revision` → `"1\n"`
- `handle_revision_json` → include `badgevms.bin` as a file entry
- `handle_file("why2025_firmware", "version.txt")` → `ota_get_running_version()`
- `handle_file("why2025_firmware", "badgevms.bin")` → stream via
  `ota_read_firmware()` with a generated `Content-Length`

No changes to the old patcher needed.

**Option B — Pre-bundle `badgevms.bin` in `storage_skel/`**

Place the firmware binary at
`sdk_apps/why2025_ota/storage_skel/badgevms.bin` as part of the build.
It would be installed to `BADGEVMS/APPS/why2025_ota/badgevms.bin` on the
badge's flash storage and served by the existing `handle_file` route.

Downsides: the binary must be re-flashed every firmware change; it roughly
doubles the flash storage footprint; the version would need a separate
`version.txt` stub in `storage_skel/`. Only viable if Option A is blocked.

### ESP32-C6 network adapter firmware

The C6 firmware (`network_adapter.bin`, `partition-table.bin`,
`bootloader.bin`) is already stored on the new badge at
`BADGEVMS/APPS/why2025_firmware_ota_c6/` and is accessible via VFS.
The problem is that **the old patcher has no concept of C6 firmware** —
it only fetches `badgevms.bin`.

**Without modifying the patcher:** Not possible. There is no mechanism for
the server side to trigger a C6 flash on the client.

**Modifying only TheNewImprovedGalacticPatcher:**

Add a new step in the patcher's update flow (after the main firmware update):
1. Check `GET /api/v3/project-latest-revisions/why2025_firmware_ota_c6`.
2. Compare `version.txt` against the locally installed C6 firmware version.
3. If outdated, download `network_adapter.bin`, `partition-table.bin`, and
   `bootloader.bin` into `BADGEVMS/APPS/why2025_firmware_ota_c6/`.
4. On next reboot, `flash_slave_c6_if_needed()` in the firmware automatically
   detects the new binaries and reflashes the C6.

The HTTP server already handles arbitrary slugs, so `why2025_firmware_ota_c6`
would be served via the existing `handle_summaries` / `handle_file` routes
without any server-side changes — as long as that app is installed on the
new badge (it is, as a PREINSTALL app).

The only change required is in `sd_overlay/src/thenewimprovedgalacticpatcher/`.

---

## Full replication checklist

| Step | Without patcher change | With patcher change |
|---|---|---|
| All apps (ELF + version.txt) | ✅ Works today | ✅ Works today |
| Main firmware (`badgevms.bin`) | ✅ Possible (add SDK OTA read) | ✅ Possible |
| C6 firmware | ❌ Not possible | ✅ Possible (add C6 step) |

---

## File layout reference

```
sdk_apps/why2025_ota/
├── main.c               Update-check logic, background mode entry point
├── window.c             SDL3 UI, state machine, thread lifecycle
├── ota_update.c/h       curl-based BadgeHub client (update mode)
├── ota_server.c/h       HTTP server thread, BadgeHub API emulation
├── ota_dns.c/h          UDP DNS server thread
└── storage_skel/        Pre-installed files (currently empty)
                         → would contain badgevms.bin if Option B is chosen

badgevms/
├── drivers/wifi.c                wifi_start_ap / wifi_stop_ap implementation
└── include/badgevms/wifi.h       SDK-exported declarations
```
