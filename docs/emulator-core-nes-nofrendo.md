# NES Emulator Core Research: nofrendo vs. alternatives

Research and considerations for porting a NES emulator to BadgeVMS (ESP32-P4).

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

### nofrendo — retro-go fork (ducalex/retro-go) — **recommended**

- **Language:** C (~100%), GPLv2
- **Architecture:** Fixed-singleton pattern. Each subsystem has one static instance: `static nes_t nes`, `static ppu_t ppu`, `static apu_t apu`, `static nes6502_context cpu`, `static console` in nofrendo.c. Total BSS from singletons: ~10–12 KB. All heap allocation (mapper, ROM, framebuffer) is done via `malloc` inside `*_create()` calls.
- **Global state for PIC:** ~30–50 GOT entries and relocations for the singletons. Comparable to any moderately complex C app — the ELF loader already handles this scale for doomgeneric. Not zero-globals, but fully manageable.
- **RAM footprint:** Static ~10 KB + heap: 2 KB CPU RAM + 8 KB SRAM + 16 KB PPURAM (CHR) + framebuffer + ROM. Entirely comfortable on 32 MB PSRAM.
- **Audio:** `CONFIG_SOUND_ENA=n` disables the APU execution path. The APU code still compiles in but the execution path is dead — never called if `osd_setsound(NULL)` (or never called at all). Stub-out, not compile-out. The dead APU object code adds ~30 KB to the binary but does not execute.
- **Port surface:** `osd.h` interface — `osd_init()`, `osd_shutdown()`, `osd_getvideoinfo()`, `osd_getsoundinfo()`, `osd_setsound()`, `osd_getinput()`, `osd_installtimer()`, plus `vid_flush()` to deliver a completed frame. Timer interface requires a periodic call to drive the emulator tick — implement as a loop iteration (same pattern as doomgeneric's main loop).
- **Mapper support:** 63 mappers in the retro-go fork (vs. 40 in the original Espressif demo). Covers ~75% of the commercial library including all headline titles (SMB, Zelda, Metroid, Mega Man, Contra, Castlevania, DuckTales, etc.).
- **Embedded track record:**
  - Espressif esp32-nesemu (official demo, original esp32)
  - ODROID-GO via retro-go (primary use case for the fork)
  - M5Stack, PocketSprite (8bkc), NumWorks calculator, Adafruit Arcada, VMUPro
  - ESP32-C3 RISC-V (rv32imc, 160 MHz) — NES at full speed; the P4 at 400 MHz is dramatically more capable
- **Maintenance:** retro-go fork actively maintained by ducalex. Original nofrendo upstream dormant since ~2000.
- **Licence:** GPLv2. See licence notes below.
- **Porting effort:** Estimated ~200 lines for `nes_badgevms.c`. Well-documented through a dozen existing ports; the BadgeVMS shim follows the doomgeneric pattern.

### InfoNES (jay-kumogata/InfoNES)

- **Language:** C++ (~94% C++), Apache-2.0
- **Global state:** Very heavy. In `InfoNES.cpp` alone: `RAM[0x800]` (2 KB), `SRAM[0x2000]` (8 KB), `PPURAM[0x4000]` (16 KB), `WorkFrame[256×240×2]` (~123 KB), `ChrBuf` (32 KB), plus misc tables. **Total static BSS: ~180 KB.** Every array is a GOT-addressed global in a PIC shared object — severe relocation overhead and code-size penalty on every access. RP2040 ports work around this by placing arrays in explicit PSRAM sections; the same is possible on P4 but adds build complexity.
- **Audio:** APU muted via `APU_Mute` flag; code always compiles. Same stub-out pattern as nofrendo.
- **Embedded track record:** RP2040 (pico-infonesPlus, pico-infones), STM32. No ESP32 port.
- **Mapper support:** 70+ mappers.
- **Maintenance:** Active (GitHub, recent 2024–2025 commits).
- **Licence:** Apache-2.0. Permissive — no copyleft.

### LiteNES (NJU-ProjectN/LiteNES)

- **Language:** C, minimal stdlib.
- **Audio:** No APU at all. Naturally silent.
- **Port surface:** Minimal `hal.c`.
- **Mapper support:** Effectively only Mapper 0 (NROM). Covers ~10% of titles. Unusable as a general emulator.
- **Licence:** Unclear (MIT-style but not confirmed).
- **Verdict:** Insufficient mapper coverage.

### Nestopia UE, FCEUX, Mesen

All C++, desktop-scale (tens of MB RAM, STL, GUI frameworks). No microcontroller ports exist or are feasible. Not viable.

### PocketNES

ARM Thumb assembly hardcoded for GBA hardware (VRAM addresses, DMA). Non-starter on RISC-V.

## Licence notes

nofrendo is GPLv2. For a hacker badge where the emulator app source is distributed openly (as is standard for WHY2025 badge apps), GPLv2 is compatible with BadgeVMS's own GPLv3 licence — GPLv2 code may be incorporated into a GPLv3 project when distributing source alongside the binary. The NES emulator app would need to carry a GPLv2 notice and distribute source, which is standard practice for open-source badge apps.

If a fully permissive (non-copyleft) licence is required, **InfoNES (Apache-2.0)** is the only viable alternative — accepting the 122+ KB global BSS overhead, C++ build requirement, and stub-out (not compile-out) audio.

## Recommendation

**Use the retro-go fork of nofrendo.**

1. **Proven on ESP32 RISC-V** — already runs NES at 60 fps on the ESP32-C3 (rv32imc, 160 MHz). The P4 at 400 MHz with 32 MB PSRAM is dramatically more capable.
2. **Best mapper coverage of any C/embedded option** — 63 mappers in the retro-go fork covers all popular titles.
3. **Well-understood port surface** — `osd.h` is documented through a dozen existing ports; the BadgeVMS shim follows exactly the doomgeneric pattern.
4. **Audio removal is practical** — `osd_setsound(NULL)` + `CONFIG_SOUND_ENA=n`; APU never executes. ~30 KB of dead code in the binary is not a meaningful cost with PSRAM available.
5. **Manageable global state** — ~10 KB BSS singletons generate ~30–50 GOT entries; within the normal range for PIC C apps. Not zero-globals, but not a real problem.
6. **GPLv2 is compatible** with open-source badge app distribution alongside BadgeVMS (GPLv3).

**Fallback: InfoNES (Apache-2.0)** if fully permissive licence becomes a hard requirement. Accept the 122+ KB BSS overhead (mitigated by explicit PSRAM placement) and C++ build dependency.

## Core lineup summary

| System | Core | Licence | Global state | ESP32 proven |
|---|---|---|---|---|
| NES | nofrendo (retro-go fork) | GPLv2 | ~10 KB singletons | Yes, extensively |
| GB/GBC | peanut-gb | MIT | None (struct-based) | Yes (MCH2022 badge) |
| SMS | TotalSMS | MIT | None (struct-based) | Yes (MCH2022 badge) |

## ESP32 NES emulation prior art

| Project | Core | Target | Notes |
|---|---|---|---|
| `espressif/esp32-nesemu` | nofrendo | ESP32 | Official Espressif demo |
| `ducalex/retro-go` | nofrendo fork | ODROID-GO | Extended mapper support; most maintained fork |
| `PocketSprite/8bkc-nofrendo` | nofrendo | PocketSprite | Tiny ESP32 handheld |
| `AppCakeLtd/vmupro-nofrendo` | nofrendo | VMUPro | Dreamcast VMU replacement |
| ESP32-C3 RISC-V NES | nofrendo | ESP32-C3 | Runs at full 60 fps on rv32imc 160 MHz |

## Key sources

- [ducalex/retro-go on GitHub](https://github.com/ducalex/retro-go)
- [espressif/esp32-nesemu on GitHub](https://github.com/espressif/esp32-nesemu)
- [jay-kumogata/InfoNES on GitHub](https://github.com/jay-kumogata/InfoNES)
- [NJU-ProjectN/LiteNES on GitHub](https://github.com/NJU-ProjectN/LiteNES)
- [NES emulator on $1 ESP32-C3 RISC-V](https://rvembedded.com/blog_post/2/)
- [fhoedemakers/pico-infonesPlus on GitHub](https://github.com/fhoedemakers/pico-infonesPlus)
