/* This file is part of BadgeVMS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "keyboard.h"
#include "pathfuncs.h"

#include <stddef.h>
#include <stdint.h>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef enum {
    DEVICE_TYPE_BLOCK,
    DEVICE_TYPE_LCD,
    DEVICE_TYPE_KEYBOARD,
    DEVICE_TYPE_BUS,
    DEVICE_TYPE_WIFI,
    DEVICE_TYPE_ORIENTATION,
    DEVICE_TYPE_SOCKET,
    DEVICE_TYPE_FILESYSTEM,
    DEVICE_TYPE_GAS,
    DEVICE_TYPE_LED_MATRIX,
    DEVICE_TYPE_BLUETOOTH,
} device_type_t;

typedef enum { ORIENTATION_0, ORIENTATION_90, ORIENTATION_180, ORIENTATION_270 } orientation_t;

typedef struct device {
    device_type_t type;
    int (*_open)(void *dev, path_t *path, int flags, mode_t mode);
    int (*_close)(void *dev, int fd);
    ssize_t (*_write)(void *dev, int fd, void const *buf, size_t count);
    ssize_t (*_read)(void *dev, int fd, void *buf, size_t count);
    ssize_t (*_lseek)(void *dev, int fd, off_t offset, int whence);
    void (*_destroy)(void *dev);
} device_t;

typedef struct filesystem {
    device_t device;
    int (*_stat)(void *dev, path_t *path, struct stat *restrict statbuf);
    int (*_fstat)(void *dev, int fd, struct stat *restrict statbuf);
    int (*_unlink)(void *dev, path_t *path);
    int (*_rename)(void *dev, path_t *oldpath, path_t *newpath);
    int (*_mkdir)(void *dev, path_t *path, mode_t mode);
    int (*_rmdir)(void *dev, path_t *path);
    DIR *(*_opendir)(void *dev, path_t *path);
    struct dirent *(*_readdir)(void *dev, DIR *dirp);
    int (*_closedir)(void *dev, DIR *dirp);
} filesystem_device_t;

typedef struct lcd_device {
    device_t device;
    void (*_draw)(void *dev, int x, int y, int w, int h, void *pixels);
    void (*_getfb)(void *dev, int num, void **pixels);
    void (*_set_refresh_cb)(void *dev, void *user_data, void (*callback)(void *user_data));
} lcd_device_t;

typedef struct keyboard_device {
    device_t device;
} keyboard_device_t;

typedef struct {
    uint8_t address;
} i2c_scanresult_t;

typedef struct {
    device_t device;
    uint8_t (*_get_address)(void *dev);
} i2c_device_t;

typedef struct {
    device_t device;
    int (*_scan)(void *dev, i2c_scanresult_t *results, int num);
    i2c_device_t *(*_device_create)(void *dev, uint8_t address, uint32_t clk_speed);
} i2c_bus_device_t;

typedef struct {
    device_t device;
} wifi_device_t;

typedef struct {
    device_t device;
} socket_device_t;

typedef struct {
    device_t device;
    orientation_t (*_get_orientation)(void *dev);
    int (*_get_orientation_degrees)(void *dev);
} orientation_device_t;

typedef struct {
    device_t device;
    float (*_get_pressure)(void *dev);
    float (*_get_temperature)(void *dev);
    float (*_get_gas_resistance)(void *dev);
    float (*_get_humidity)(void *dev);
} gas_device_t;

device_t *device_get(char const *name);
