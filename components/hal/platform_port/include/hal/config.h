/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_CONFIG(x) HAL_CONFIG_##x

#define HAL_CONFIG_CHIP_SUPPORT_MIN_REV CONFIG_ESP_REV_MIN_FULL

#ifdef __cplusplus
}
#endif
