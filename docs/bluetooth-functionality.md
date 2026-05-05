# Bluetooth Functionality

The WHY2025 badge supports Bluetooth Low Energy (BLE 5.3) via the ESP32-C6 coprocessor, which already handles WiFi. The same radio chip serves both protocols; they can run simultaneously. The BLE stack (NimBLE) runs on the C6 and is bridged to the P4 compute module through the existing esp-hosted layer.

## What It Does

### Wireless Keyboards

The badge can pair with any standard BLE HID keyboard. Once paired, keypresses from the wireless keyboard behave identically to the physical badge keyboard — the same key events reach whatever app is currently in focus. Apps have no awareness of whether input arrives over wire or Bluetooth; the system handles the translation transparently.

Pairing uses just-works mode (no PIN required). Once a keyboard is paired, the badge remembers it and reconnects automatically whenever both devices are in range and Bluetooth is enabled.

### Badge-to-Badge

Badges can discover and connect to each other. Every badge continuously advertises itself by name over BLE while Bluetooth is enabled, so nearby badges appear in each other's scan results without any manual setup step.

Once two badges are connected, they can exchange short text messages through a custom GATT service. This enables contact sharing, notes, or any other short-form communication that app developers want to build on top of.

The badge remembers previously connected badges and can reconnect to them, but connecting to a new badge always starts with a scan.

## User Experience

### Settings Screen

Bluetooth has its own section in the badge settings, organised into three parts:

**Identity**  
Shows the badge's Bluetooth name (e.g. `WHY2025-A3F2`, derived from the BT MAC address by default) and an on/off toggle. The name can be changed via a text-entry dialog; it is what other badges and keyboards see when scanning.

**Keyboards**  
Lists keyboards that have been paired previously, each showing whether it is currently connected. A "Scan for keyboards" action opens a modal listing nearby BLE HID keyboards. Tapping one pairs and connects to it. The modal closes automatically once the connection is established. Keyboards can be removed from the paired list with a long-press.

**Nearby Badges**  
Lists previously connected badges with their current connection status. A "Scan for badges" action works the same way as the keyboard scan, filtered to devices advertising the badge service. Connected badges have a "Send message" option that opens a text-entry dialog.

## Persistence

The following are stored in flash (NVS) and restored at boot:

- Whether Bluetooth is enabled or disabled
- The badge's own BT display name
- The list of paired devices (address, name, and type for up to 16 devices)

The enabled flag is also reflected in the launcher config so other parts of the system can read it consistently.

## Limits

- BLE only — the C6 does not support Classic Bluetooth (A2DP, RFCOMM, etc.)
- Up to 16 paired devices stored
- Up to 32 scan results shown at a time
- Badge messages are UTF-8 text, maximum 512 bytes per message
- Simultaneous connections are limited by the NimBLE stack configuration on the C6 (typically 3–4 concurrent connections)
