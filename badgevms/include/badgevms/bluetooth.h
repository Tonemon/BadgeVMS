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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    BT_DISABLED,
    BT_ENABLED,
} bt_status_t;

typedef enum {
    BT_DISCONNECTED,
    BT_CONNECTING,
    BT_CONNECTED,
} bt_connection_status_t;

typedef enum {
    BT_DEVICE_UNKNOWN,
    BT_DEVICE_KEYBOARD,
    BT_DEVICE_BADGE,
} bt_device_type_t;

typedef struct bt_device *bt_device_handle;

const char           *bt_device_get_name(bt_device_handle dev);
const char           *bt_device_get_addr(bt_device_handle dev);
bt_device_type_t      bt_device_get_type(bt_device_handle dev);
void                  bt_device_free(bt_device_handle dev);

void        bt_set_enabled(bool enabled);
bt_status_t bt_get_status(void);

void        bt_set_own_name(const char *name);
const char *bt_get_own_name(void);

void             bt_scan_start(void);
int              bt_scan_get_num_results(void);
bt_device_handle bt_scan_get_result(int index);

bt_connection_status_t bt_connect(bt_device_handle dev);
bt_connection_status_t bt_disconnect(bt_device_handle dev);
bt_connection_status_t bt_get_connection_status(bt_device_handle dev);

int              bt_get_num_paired(void);
bt_device_handle bt_get_paired(int index);
void             bt_forget_device(bt_device_handle dev);

bool bt_send_message(bt_device_handle dev, const char *msg, size_t len);
