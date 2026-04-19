# IDF 5.5.4 Compatibility — Multi-Component Porting Work

## The Bigger Picture

The firmware overrides 8 IDF components with badge-specific forks:

| Component | Why Forked |
|---|---|
| `hal` | hw_ver3 chip support before IDF caught up; custom PHY/crypto/TWAI code |
| `esp_hw_support` | SPM/TCM region handling, LP IO management |
| `esp_psram` | PSRAM virtual address window management |
| `esp_driver_ppa` | PPA color pipeline extensions for the badge display |
| `esp_lcd` | Compositor integration |
| `freertos` | Virtual memory system (Zeus/Hades/Cerberos process model) |
| `esp_http_client` | curl emulation layer |
| `esp-tls` | TLS integration |

All 8 have genuine badge-specific changes — none can be simply deleted.

The firmware targets **IDF 5.5** but the installed IDF is `v5.5.4-299-ge46782886f` —
299 commits ahead of the v5.5 tag. Every API change in those 299 commits that
touches a forked component must be manually reconciled. This document tracks that work.

---

## The HAL Problem (Root Cause)

### What went wrong

The firmware's custom `components/hal/` became a stale fork. The original reason for
forking — ESP32-P4 hw_ver3 not yet supported upstream — was resolved by IDF, but the
fork was never cleaned up. As IDF evolved toward hw_ver3, the firmware's stale copies
silently intercepted every `#include <hal/xxx_ll.h>` and served outdated code.

### Root cause 1 — Wrong chip revision in Kconfig

`components/esp_hw_support/port/esp32p4/Kconfig.hw_support` only defined
revisions 0, 1, and 100 (hw_ver1 era) and defaulted to rev 1:

```kconfig
choice ESP32P4_REV_MIN
    default ESP32P4_REV_MIN_1   ← wrong
    config ESP32P4_REV_MIN_0
    config ESP32P4_REV_MIN_1
    config ESP32P4_REV_MIN_100
    # rev 300/301 missing entirely
endchoice
```

This set `CONFIG_ESP_REV_MIN_FULL=1`, activating hw_ver1 code paths while SOC
headers targeted hw_ver3. Every hw_ver3-only register access failed.

**Fix:** Updated to offer only rev 3.0/3.1 and locked `sdkconfig.defaults` to
`CONFIG_ESP32P4_REV_MIN_300=y`.

### Root cause 2 — Stale HAL header overrides (~90 files)

`components/hal/esp32p4/include/hal/` was frozen at the time the firmware was first
written. **Fix:** Deleted entirely. CMakeLists now falls back to IDF's target headers.

### Root cause 3 — Trivial HAL .c copies (~55 files)

Most `components/hal/*.c` files were byte-for-byte copies of IDF's versions.
**Fix:** Deleted. CMakeLists now resolves each listed source: local copy if it exists,
otherwise IDF's version. No trivial copy maintenance required.

---

## HAL Files Intentionally Kept (Genuine Badge Changes)

These files have firmware-specific logic and remain as overrides:

| File | Why Kept |
|---|---|
| `apm_hal.c` | `apm_tee_hal_set_master_secure_mode*` functions IDF removed |
| `cache_hal.c` | Different IRAM attribute handling for badge memory layout |
| `gdma_hal_axi.c` + `gdma_hal_top.c` | Encrypted memory access extensions |
| `mipi_dsi_hal.c` | Custom PHY PLL calculation for the badge display |
| `key_mgr_hal.c` | XTS-AES key length functions IDF doesn't expose |
| `spi_hal.c` + `spi_hal_iram.c` | Additional interrupt mask functions |
| `ecc_hal.c` | SECP192/256 curve detection using size-based logic |
| `ecdsa_hal.c` | Key manager integration guards |
| `usb_dwc_hal.c` | USB core ID verification and reset logic |
| `mpi_hal.c` | Explicit power up/down calls |
| `huk_hal.c` | ROM function call wrapping |
| `cam_hal.c` | DMA bit-order reversal |
| `gpio_hal.c` | Output enable control variant |
| `touch_sens_hal.c` | Shield output bypass (now: stub, bypass removed for hw_ver3) |
| `twai_hal_ctufd.c` + `twai_hal_sja1000.c` | Firmware uses old file names |

