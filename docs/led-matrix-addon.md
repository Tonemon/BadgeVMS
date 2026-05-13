# LED Matrix Addon — PCA9698 Driver and sdk_apps/led_matrix_v1

## Overview

This documents the work to support a 12×20 LED matrix addon for the WHY2025 badge. The addon uses an NXP PCA9698 40-bit I2C I/O expander to drive the matrix. A kernel driver (`badgevms/drivers/pca9698.c`) registers the device as `LEDMATRIX0`, and an SDK app (`sdk_apps/led_matrix_v1`) provides a full-screen UI with static text, scrolling text, and a hardware diagnostic mode.

---

## Hardware Description

**Addon PCB markings:**
> I²C Expander NXP PCA9698DGG (40-bit Fm+ GPIO)  
> Logic supply: +3.3V (I²C pull-ups)  
> LED/Driver supply: +5V (rows src 10 mA, cols sink 25 mA)  
> I²C Default address: 0x40 (AD0–2 = 0)

### I²C Address

The "0x40" on the PCB is the **8-bit write address** (7-bit address shifted left by one, R/W bit = 0). In 7-bit notation — which is what ESP-IDF and the BadgeVMS I²C bus use — the address is **0x20** with all address pins (A0/A1/A2) tied to ground.

### Matrix Layout

- **12 rows** driven by PB0–PB11 (anode side, active HIGH via 1kΩ R-nets)
- **20 columns** driven by PA0–PA19 (cathode side, active LOW)
- LED is ON when its row anode is HIGH **and** its column cathode is LOW simultaneously

### Drive Sequence (Multiplexed)

The matrix is multiplexed — only one row is active at a time:

1. Assert one PB row HIGH (activate its anode)
2. Write a 20-bit column mask where LOW = LED ON
3. Repeat for all 12 rows at ≥1 kHz total

A static write cannot illuminate the full matrix. The firmware must cycle continuously through all rows.

---

## Badge–Addon Wiring

| Badge pin | Addon pin | Notes |
|---|---|---|
| 3.3V | 3V | Logic supply for PCA9698, I²C pull-ups |
| 5V | 5V | LED driver supply |
| GND | GND | |
| SDA | SDA | I²C data |
| SCL | SCL | I²C clock |
| CRESET | NC | **Not connected** — see Known Issues |
| STROBE | NC | |
| INT1 | NC | |
| INT2 | NC | |
| INT_KEY | NC | |
| .TC | NC | |
| D0 | IO7 | Badge GPIO → PA7 (column 7 cathode) |
| D1/DBL | IO5 | Badge GPIO → PA5 (column 5 cathode) |
| D2 | IO3 | Badge GPIO → PA3 (column 3 cathode) |
| D3 | IO8 | Badge GPIO → PA8 (column 8 cathode) |
| D4 | IO10 | Badge GPIO → PA10 (column 10 cathode) |

D0–D4 connect to PCA9698 column cathode pins. These are regular I/O pins and do not affect I²C communication or addressing.

---

## PCA9698 Bank Mapping

The PCA9698 has five 8-bit I/O banks (IO0–IO4). Based on the PCB pin labelling (PA = columns, PB = rows) and confirmed by the D0–D4 badge connections landing on column pins:

| Bank | Register | Pins | Function | Active logic |
|---|---|---|---|---|
| IO0 | OP0 (0x08) | PA0–PA7 | Columns 0–7 | LOW = LED ON |
| IO1 | OP1 (0x09) | PA8–PA15 | Columns 8–15 | LOW = LED ON |
| IO2 | OP2 (0x0A) | PA16–PA19 (bits 0–3) | Columns 16–19, bits 4–7 unused | LOW = LED ON |
| IO3 | OP3 (0x0B) | PB0–PB7 | Rows 0–7 | HIGH = active |
| IO4 | OP4 (0x0C) | PB8–PB11 (bits 0–3) | Rows 8–11, bits 4–7 unused | HIGH = active |

All five IOC (direction) registers are set to 0x00 (all outputs) at init.  
PA banks initialise to 0xFF (all column cathodes HIGH = LEDs off).  
PB banks initialise to 0x00 (all row anodes LOW = no rows active).

Auto-increment write (command byte OR 0x80) is used to write all five OP registers in one I²C transaction.

---

## Software Implementation

### Kernel Driver — `badgevms/drivers/pca9698.c`

- Registered in `badgevms/CMakeLists.txt` as a build source
- Included and instantiated in `badgevms/why2025_firmware.c`:
  ```c
  device_register("LEDMATRIX0", pca9698_create(0x20));
  ```
- `pca9698_create(addr)` fetches `I2CBUS0`, creates an I²C device at 400 kHz, configures all banks as outputs, and returns the device handle
- `_write(dev, 0, banks, 5)` writes all five OP registers in one auto-increment I²C transaction
- `device_type_t` extended with `DEVICE_TYPE_LED_MATRIX` in `badgevms/include/badgevms/device.h`

**I²C speed constraint:** The PCA9698DGG supports Fm+ (1 MHz) but the `espressif__i2c_bus` managed component hard-caps `i2c_bus_device_create` at 400 kHz (`I2C_BUS_CHECK(clk_speed <= 400000, ...)`). Any value above 400 kHz causes device creation to fail and return NULL, which propagates to `device_register` receiving NULL and silently not registering the device. The driver therefore uses 400 kHz, giving a full-matrix scan rate of approximately 600 Hz (12 rows × ~137 µs/row).

