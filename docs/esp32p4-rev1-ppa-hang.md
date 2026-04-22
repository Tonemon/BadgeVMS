# ESP32-P4 Rev 1.x: PPA Hang and Software Blit Fallback

## Summary

On ESP32-P4 rev 1.x silicon (prototype WHY2025 badge hardware), the PPA
(Pixel Processing Accelerator) completion interrupt never fires. Any blocking
call to `ppa_do_scale_rotate_mirror` hangs indefinitely, freezing the
compositor task and making the badge completely unresponsive — no display
output, no keyboard input, no launcher.

The fix is a `software_blit()` fallback in the compositor, compiled in place
of the PPA path when `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`.

---

## Background: What the PPA Does

Every frame, the compositor copies each window's framebuffer into the active
display framebuffer. This is not a plain memcpy: the content must be rotated
90° to match the physical screen orientation, colour channels must be swapped
(RGB → BGR for the ST7703 panel), and arbitrary scaling must be supported for
windowed (non-fullscreen) apps.

On production hardware (rev 3.x) this is handled by the PPA, a DMA-based
hardware unit that performs scale-rotate-mirror operations in a single pass.
The compositor calls `ppa_do_scale_rotate_mirror` in blocking mode
(`PPA_TRANS_MODE_BLOCKING`), which posts the operation to hardware and then
waits on a FreeRTOS semaphore until the PPA completion ISR gives it back.

---

## Symptom Sequence

Observed on rev 1.0 badge with `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`:

1. Badge boots, DSI panel initialises, all three display framebuffers
   allocated — no errors.
2. BadgeVMS reaches the main supervision loop, launches the ELF launcher.
3. The launcher creates its window; the compositor allocates window
   framebuffers.
4. On the first render pass, `ppa_do_scale_rotate_mirror` is called.
5. The call **never returns**. The compositor task is permanently blocked
   waiting for a semaphore that will never be given.
6. Because the compositor task also drives keyboard dispatch and the frame
   swap, the badge appears completely frozen — yellow screen (DSI streaming
   uninitialised framebuffer content), keyboard silent, no further log output.

The hang was confirmed by adding diagnostic `ESP_LOGW` lines around each step
of both the DPI panel init (`esp_lcd_panel_dpi.c`) and the compositor render
loop. The log showed:

```
W compositor: PPA blit rect 0,0 720x720 -> 0,0...
```

with no following `PPA done:` line, while the launcher's own task continued
to log (allocating its second framebuffer) because it runs on a different
RTOS task.

---

## Root Cause

Espressif's own documentation notes that ESP32-P4 revisions below 3.0 and
revisions 3.0+ have "huge hardware differences" — enough that the two
families are mutually exclusive targets in IDF Kconfig. The PPA peripheral is
one of the affected blocks: its interrupt routing, DMA controller wiring, or
register layout changed between rev 1.x and rev 3.x. IDF's PPA driver was
developed and validated against rev 3.x; on rev 1.x, the hardware never
asserts the completion interrupt, so the blocking semaphore wait in
`ppa_do_scale_rotate_mirror` never unblocks.

No watchdog catches this because both `CONFIG_ESP_INT_WDT` and
`CONFIG_ESP_TASK_WDT_EN` are disabled in `sdkconfig.defaults`.

---

## The Fix: `software_blit()`

A `software_blit()` function was added to `badgevms/compositor/compositor.c`,
guarded by `#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3`. It replicates the same
pixel mapping that the PPA SRM operation performs, entirely in software:

- **All four rotation angles** (0°, 90°, 180°, 270°) using the same
  coordinate transform as `rotate_coordinates()` in `pixel_functions.h`.
- **Nearest-neighbour scaling** via integer reverse-mapping from output pixel
  to source pixel.
- **RGB565 and ARGB8888 input**, matching the `ppa_srm_color_mode_t` used by
  the compositor.
- **R/B channel swap** (`rgb_swap`), preserving the existing colour-order
  workaround for the ST7703 panel.

The PPA client is also not registered (`ppa_register_client`) on the rev 1.x
path, since the hardware is non-functional.

### Coordinate transform for the default ROTATION_ANGLE_270 case

The compositor default is `rotation = ROTATION_ANGLE_270`. `rotation_to_srm`
maps this to `PPA_SRM_ROTATION_ANGLE_90`. The software equivalent, derived
from `rotate_coordinates(ROTATION_ANGLE_270)`:

```
source (sx, sy)  →  display (sy,  719 - sx)
```

Or in terms of the `software_blit` output loop: for output pixel `(ox, oy)`,
the source pixel is:

```
sx = (out_height - 1 - oy) / scale
sy = ox / scale
```

### Cache coherency

The PPA operates as a DMA engine, reading/writing PSRAM directly and bypassing
the CPU cache. The software blit reads and writes through the CPU's L2 cache.
No extra `esp_cache_msync` is needed:

- **Source (window framebuffer):** written by the window task through L2
  cache; read by the compositor task through the same shared L2 cache —
  coherent automatically.
- **Destination (display framebuffer):** written by the software blit through
  L2 cache; flushed to PSRAM by `dpi_panel_draw_bitmap`, which detects that
  `color_data` is one of its own framebuffers and calls
  `esp_cache_msync(C2M | UNALIGNED)` before the DW-GDMA DMA transfer.

### Performance

Software rotation of 720×720 RGB565 is approximately 520k pixel
read-modify-write operations per frame. Source reads are sequential (good L2
cache locality); destination writes are strided (one cache miss per pixel in
the worst case). Measured on the badge: sufficient for the launcher and
settings UI. Demanding applications may notice lower frame rates compared to
production hardware.

---

## Files Changed

| File | Change |
|---|---|
| `badgevms/compositor/compositor.c` | Added `software_blit()`; dispatch via `#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3`; guarded `ppa_register_client` |
| `badgevms/compositor/framebuffer_private.h` | Fixed declaration mismatch: `framebuffer_allocate` now declares 3 params matching the implementation |
| `components/esp_lcd/dsi/esp_lcd_panel_dpi.c` | Pass correct `pixel_format_t` to `framebuffer_allocate`; drop unused `alignment`/`cache_line_size` variables |

---

## Related Issues

- `docs/hal-compatibility-fix.md` — broader IDF 5.5.4 porting work this fix
  is part of.
- `known-issues.md` — existing note on PPA crash for heights where
  `height > 32 && (height % 32) == 1`; that issue is on rev 3.x and is
  separate from this hang.
- `sdkconfig.defaults` — `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` and
  `CONFIG_ESP32P4_REV_MIN_100=y` enable the rev 1.x target; the
  `Kconfig.hw_support` in `components/esp_hw_support/port/esp32p4/` was
  updated from upstream IDF to expose these options.