---

## Fixes Applied (IDF 5.5.4 drift)

### hal — type headers

| File | Change |
|---|---|
| `color_types.h` | 4 YUV422 pixel format variants; `color_macroblock_yuv_data_t` struct |
| `ppa_types.h` | YUV420/422 variants and GRAY8 in srm/blend/fill color mode enums |
| `jpeg_types.h` | `JPEG_ENC_SRC_YUV444` and `JPEG_ENC_SRC_YUV420` |
| `key_mgr_types.h` | Full restructure: XTS types first, ECDSA split into 192/256/384; compat aliases kept |
| `spi_flash_hal.h` | `trs_val` field added to context and config structs |
| `adc_types.h` | `ADC_CHANNEL_10` |
| `dma2d_types.h` | YUV422/444 CSC conversion enum values |
| `ecdsa_types.h` | `HAL_ECDSA_COMBINE_KEY_BLOCKS` / `HAL_ECDSA_EXTRACT_KEY_BLOCKS` |
| `isp_types.h` | `isp_wbg_gain_t` struct |
| `lcd_types.h` | `LCD_COLOR_FMT_GRAY8` |
| `mcpwm_types.h` | `mcpwm_timer_etm_event_type_t` |
| `mipi_dsi_types.h` | PHY PLL ref clock source type renamed; compat alias added |
| `pmu_types.h` | `PMU_HP_PD_CPU=3` for ESP32-P4 |

### hal — source files

