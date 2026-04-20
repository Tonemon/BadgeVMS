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

**Build result: `Project build complete` — all 8 components compile and link cleanly.**

| Component | Status | Fixes Applied |
|---|---|---|
| `hal` | **Complete** | Stale files deleted, chip rev fixed, all API drift resolved |
| `esp_hw_support` | **Complete** | SPM rename, io_mux, GDMA, esp_cpu all fixed |
| `esp_psram` | **Complete** | mspi additions done |
| `esp_driver_ppa` | **Complete** | `ppa_srm.c`: constants → `ppa_ll_srm_get_dma_dscr_port_mode_block_size()`; `ppa_fill.c`: added `color_mode` arg to `ppa_ll_blend_configure_filling_block()` |
| `esp_lcd` | **Complete** | parl: `parlio_private.h` → `parlio_tx_private.h`; i80/rgb: `lcd_ll_enable_interrupt()` wrapped in `PERIPH_RCC_ATOMIC()`; dsi/bus: PHY clock API split; dsi/dpi: renamed underrun event + color range function |
| `freertos` | **Complete** | Added `xPortFPUContextIsDirty()` (FPU sleep retention); added `hp_system_reg.h` include for ESP32-P4; added two OpenOCD debug table entries |
| `esp_http_client` | **No drift** | API surface unchanged across 299 commits |
| `esp-tls` | **No drift** | API surface unchanged across 299 commits |

### Resolved blocking errors (all fixed)

| File | Error | Fix |
|---|---|---|
| `ppa_srm.c` | `PPA_LL_SRM_YUV420_BLOCK_SIZE` undeclared | Replaced with `ppa_ll_srm_get_dma_dscr_port_mode_block_size()` |
| `ppa_fill.c` | `ppa_ll_blend_configure_filling_block` wrong arg count | Added `fill_cm` as second argument |
| `esp_lcd_panel_io_parl.c` | `parlio_private.h` not found | Changed to `parlio_tx_private.h` |
| `esp_lcd_panel_io_i80.c` | `__DECLARE_RCC_ATOMIC_ENV` undeclared | Wrapped `lcd_ll_enable_interrupt()` in `PERIPH_RCC_ATOMIC()` |
| `esp_lcd_panel_rgb.c` | `__DECLARE_RCC_ATOMIC_ENV` undeclared | Wrapped `lcd_ll_enable_interrupt()` in `PERIPH_RCC_ATOMIC()` |
| `esp_lcd_mipi_dsi_bus.c` | `mipi_dsi_ll_enable_phy_reference_clock` undeclared | Renamed to `mipi_dsi_ll_enable_phy_pllref_clock` |
| `esp_lcd_mipi_dsi_bus.c` | `mipi_dsi_ll_set_phy_clock_source` undeclared | Replaced with split API: `set_phy_config_clock_source` + `set_phy_pllref_clock_source` + `set_phy_pll_ref_clock_div` |
| `esp_lcd_panel_dpi.c` | `MIPI_DSI_LL_EVENT_UNDERRUN` undeclared | Renamed to `MIPI_DSI_BRG_LL_EVENT_UNDERRUN` |
| `esp_lcd_panel_dpi.c` | `mipi_dsi_brg_ll_set_input_color_space` undeclared | Renamed to `mipi_dsi_brg_ll_set_input_color_range` |

### Latent runtime fix

**`freertos/port.c` missing `xPortFPUContextIsDirty()`:**
IDF 5.5.4 added this function for FPU register retention during light sleep
(guards: `SOC_CPU_COPROC_NUM > 0 && SOC_CPU_HAS_FPU && SOC_PM_FPU_RETENTION_BY_SW`,
all true for ESP32-P4). Without it, the badge would crash on any light sleep
cycle where a task had used the FPU. Added to `port.c` and declared in `portmacro.h`.

---

## Effort Summary

| Area | Fraction of drift | Status |
|---|---|---|
| hal (stale file cleanup + type/API fixes) | ~40% | Complete |
| esp_hw_support | ~15% | Complete |
| esp_psram | ~5% | Complete |
| esp_driver_ppa | ~15% | Complete |
| esp_lcd | ~15% | Complete |
| freertos | ~8% | Complete |
| esp_http_client, esp-tls | ~2% | No changes needed |

**100% of the IDF 5.5.4 compatibility reconciliation is complete.**

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