### SDK App — `sdk_apps/led_matrix_v1/main.c`

Uses `SDL_MAIN_USE_CALLBACKS=1`. Three modes:

| Mode | Key | Description |
|---|---|---|
| Static | default | Show up to 4 characters (4 × 5 px = 20 columns). LEFT/RIGHT navigate. |
| Scroll | M | Text scrolls left continuously at 1 pixel per 50 ms. |
| Diag | D | I²C bus scan + 36 hardware test patterns. |

**Framebuffer and multiplexing thread:**

A global `uint8_t g_framebuf[12][20]` holds the current LED state (1 = ON). A background SDL thread (`multiplex_thread_fn`) continuously cycles all 12 rows, builds the five bank bytes for each row via `build_banks_for_row`, and calls `_write`. The SDL main thread updates the framebuffer under `g_fb_mutex`; the thread reads it under the same mutex.

`build_banks_for_row` for a given row `r` and column data:
```c
// Default: all column cathodes HIGH (off), all row anodes LOW (off)
banks[0] = banks[1] = banks[2] = 0xFF;
banks[3] = banks[4] = 0x00;

// Set active columns (active LOW = clear bit)
for c in 0..7:   if col[c]    banks[0] &= ~(1 << c)
for c in 0..7:   if col[c+8]  banks[1] &= ~(1 << c)
for c in 0..3:   if col[c+16] banks[2] &= ~(1 << c)

// Set active row (active HIGH)
if r < 8:  banks[3] = 1 << r
else:      banks[4] = 1 << (r - 8)
```

**Diagnostic mode** has 36 patterns: All ON, All OFF, each of the 20 columns individually, each of the 12 rows individually, and two checkerboards. The I²C bus scan is paused via `g_multiplex_paused` while scanning to avoid bus contention between the multiplexing thread and the scan.

**Font:** 5×7 bitmap font (ASCII 0x20–0x7E). Characters are centred vertically in the 12-row matrix with 2 rows of padding at top and 3 at bottom.

**SDL rendering:** 720×720 px. Matrix preview occupies the top 548 px (32×42 px LEDs with 4 px gap = 716×548 px, centred). A 3-line status strip occupies the bottom 152 px.

---

## Known Issues

### PCA9698 not found on I²C bus

**Symptom:** I²C scan finds no device in the 0x20–0x27 range. Diag mode reports "NOT FOUND — check wiring/power". Hardware shows "NOT RESPONDING".

**Root cause:** The PCA9698 has an active-low `/RESET` pin. When this pin is at 0V the chip is frozen in reset and does not respond to I²C at any address. The pin must be pulled HIGH (3.3V) for normal operation.

The badge's `CRESET` signal is **not connected** to the addon (NC). Whether the PCA9698's `/RESET` pin is held HIGH therefore depends entirely on whether the addon PCB includes a pull-up resistor on that pin.

**This is a hardware issue. It cannot be fixed in software.**

**Diagnosis:**

With the addon powered, measure the voltage between the PCA9698's `/RESET` pin and GND with a multimeter:
- 3.3V → chip is not in reset; look for another cause
- 0V or floating near 0V → chip is in reset; pull-up is missing or insufficient

**Fix:**

Solder a **10kΩ resistor** between the PCA9698 `/RESET` pin and the 3.3V supply on the addon PCB. On the HVQFN48 package, `/RESET` is pin 25.

Once the chip responds on the I²C bus, it will appear at address 0x20 (assuming A0/A1/A2 are all grounded as the PCB markings indicate). If the scan finds it at a different address in the 0x20–0x27 range, update `pca9698_create(0x20)` in `badgevms/why2025_firmware.c` to match.

### I²C bus scan shows 3 devices regardless of addon

The badge I²C bus always finds:

| Address | Device |
|---|---|
| 0x34 | TCA8418 keyboard controller |
| 0x69 | BMI270 IMU |
| 0x76 | BME690 gas/environmental sensor |

These are badge-onboard devices unrelated to the addon. Their presence with and without the addon connected confirms that I²C wiring (SDA/SCL/power) is functional.

---

## File Index

| File | Purpose |
|---|---|
| `badgevms/include/badgevms/device.h` | `DEVICE_TYPE_LED_MATRIX` added to `device_type_t` |
| `badgevms/drivers/pca9698.h` | Driver header: `pca9698_create()` declaration |
| `badgevms/drivers/pca9698.c` | Kernel driver: init, 5-byte OP register write |
| `badgevms/CMakeLists.txt` | `drivers/pca9698.c` added to SRCS |
| `badgevms/why2025_firmware.c` | `device_register("LEDMATRIX0", pca9698_create(0x20))` |
| `sdk_apps/led_matrix_v1/main.c` | SDL3 app: framebuffer, mux thread, UI, diag |
| `sdk_apps/led_matrix_v1/manifest.json` | App manifest |
| `sdk_apps/CMakeLists.txt` | `build_app(led_matrix_v1 PREINSTALL ...)` |
