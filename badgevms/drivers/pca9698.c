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

#include "pca9698.h"

#include "badgevms_i2c_bus.h"
#include "badgevms/device.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

#define TAG "PCA9698"

#define PCA9698_REG_OP0  0x08
#define PCA9698_REG_IOC0 0x18
#define PCA9698_AI       0x80

typedef struct {
    device_t     device;
    i2c_device_t *i2c_dev;
} pca9698_device_t;

static int pca9698_open(void *dev, path_t *path, int flags, mode_t mode) {
    (void)dev;
    (void)path;
    (void)flags;
    (void)mode;
    return 0;
}

static int pca9698_close(void *dev, int fd) {
    (void)dev;
    if (fd == 0) {
        return 0;
    }
    return -1;
}

static ssize_t pca9698_write(void *dev, int fd, void const *buf, size_t count) {
    (void)fd;
    pca9698_device_t *d = (pca9698_device_t *)dev;
    return d->i2c_dev->device._write(d->i2c_dev, PCA9698_REG_OP0 | PCA9698_AI, buf, count);
}

static ssize_t pca9698_read(void *dev, int fd, void *buf, size_t count) {
    (void)dev;
    (void)fd;
    (void)buf;
    (void)count;
    return -1;
}

static ssize_t pca9698_lseek(void *dev, int fd, off_t offset, int whence) {
    (void)dev;
    (void)fd;
    (void)offset;
    (void)whence;
    return -1;
}

static void pca9698_destroy(void *dev) {
    free(dev);
}

device_t *pca9698_create(uint8_t i2c_address) {
    pca9698_device_t *d = calloc(1, sizeof(pca9698_device_t));
    if (!d) {
        ESP_LOGE(TAG, "Failed to allocate pca9698_device_t");
        return NULL;
    }

    device_t *bus = device_get("I2CBUS0");
    if (!bus) {
        ESP_LOGE(TAG, "I2CBUS0 not found");
        free(d);
        return NULL;
    }

    i2c_device_t *i2c_dev = ((i2c_bus_device_t *)bus)->_device_create(bus, i2c_address, 400000);
    if (!i2c_dev) {
        ESP_LOGE(TAG, "Failed to create I2C device at 0x%02x", i2c_address);
        free(d);
        return NULL;
    }

    d->i2c_dev = i2c_dev;

    /* Configure all 5 banks as outputs (IOC = 0x00) */
    uint8_t zeros[5] = {0};
    if (i2c_dev->device._write(i2c_dev, PCA9698_REG_IOC0 | PCA9698_AI, zeros, 5) < 0) {
        ESP_LOGE(TAG, "Failed to configure IOC registers");
    }

    /* Clear all outputs */
    if (i2c_dev->device._write(i2c_dev, PCA9698_REG_OP0 | PCA9698_AI, zeros, 5) < 0) {
        ESP_LOGE(TAG, "Failed to clear OP registers");
    }

    d->device.type    = DEVICE_TYPE_LED_MATRIX;
    d->device._open   = pca9698_open;
    d->device._close  = pca9698_close;
    d->device._write  = pca9698_write;
    d->device._read   = pca9698_read;
    d->device._lseek  = pca9698_lseek;
    d->device._destroy = pca9698_destroy;

    return &d->device;
}
