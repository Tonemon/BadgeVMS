# Disabling SSL Verification on Old Badge Firmware

This guide explains how to patch, rebuild, and reflash an old WHY2025 badge so it accepts the self-signed TLS certificate served by a host badge running "Host OTA update." This is only needed when the host badge is using HTTPS with a self-signed certificate (`ota_host_self_signed_cert = true`). If the host badge is using plain HTTP (`ota_host_self_signed_cert = false`), no patch is required, but the old badge's OTA client hardcodes `https://`, so it will still fail to connect over HTTP. The SSL-disable patch is required in both cases until the old badge is updated.

## Prerequisites

- ESP-IDF installed and on your PATH (the old firmware was built with the IDF version present in the repo, check `old_firmware/components/` to identify it)
- A USB connection to the badge (the USB-C port used for flashing/monitoring)
- The badge powered on and in normal operation (not in bootloader mode yet)

## Step 1. Clone the repository

```bash
git clone https://gitlab.com/why2025/team-badge/firmware BadgeVMS
cd BadgeVMS
```

## Step 2. Patch SSL verification off

Open `old_firmware/badgevms/curl.c` and find the `curl_easy_init` function (around line 571). Change the two lines that enable SSL verification:

**Before:**
```c
curl->ssl_verify_peer          = true;
curl->config.crt_bundle_attach = esp_crt_bundle_attach;
```

**After:**
```c
curl->ssl_verify_peer                    = false;
curl->config.crt_bundle_attach           = NULL;
curl->config.skip_cert_common_name_check = true;
```

This disables both certificate chain verification and hostname verification for all HTTP connections made by BadgeVMS apps, including the OTA updater.

## Step 3. Configure ESP-IDF, Build and Flash

These steps are mentioned in the `README.md` as well. In short:

To build BadgeVMS you need to have esp-idf 5.5 installed. For installation instructions see here: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html.


Then build and run on the badge with:
```bash
idf.py build flash monitor
```

Note: When you do a `git pull` please run an `idf.py fullclean` before rebuilding so changes to sdkconfig.defaults are picked up



## Step 4. Verify

After flashing, open the serial monitor:

```bash
idf.py -p /dev/ttyUSB0 monitor
```

Connect the patched badge to the `WHY2025-open` network hosted by the host badge and launch the OTA updater app. You should see HTTP requests reaching the host badge's server without TLS errors.

## Security note

Disabling SSL verification makes the badge accept any certificate from any server on any network. This patch is only appropriate for the specific purpose of a local badge-to-badge update session.
