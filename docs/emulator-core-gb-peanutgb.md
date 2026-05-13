# GB/GBC Emulator Core Research: peanut-gb vs. gnuboy

Research and considerations for porting a Game Boy / Game Boy Color emulator to BadgeVMS (ESP32-P4).

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

### peanut-gb (deltabeard/Peanut-GB) — **recommended**

- **Language:** C (single header, `peanut_gb.h`), MIT licence
- **Architecture:** Single `struct gb_s` contains all emulator state. Host allocates it. No global variables, no static arrays. Purpose-built for embedded targets.
- **GBC support:** Yes — handles both `.gb` and `.gbc` files in the same binary.
- **RAM footprint:** The `struct gb_s` is large (~100–200 KB depending on compile flags) but entirely on the heap. No BSS globals. ROM is not copied — host provides read callbacks into its own ROM buffer.
- **Platform surface:** Host implements a small set of callbacks:
  ```c
  // Required
  uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr);
  uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr);
  void    gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val);
  void    gb_error(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t addr);

  // Per-scanline rendering (called 144 times per frame)
  void lcd_draw_line(struct gb_s *gb,
                     const uint8_t *pixels,   // 160 pixels, palette indices or RGB
                     const uint_fast8_t line);
  ```
  Audio callbacks only needed if `ENABLE_SOUND` is defined — omit the define entirely to compile with no audio code at all.
- **Rendering model:** Scanline-based. `lcd_draw_line` is called once per rendered line. Host blits each line into its framebuffer. This is memory-friendly: no intermediate full-frame buffer inside the core.
- **Embedded track record:**
  - Raspberry Pi Pico (multiple community ports, runs at full speed)
  - MCH2022 badge (ESP32-S3, 8 MB PSRAM) — direct badge app port
  - Badge.team ESP32 badge hardware
  - HAGL graphics library embedded demos
  - Numerous Cortex-M and RISC-V bare-metal projects
- **Porting effort:** Estimated ~150–200 lines for `gb_badgevms.c`. The scanline callback maps naturally onto BadgeVMS framebuffer rows.
- **Known gotchas:**
  - The scanline callback is called from inside `gb_run_frame()` — no separate "end of frame" hook. If the host needs to do something after all 144 lines, it does so after `gb_run_frame()` returns.
  - `PEANUT_GB_HIGH_LCD_ACCURACY` increases accuracy at higher CPU cost; for P4 at 400 MHz this should be fine, but can be disabled if frame rate suffers.
  - Cart RAM persistence (save files) requires the host to flush `gb_cart_ram_read` backing storage to SD on exit.

### gnuboy

- **Language:** C, GPL-2-or-later
- **Architecture:** Traditional global/static state spread across multiple compilation units. Large static arrays for CPU state, video, memory maps.
- **GBC support:** Partial/legacy — original gnuboy had limited GBC support; the `libretro/gnuboy` fork improved it.
- **RAM footprint:** Similar total to peanut-gb, but distributed across global BSS. The global state is problematic for PIC shared objects: large BSS sections require extra relocations and inflate the ELF.
- **Platform surface:** Less clean abstraction; porting involves modifying internal `sys_*` functions rather than providing typed callbacks.
- **Embedded track record:**
  - `espressif/esp32-gnuboy` — official Espressif demo port, proves it runs on ESP32.
  - Less commonly used on badge hardware than peanut-gb in recent years.
- **Licence:** GPL-2-or-later (compatible with BadgeVMS's GPL, but more restrictive than MIT).
- **Status:** The upstream gnuboy repo is largely dormant. Active development moved to the libretro fork.

## Feature comparison

| Feature | peanut-gb | gnuboy |
|---|---|---|
| Licence | MIT | GPL-2+ |
| Global state | None (struct-based) | Significant |
| PIC-friendly | Yes | Awkward |
| GB support | Yes | Yes |
| GBC support | Yes | Partial/fork |
| Audio disable | `#undef ENABLE_SOUND` | Requires stubs |
| Scanline rendering | Yes (callback) | Full-frame |
| Embedded ports | Many (Pico, badges) | Some (esp32) |
| Maintenance | Active | Mostly dormant |
| RAM footprint | ~100–200 KB heap | ~similar, BSS |

## Recommendation

**Use peanut-gb** as the GB/GBC emulator core.

Reasons in priority order:

1. **MIT licence** — consistent with TotalSMS; no copyleft obligations.
2. **Zero global state** — `struct gb_s` is a single heap allocation. Ideal for PIC shared objects; no large BSS relocation issues.
3. **Audio compile-time opt-out** — `ENABLE_SOUND` is simply not defined. Zero audio code compiled in. No stubs needed.
4. **Native GBC support** — handles both `.gb` and `.gbc` in the same binary; no separate GBC core needed.
5. **Scanline rendering model** — fits naturally into BadgeVMS framebuffer access; each `lcd_draw_line` call writes one row of the framebuffer directly.
6. **Extensive embedded/badge track record** — specifically ported to ESP32-S3 badge hardware (MCH2022). Closest prior art to BadgeVMS.
7. **Active maintenance** — unlike gnuboy upstream, peanut-gb receives regular fixes and accuracy improvements.

**Fallback: libretro/gnuboy** if peanut-gb compatibility gaps affect target games. The esp32-gnuboy prior art means it is a known-good option on this hardware family.

## ESP32 GB emulation prior art

| Project | Core | Target | Notes |
|---|---|---|---|
| `espressif/esp32-gnuboy` | gnuboy | ESP32 | Official Espressif demo |
| `hpvb/mch2022-esp32-app-gb` | peanut-gb | ESP32-S3 badge | Closest to BadgeVMS architecture |
| Various Pico ports | peanut-gb | RP2040 | Full speed GB/GBC |
| Badge.team ports | peanut-gb | ESP32 badge | Community badge ports |

## Key sources

- [deltabeard/Peanut-GB on GitHub](https://github.com/deltabeard/Peanut-GB)
- [libretro/gnuboy on GitHub](https://github.com/libretro/gnuboy)
- [espressif/esp32-gnuboy on GitHub](https://github.com/espressif/esp32-gnuboy)
- [MCH2022 badge GB app](https://github.com/hpvb/mch2022-esp32-app-gb)
