# Badge-to-Badge OTA Update: Architecture & Implementation Plan

This document covers architecture, component responsibilities, the certificate strategy, and the open challenges.

---

## Goal

Allow a single host badge to broadcast a local WiFi network over which any number of client badges can perform a full software update, receiving all apps, the main firmware, and the C6 network adapter firmware, all with no internet connection required. The result is a complete replication of the host badge's software state onto each client badge.

---

## What Gets Transferred

| Component | Source on host | How it reaches the client |
|-----------|---------------|--------------------------|
| All installed apps | `/BADGEVMS/APPS/<slug>/` | Served as BadgeHub API responses |
| `badgevms_launcher` | `/BADGEVMS/APPS/badgevms_launcher/` | Same; client OTA installs it as a regular app |
| `badgevms_settings` | `/BADGEVMS/APPS/badgevms_settings/` | Same |
| `why2025_firmware_ota_c6` | `/BADGEVMS/APPS/why2025_firmware_ota_c6/` | Served as app; contains bootloader.bin, partition-table.bin, network_adapter.bin; BadgeVMS flashes the C6 on next reboot via `flash_slave_c6_if_needed()` |
| BadgeVMS main firmware | `badgevms.bin` (located via the firmware OTA endpoint) | Served via `/api/v3/projects/why2025_firmware/rev<N>/files/badgevms.bin` |

After the client badge completes the OTA cycle and reboots, BadgeVMS automatically detects the new C6 binaries on the SD card and reflashes the ESP32-C6 over UART before starting the main OS. The client badge is then a full replica of the host.

---

## Network Topology

```
Host badge (AP mode)
│
├── SSID: "WHY2025-open" (configurable, open/no password)
│   IP: 192.168.4.1
│
├── DHCP server
│   Assigns 192.168.4.x to connecting clients
│
├── DNS server (port 53 UDP)
│   Resolves badge.why2025.org → 192.168.4.1
│   All other queries → NXDOMAIN (no internet forwarding)
│
└── HTTP(S) server (port 80 or 443)
    Simulates the BadgeHub API v3 surface used by the OTA client
```

Client badges connect to the SSID, receive a DHCP lease and the host badge's IP as their DNS server. When the OTA client resolves `badge.why2025.org`, it receives `192.168.4.1` and connects directly to the host badge's server.

---

## TLS / Certificate Strategy

The OTA client on old badges uses `esp_crt_bundle_attach` (the Mozilla CA bundle embedded in the firmware) and verifies TLS certificates by default. This creates a challenge for self-signed certificates.

### Phase 1. Self-signed certificate (proof of concept)

- Host badge generates an RSA-2048 key pair and a self-signed X.509 certificate for CN=`badge.why2025.org` using mbedTLS at Host OTA launch time.
- Certificate and key are stored entirely in heap memory; not written to SD card or flash; discarded when hosting stops.
- Client badges must be patched to disable SSL verification before connecting. See `docs/old-firmware-ssl-disable.md`.
- If `ota_host_self_signed_cert = false`, the server runs plain HTTP on port 80. Old badges still need the SSL patch in this case because their OTA client URL is hardcoded to `https://`.

### Phase 2. Real certificate (production, transparent to unmodified clients)

- We would need to obtain the TLS private key for `badge.why2025.org` from the WHY2025 badge team if that is possible (likely not). The domain uses a Let's Encrypt certificate (90-day validity) signed by ISRG Root X1, which is present in the Mozilla CA bundle on all badge firmware versions.
- We would need to embed the certificate chain and private key in the host badge firmware at build time.
- With this cert, DNS hijacking + HTTPS on the host badge is completely transparent to unmodified old badges. No client-side patching is needed.
- The certificate must be renewed before expiry and a new firmware build distributed to host badges.

---

## Component Breakdown

### 1. WiFi AP mode

The host badge switches the WiFi interface to AP (access point) mode when hosting starts. It broadcasts the configured SSID (from `ota_host_ssid` in config.json) as an open network. ESP-IDF's WiFi AP support handles DHCP automatically. The AP IP is `192.168.4.1`.

When hosting stops, the badge reconnects to its previously configured station network (or goes back to the disconnected state).

**Consideration:** The badge cannot simultaneously be a WiFi client (STA mode) and serve as an AP with full DNS hijacking, because in AP+STA mode the DNS responses for `badge.why2025.org` would affect the badge's own connections. For the hosting session, the badge should be AP-only.

### 2. DNS server

A minimal UDP DNS server listening on port 53, running in a background task. It responds to all A record queries for `badge.why2025.org` with `192.168.4.1`. All other queries receive NXDOMAIN. No actual DNS forwarding is needed, the goal is isolation, not internet access.

**Implementation:** A raw UDP socket is sufficient. The DNS query/response format is straightforward: parse the question section, construct a single A record response. No full DNS library is required.

### 3. HTTP(S) server

Runs on the main ESP32-P4 chip in a background task. Listens on port 443 (HTTPS with self-signed cert) or port 80 (plain HTTP). Serves the five BadgeHub API v3 endpoint shapes used by the OTA client:

