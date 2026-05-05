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

#include "badgevms/bluetooth.h"
#include "badgevms/device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"

#define BT_MAX_SCAN_RESULTS 32
#define BT_MAX_PAIRED       16
#define BT_MSG_MAX_LEN      512

#define BT_CONNECTED_BIT    BIT0
#define BT_DISCONNECTED_BIT BIT1
#define BT_SCAN_DONE_BIT    BIT2

typedef enum {
    BT_COMMAND_SCAN,
    BT_COMMAND_CONNECT,
    BT_COMMAND_DISCONNECT,
    BT_COMMAND_SEND_MESSAGE,
} bt_command_t;

struct bt_device {
    char             name[64];
    char             addr_str[18]; /* "AA:BB:CC:DD:EE:FF" */
    ble_addr_t       addr;
    bt_device_type_t type;
    uint16_t         conn_handle;  /* BLE_HS_CONN_HANDLE_NONE if not connected */
    bt_connection_status_t conn_status;
};

typedef struct {
    TaskHandle_t  caller;
    bt_command_t  command;
    void         *arg;
} bt_command_message_t;

typedef struct {
    void (*on_device_found)(const struct bt_device *dev);
    void (*on_connected)(struct bt_device *dev);
    void (*on_disconnected)(struct bt_device *dev);
    void (*on_data)(struct bt_device *dev, const uint8_t *data, size_t len);
} bt_profile_t;

typedef struct {
    bt_status_t        status;
    SemaphoreHandle_t  mutex;
    EventGroupHandle_t event_group;
    char               own_name[32];

    struct bt_device   scan_results[BT_MAX_SCAN_RESULTS];
    int                num_scan_results;

    struct bt_device   paired[BT_MAX_PAIRED];
    int                num_paired;
} bt_iris_state_t;

typedef struct {
    device_t device;
} bluetooth_device_t;

/* Registered profiles — set during bluetooth_create() */
extern bt_profile_t bt_hid_profile;
extern bt_profile_t bt_badge_profile;
extern bt_iris_state_t iris_state;

void badge_profile_register_services(void);
device_t *bluetooth_create(void);
