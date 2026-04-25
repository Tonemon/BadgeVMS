# SMS Emulator Core Research: TotalSMS

Research and considerations for porting a Sega Master System emulator to BadgeVMS (ESP32-P4).

## Target constraints

| Constraint | Value |
|---|---|
| SoC | ESP32-P4, RISC-V 32-bit (rv32imafc) |
| PSRAM | 32 MB |
| Audio | None — must be fully stubbed or omitted |
| Display | 720×720 RGB565, 60 Hz |
| Input | Full keyboard (TCA8418) |
| App format | Position-independent ELF shared object, newlib libc |

## Candidates evaluated

### TotalSMS (ITotalJustice/TotalSMS) — **recommended**

- **Language:** C (95.6%), MIT licence
- **Architecture:** Zero global state. Everything lives in a single `SMS_Core` struct that the host allocates. No static arrays, no BSS globals to worry about in a PIC binary.
- **RAM footprint:** ~60–80 KB total for the core state. ROM is a pointer into the host's own buffer — the core does not copy it.
- **Platform surface (minimal port required):**
  ```c
  SMS_Core sms;
  SMS_init(&sms);
  SMS_loadrom(&sms, rom_data, size, SMS_System_SMS);
  SMS_set_pixels(&sms, framebuffer->pixels, pitch, 16); // 16 = RGB565
  // SMS_set_apu_callback omitted entirely — no audio needed
  while (running) {
      handle_input(&sms);                      // set SMS_PortA/B bits
      SMS_run(&sms, SMS_CYCLES_PER_FRAME);
      window_present(window, false, NULL, 0);
  }
  ```
- **Audio:** APU callback is optional. Passing `NULL` or simply not calling `SMS_set_apu_callback` disables audio completely with no stubs required.
- **Embedded track record:**
  - MCH2022 badge (ESP32-S3, 8 MB PSRAM, 240 MHz IDF) ran TotalSMS as a badge app at 60 FPS — the closest existing precedent to BadgeVMS.
  - Also ported to GameCube, N64, NDS, PSX, and libretro.
- **Porting effort:** Estimated ~100–150 lines for `sms_badgevms.c` (platform shim). Comparable in scale to `doomgeneric_badgevms.c` (215 lines).

### smsplus-gx (libretro/smsplus-gx)

- **Language:** C (99.9%), GPL-2-or-later
- **Architecture:** Large static/global lookup tables: `bg_pattern_cache` (128 KB), `bp_lut` (256 KB), `lut` (64 KB). All must be initialised at startup.
- **RAM footprint:** ~630 KB (dominated by the two rendering LUTs — acceptable on 32 MB PSRAM, but the large static globals are awkward in a PIC binary).
- **Platform surface:** Libretro shim (~700 lines). Replace `smsplus_libretro.c` with a BadgeVMS equivalent.
- **Embedded track record:** `esp-box-emu` (ESP32-S3 + ESP-BOX board, IDF component).
- **Status:** Active libretro community; better game compatibility than TotalSMS in edge cases.

### SMS Plus original (0ldsk00l/smsplus)

- **Language:** C (~100%), GPL-2
- **Architecture:** Global state. GLFW3 shell (throwaway). Smaller LUTs than smsplus-gx.
- **Embedded track record:** `espressif/esp31-smsemu`, `hi631/esp32-smsemu`, `ESP_8_BIT` project.
- **Status:** Superseded by smsplus-gx in every dimension. No reason to prefer it over smsplus-gx.

### Osmose

- C++, Qt5/SDL2 required. Disqualified immediately.

### Meka / fMSX

- Meka targets desktop SDL. fMSX covers MSX hardware, not SMS. Neither has relevant embedded SMS history.

## Recommendation

**Use TotalSMS** as the primary SMS core.

Reasons in priority order:

1. **MIT licence** — no copyleft obligations; straightforward distribution.
2. **Zero global state** — `SMS_Core` is a single heap-allocated struct. Ideal for PIC shared objects where large static BSS sections cause relocation issues.
3. **Smallest RAM footprint** — ~60–80 KB vs. ~630 KB for smsplus-gx. Leaves more PSRAM headroom for ROM data and framebuffers.
4. **Cleanest port surface** — the entire platform shim fits in ~150 lines of C. Audio omitted by not registering the APU callback.
5. **Direct badge precedent** — MCH2022 (ESP32-S3, IDF, badge runtime) ran it at 60 FPS.

**Fallback: smsplus-gx** if TotalSMS accuracy gaps affect target games. The libretro shim pattern is well-understood and the port follows the same structure as the doomgeneric port.

## ESP32 SMS emulation prior art

| Project | Core | Target | Notes |
|---|---|---|---|
| `espressif/esp31-smsemu` | SMS Plus | ESP31 (prototype) | Official Espressif demo |
| `hi631/esp32-smsemu` | SMS Plus | ESP32 | Direct port |
| `esp-cpp/esp-box-emu` | smsplus | ESP32-S3 | IDF component |
| `hpvb/mch2022-esp32-app-sms` | TotalSMS | ESP32-S3 badge | Closest to BadgeVMS architecture |

## Key sources

- [ITotalJustice/TotalSMS on GitHub](https://github.com/ITotalJustice/TotalSMS)
- [libretro/smsplus-gx on GitHub](https://github.com/libretro/smsplus-gx)
- [0ldsk00l/smsplus on GitHub](https://github.com/0ldsk00l/smsplus)
- [hpvb/mch2022-esp32-app-sms on GitHub](https://github.com/hpvb/mch2022-esp32-app-sms)
- [esp-cpp/esp-box-emu on GitHub](https://github.com/esp-cpp/esp-box-emu)