| Endpoint | Response |
|----------|----------|
| `GET /api/v3/ping` | `200 OK`, empty body |
| `GET /api/v3/project-summaries?category=Default` | JSON array of all installed apps (slugs) |
| `GET /api/v3/project-latest-revisions/<slug>` | `"1"` (synthetic revision number) |
| `GET /api/v3/projects/<slug>/rev1` | JSON with `version.files[]` array containing `{url, full_path}` for each file |
| `GET /api/v3/projects/<slug>/rev1/files/<filename>` | Binary file stream from `/BADGEVMS/APPS/<slug>/<filename>` |

The server builds the app catalog by calling `application_list()` at start time, then serves the results statically for the duration of the hosting session.

**HTTPS path:** When `ota_host_self_signed_cert = true`, the server uses mbedTLS to wrap the socket with TLS using the in-memory cert and key generated at launch.

**Concurrency:** The server handles one client connection at a time. Badge updates are sequential by nature (one badge connects, updates, disconnects) so this is sufficient. A small connection queue handles brief overlaps.

### 4. App catalog builder

At Host OTA launch, the app catalog is built by:
1. Calling `application_list()` to enumerate all installed apps.
2. For each app, reading the files present in `/BADGEVMS/APPS/<slug>/` to build the file list for the `rev1` endpoint.
3. Constructing the `project-summaries` response to include every app as a "default" app, so client badges install everything.

The firmware endpoint is handled separately: the server reads `badgevms.bin` from the known firmware path and serves it at the firmware URL. The version is read from the running firmware version string.

### 5. In-memory certificate generation

When `ota_host_self_signed_cert = true`:
1. Generate RSA-2048 private key using mbedTLS `mbedtls_rsa_gen_key`.
2. Construct a self-signed X.509 certificate: CN=`badge.why2025.org`, validity 1 year from generation time, basic constraints (CA:false).
3. Store PEM-encoded cert and key in heap buffers.
4. Pass buffers to the TLS server context at init time.
5. Free buffers when hosting stops.

RSA-2048 key generation on ESP32-P4 takes a few seconds. The UI should show a "Generating certificate..." status during this step.

---

## Client Badge Update Flow

1. User opens WHY 2025 OTA updater on the client badge.
2. Badge connects to `WHY2025-open` (no password).
3. OTA client resolves `badge.why2025.org` via the host badge's DNS → `192.168.4.1`.
4. OTA client hits the ping endpoint, then fetches `project-summaries?category=Default`.
5. For each app in the list, the client checks the installed version against `rev1/files/version.txt`. Apps that are missing or out of date are queued.
6. Client downloads and installs all queued apps.
7. Client checks firmware version; if the host badge's firmware is newer, downloads and flashes `badgevms.bin` via the existing OTA partition mechanism.
8. Client reboots. On boot, `flash_slave_c6_if_needed()` detects the new C6 binaries in `why2025_firmware_ota_c6/` and reflashes the C6 chip over UART.
9. Client badge is now a full replica of the host badge's software state.

---

## Challenges and Considerations

### Certificate validity on unpatched clients

Old badges verify TLS by default. Until Phase 2 (real cert) is implemented, every client badge must be patched to disable SSL verification before it can connect to the host. This is a manual step per badge. See `docs/old-firmware-ssl-disable.md`.

### RSA key generation time

Generating a 2048-bit RSA key on an embedded CPU takes several seconds. During this time the badge is unresponsive. Options: show a clear "Generating certificate, please wait..." screen; or pre-generate the key lazily in the background when the user navigates to Host settings (not at launch time).

### AP-only mode means no internet for the host badge

While hosting, the host badge cannot simultaneously use WiFi as a client. If the host badge needs to update itself first (to get the latest software to serve), it must do so before starting hosting mode.

### File serving from SD card

The HTTP server reads app files from the SD card. Large files (ELF binaries, `badgevms.bin`) are streamed in chunks. If the SD card is slow, transfers will be slow. The `badgevms.bin` for the WHY2025 badge is typically several megabytes, over a local WiFi link at ~1 MB/s this is manageable but noticeable.

### Multiple simultaneous clients

The single-threaded server design means only one client badge updates at a time. For a small group of badges this is acceptable. For larger groups (20+ badges), a queuing mechanism or a multi-threaded server would be needed. This is a future improvement.

### App file layout assumptions

The server assumes that all files for an app are flat inside `/BADGEVMS/APPS/<slug>/`. If an app stores files in subdirectories, the file-listing logic must recurse. This should be verified against the actual installed app structure.

### `why2025_firmware_ota_c6` binary_path is empty

The `why2025_firmware_ota_c6` app descriptor has `"binary_path": ""`. It has no executable, it is purely a file container for the C6 binaries. The server must serve its files correctly but should not try to set or execute a binary path on the client.

### DNS server conflicts

If a client badge has a hardcoded DNS server configured (not using DHCP-assigned DNS), the host badge's DNS hijacking will not work. The OTA client relies on DHCP-assigned DNS. This is standard behavior and should not be an issue in practice.

### Certificate renewal (Phase 2)

A Let's Encrypt certificate is valid for 90 days. Host badge firmware carrying the real cert must be rebuilt and redistributed before expiry. A renewal reminder or automated check could be added to the badge build system.