| File | Change |
|---|---|
| `twai_hal_sja1000.c` | `twai_ll_parse_frame_buffer` split into `twai_ll_parse_frame_header` + `twai_ll_parse_frame_data`; `hal_ctx` added as first param to `twai_hal_parse_frame` |
| `twai_hal.h` | `timer_freq` field added to config; `twai_hal_parse_frame` signature updated |
| `mipi_dsi_hal.c` | `mipi_dsi_brg_ll_set_pixel_format` → `mipi_dsi_brg_ll_set_input_color_format`; `sub_config` param dropped |
| `spi_hal.c` | `spi_ll_set_mosi_free_level` → `spi_ll_set_data_pin_idle_level` |
| `spi_hal.h` | Function renamed + compat alias; `spi_hal_clear/get_intr_mask` moved out of `SOC_SPI_SCT_SUPPORTED` guard |
| `touch_sens_hal.c` | Removed `touch_ll_sample_cfg_bypass_shield_output` call (removed from IDF; hw_ver3 doesn't support shield bypass) |
| `usb_dwc_hal.c` | Removed polling loop for `usb_dwc_ll_grstctl_is_core_soft_reset_in_progress` (IDF absorbed into the LL function itself) |
| `ledc_hal_iram.c` | `bool duty_start` param removed from `ledc_hal_set_duty_start` |
| `ledc_hal.h` | `ledc_hal_get_fade_end_intr_addr` inline added |
| `uart_hal.h` | `uart_hal_get_intr_status_reg` macro added |
| `platform_port/include/hal/config.h` | New file required by IDF's LL headers |

### esp_hw_support

| File | Change |
|---|---|
| `port/esp32p4/cpu_region_protect.c` | `SOC_TCM_LOW/HIGH` → `SOC_SPM_LOW/HIGH` |
| `include/esp_memory_utils.h` | Same rename; guard extended to `SOC_MEM_TCM_SUPPORTED \|\| SOC_MEM_SPM_SUPPORTED` |
| `include/esp_private/io_mux.h` | `MAX_RTC_GPIO_NUM` → `SOC_RTCIO_PIN_COUNT`; `io_mux_is_lp_io_in_use()` declaration added |
| `port/esp32p4/io_mux.c` | Assert updated; `io_mux_is_lp_io_in_use()` implementation added |
| `dma/include/esp_private/gdma_link.h` | `gdma_final_node_link_type_t` enum added; `buffer_alignment` field added; `mark_final` type changed to `gdma_final_node_link_type_t:2`; `gdma_link_get_buffer` and `gdma_link_get_length` declarations added |
| `include/esp_cpu.h` | `esp_cpu_intr_set_xtvt_addr()` added for CLIC vectored interrupt table |

### esp_psram

| File | Change |
|---|---|
| `include/esp_private/esp_psram_mspi.h` | New file (copied from IDF); provides `esp_psram_mspi_mb_init()` and `esp_psram_mspi_mb()` |
| `CMakeLists.txt` | IDF's `system_layer/esp_psram_mspi.c` added to source list |

---

## Current Status

| Component | Status |
|---|---|
| `hal` | **Complete** — stale files deleted, all known API drift fixed |
| `esp_hw_support` | **Complete** — SPM rename, io_mux, GDMA, esp_cpu all fixed |
| `esp_psram` | **Complete** — mspi additions done |
| `esp_driver_ppa` | **In progress** — two errors remain (see below) |
| `esp_lcd` | **Unknown** — not yet reached in build |
| `freertos` | **Unknown** — not yet reached in build |
| `esp_http_client` | **Unknown** — likely low drift (stable API surface) |
| `esp-tls` | **Unknown** — likely low drift (stable API surface) |

### Remaining blocking errors

**`components/esp_driver_ppa/src/ppa_srm.c`:**
```c
.block_h = (srm_trans_desc->in.srm_cm == PPA_SRM_COLOR_MODE_YUV420)
    ? PPA_LL_SRM_YUV420_BLOCK_SIZE : PPA_LL_SRM_DEFAULT_BLOCK_SIZE,
```
`PPA_LL_SRM_YUV420_BLOCK_SIZE` and `PPA_LL_SRM_DEFAULT_BLOCK_SIZE` are LL-level
constants added in IDF 5.5.4. Need to check IDF's `ppa_ll.h` for values and either
define them in the firmware's ppa_ll.h or update ppa_srm.c.

**`components/esp_driver_ppa/src/ppa_fill.c`:**
```c
// Firmware calls:
ppa_ll_blend_configure_filling_block(platform->hal.dev,
    &fill_trans_desc->fill_argb_color,
    fill_trans_desc->fill_block_w,
    fill_trans_desc->fill_block_h);

// IDF 5.5.4 signature:
void ppa_ll_blend_configure_filling_block(ppa_dev_t *dev,
    ppa_fill_color_mode_t color_mode,
    void *data,
    uint32_t hb, uint32_t vb);
```
A `color_mode` parameter was added as the second argument. The call site needs
to pass the appropriate color mode and confirm the `data` semantics match.

---

## Effort Estimate

Estimating against the 299 IDF commits of drift:

| Area | Fraction of drift | Status |
|---|---|---|
| hal (stale file cleanup + type/API fixes) | ~40% | Done |
| esp_hw_support | ~15% | Done |
| esp_psram | ~5% | Done |
| esp_driver_ppa | ~15% | ~50% done |
| esp_lcd | ~15% | Not started |
| freertos, esp_http_client, esp-tls | ~10% | Not started |

**Rough estimate: ~60-65% of the total reconciliation work is complete.**

The hal cleanup was the largest single chunk — not because it had the most API changes,
but because it required identifying and deleting ~145 stale files, building the CMakeLists
fallback mechanism, and then fixing a dozen API drift points across the remaining kept files.

The remaining work (esp_driver_ppa tail, esp_lcd, freertos) will surface errors only
when the build progresses far enough to compile those components. esp_lcd and freertos
may have significant drift given their complexity, but their API surfaces are also more
stable than the low-level HAL and hardware support layers.

---

## Key Lesson

When a project forks IDF's HAL to work around missing chip support, set a reminder
to **delete the fork when IDF catches up**. The fork becomes actively harmful once IDF
adds proper support — it silently intercepts every include and serves stale code.

The tell: if your custom HAL component has files byte-for-byte identical to IDF's
(modulo copyright year), the fork has outlived its purpose.

For the remaining components (`esp_driver_ppa`, `esp_lcd`, etc.), the lesson applies
differently: these have genuine badge-specific changes that must be maintained. The
right approach is a periodic diff against IDF's upstream version to catch drift early,
rather than discovering it all at once during a build.
