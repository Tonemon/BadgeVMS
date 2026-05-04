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

#include "bluetooth_internal.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "task.h"

#include <string.h>

#define TAG "bluetooth"
#define NVS_NS "badgevms_bt"

bt_iris_state_t iris_state;
static QueueHandle_t iris_queue;
static TaskHandle_t  iris_handle;

static void nvs_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    size_t sz = sizeof(iris_state.own_name);
    nvs_get_str(h, "own_name", iris_state.own_name, &sz);

    uint8_t count = 0;
    nvs_get_u8(h, "paired_count", &count);
    if (count > BT_MAX_PAIRED) count = BT_MAX_PAIRED;
    iris_state.num_paired = count;

    for (int i = 0; i < count; i++) {
        char key[24];
        sz = sizeof(iris_state.paired[i].name);
        snprintf(key, sizeof(key), "paired_%d_name", i);
        nvs_get_str(h, key, iris_state.paired[i].name, &sz);

        sz = sizeof(iris_state.paired[i].addr_str);
        snprintf(key, sizeof(key), "paired_%d_addr", i);
        nvs_get_str(h, key, iris_state.paired[i].addr_str, &sz);

        uint8_t type_u8 = BT_DEVICE_UNKNOWN;
        snprintf(key, sizeof(key), "paired_%d_type", i);
        nvs_get_u8(h, key, &type_u8);
        iris_state.paired[i].type = (bt_device_type_t)type_u8;
        iris_state.paired[i].conn_handle = BLE_HS_CONN_HANDLE_NONE;
        iris_state.paired[i].conn_status = BT_DISCONNECTED;
    }

    nvs_close(h);
}

static void nvs_save_paired(void) {
    xSemaphoreTake(iris_state.mutex, portMAX_DELAY);
    int count = iris_state.num_paired;
    struct bt_device snap[BT_MAX_PAIRED];
    memcpy(snap, iris_state.paired, count * sizeof(struct bt_device));
    xSemaphoreGive(iris_state.mutex);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "paired_count", (uint8_t)count);
    for (int i = 0; i < count; i++) {
        char key[24];
        snprintf(key, sizeof(key), "paired_%d_name", i);
        nvs_set_str(h, key, snap[i].name);
        snprintf(key, sizeof(key), "paired_%d_addr", i);
        nvs_set_str(h, key, snap[i].addr_str);
        snprintf(key, sizeof(key), "paired_%d_type", i);
        nvs_set_u8(h, key, (uint8_t)snap[i].type);
    }
    nvs_commit(h);
    nvs_close(h);
}

static const ble_uuid128_t badge_svc_uuid = BLE_UUID128_INIT(
    0xF0, 0xDE, 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12,
    0xF0, 0xDE, 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12
);
static const ble_uuid128_t badge_msg_chr_uuid = BLE_UUID128_INIT(
    0xF1, 0xDE, 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12,
    0xF0, 0xDE, 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12
);
static uint16_t badge_msg_chr_handle;

static void iris_do_scan(void);
static void iris_do_connect(bt_command_message_t *cmd);
static void iris_do_disconnect(bt_command_message_t *cmd);
static void iris_do_send_message(bt_command_message_t *cmd);

static void iris(void *ignored) {
    ESP_LOGW("IRIS", "Starting");
    bt_command_message_t *cmd;
    while (1) {
        if (xQueueReceive(iris_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            switch (cmd->command) {
                case BT_COMMAND_SCAN:         iris_do_scan(); break;
                case BT_COMMAND_CONNECT:      iris_do_connect(cmd); break;
                case BT_COMMAND_DISCONNECT:   iris_do_disconnect(cmd); break;
                case BT_COMMAND_SEND_MESSAGE: iris_do_send_message(cmd); break;
                default: ESP_LOGW("IRIS", "Unknown command %d", cmd->command);
            }
            if (cmd->caller) {
                if (eTaskGetState(cmd->caller) != eDeleted) {
                    xTaskNotifyIndexed(cmd->caller, 0, 0, eSetValueWithOverwrite);
                }
            }
            free(cmd);
        }
    }
}

static void send_command_async(bt_command_t command, void *arg) {
    bt_command_message_t *c = calloc(1, sizeof(bt_command_message_t));
    c->caller  = NULL;
    c->command = command;
    c->arg     = arg;
    xQueueSend(iris_queue, &c, portMAX_DELAY);
}

static void send_command_sync(bt_command_t command, void *arg) {
    bt_command_message_t *c = calloc(1, sizeof(bt_command_message_t));
    c->caller  = xTaskGetCurrentTaskHandle();
    c->command = command;
    c->arg     = arg;
    xQueueSend(iris_queue, &c, portMAX_DELAY);
    ulTaskNotifyTakeIndexed(0, pdTRUE, portMAX_DELAY);
}

/* Stubs for iris_do_scan/connect/disconnect — filled in by Task 8 */
static void iris_do_scan(void) {}
static void iris_do_connect(bt_command_message_t *cmd) { (void)cmd; }
static void iris_do_disconnect(bt_command_message_t *cmd) { (void)cmd; }

static void iris_do_send_message(bt_command_message_t *cmd) {
    if (!cmd->arg) return;
    typedef struct { bt_device_handle dev; size_t len; char msg[BT_MSG_MAX_LEN]; } send_arg_t;
    send_arg_t *a = cmd->arg;
    if (a->dev->conn_handle == BLE_HS_CONN_HANDLE_NONE) { free(a); return; }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(a->msg, a->len);
    if (om) {
        ble_gattc_notify_custom(a->dev->conn_handle, badge_msg_chr_handle, om);
    }
    free(a);
}

void bt_set_enabled(bool enabled) {
    xSemaphoreTake(iris_state.mutex, portMAX_DELAY);
    if (enabled && iris_state.status == BT_DISABLED) {
        iris_state.status = BT_ENABLED;
        xSemaphoreGive(iris_state.mutex);
    } else if (!enabled && iris_state.status == BT_ENABLED) {
        iris_state.status = BT_DISABLED;
        xSemaphoreGive(iris_state.mutex);
        ble_gap_adv_stop();
        ble_gap_disc_cancel();
    } else {
        xSemaphoreGive(iris_state.mutex);
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "enabled", enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

bt_status_t bt_get_status(void) {
    return iris_state.status;
}

void bt_set_own_name(const char *name) {
    if (!name || !name[0]) return;
    char name_copy[sizeof(iris_state.own_name)];
    xSemaphoreTake(iris_state.mutex, portMAX_DELAY);
    strncpy(iris_state.own_name, name, sizeof(iris_state.own_name) - 1);
    iris_state.own_name[sizeof(iris_state.own_name) - 1] = '\0';
    memcpy(name_copy, iris_state.own_name, sizeof(name_copy));
    xSemaphoreGive(iris_state.mutex);
    ble_svc_gap_device_name_set(name_copy);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "own_name", name_copy);
        nvs_commit(h);
        nvs_close(h);
    }
}

const char *bt_get_own_name(void) {
    return iris_state.own_name;
}

const char *bt_device_get_name(bt_device_handle dev) { return dev->name; }
const char *bt_device_get_addr(bt_device_handle dev) { return dev->addr_str; }
bt_device_type_t bt_device_get_type(bt_device_handle dev) { return dev->type; }

void bt_device_free(bt_device_handle dev) {
    free(dev);
}

int bt_scan_get_num_results(void) {
    xSemaphoreTake(iris_state.mutex, portMAX_DELAY);
    int n = iris_state.num_scan_results;
    xSemaphoreGive(iris_state.mutex);
    return n;
}

bt_device_handle bt_scan_get_result(int index) {
    bt_device_handle ret = NULL;
    xSemaphoreTake(iris_state.mutex, portMAX_DELAY);
    if (index >= 0 && index < iris_state.num_scan_results) {
        ret = malloc(sizeof(struct bt_device));
        if (ret) memcpy(ret, &iris_state.scan_results[index], sizeof(struct bt_device));
    }
    xSemaphoreGive(iris_state.mutex);
    return ret;
}

void bt_scan_start(void) {
    if (iris_state.status == BT_DISABLED) return;
    send_command_async(BT_COMMAND_SCAN, NULL);
}

bt_connection_status_t bt_get_connection_status(bt_device_handle dev) {
    return dev->conn_status;
}

int bt_get_num_paired(void) {
    xSemaphoreTake(iris_state.mutex, portMAX_DELAY);
    int n = iris_state.num_paired;
    xSemaphoreGive(iris_state.mutex);
    return n;
}

bt_device_handle bt_get_paired(int index) {
    bt_device_handle ret = NULL;
    xSemaphoreTake(iris_state.mutex, portMAX_DELAY);
    if (index >= 0 && index < iris_state.num_paired) {
        ret = malloc(sizeof(struct bt_device));
        if (ret) memcpy(ret, &iris_state.paired[index], sizeof(struct bt_device));
    }
    xSemaphoreGive(iris_state.mutex);
    return ret;
}

void bt_forget_device(bt_device_handle dev) {
    xSemaphoreTake(iris_state.mutex, portMAX_DELAY);
    for (int i = 0; i < iris_state.num_paired; i++) {
        if (strcmp(iris_state.paired[i].addr_str, dev->addr_str) == 0) {
            memmove(
                &iris_state.paired[i],
                &iris_state.paired[i + 1],
                (iris_state.num_paired - i - 1) * sizeof(struct bt_device)
            );
            iris_state.num_paired--;
            break;
        }
    }
    xSemaphoreGive(iris_state.mutex);
    nvs_save_paired();
    free(dev);
}

bt_connection_status_t bt_connect(bt_device_handle dev) {
    if (!dev || iris_state.status == BT_DISABLED) return BT_DISCONNECTED;
    send_command_sync(BT_COMMAND_CONNECT, dev);
    return dev->conn_status;
}

bt_connection_status_t bt_disconnect(bt_device_handle dev) {
    if (!dev) return BT_DISCONNECTED;
    send_command_sync(BT_COMMAND_DISCONNECT, dev);
    return dev->conn_status;
}

bool bt_send_message(bt_device_handle dev, const char *msg, size_t len) {
    if (!dev || !msg || len == 0 || iris_state.status == BT_DISABLED) return false;
    if (len > BT_MSG_MAX_LEN) len = BT_MSG_MAX_LEN;
    /* arg ownership passed to iris task — allocated here, freed in iris_do_send_message */
    typedef struct { bt_device_handle dev; size_t len; char msg[BT_MSG_MAX_LEN]; } send_arg_t;
    send_arg_t *a = malloc(sizeof(send_arg_t));
    if (!a) return false;
    a->dev = dev;
    a->len = len;
    memcpy(a->msg, msg, len);
    send_command_async(BT_COMMAND_SEND_MESSAGE, a);
    return true;
}

static int bt_dev_open(void *dev, path_t *path, int flags, mode_t mode) {
    return (path->directory || path->filename) ? -1 : 0;
}
static int bt_dev_close(void *dev, int fd) { return fd == 0 ? 0 : -1; }
static ssize_t bt_dev_write(void *dev, int fd, void const *buf, size_t n) { (void)dev; (void)fd; (void)buf; (void)n; return 0; }
static ssize_t bt_dev_read(void *dev, int fd, void *buf, size_t n) { (void)dev; (void)fd; (void)buf; (void)n; return 0; }
static ssize_t bt_dev_lseek(void *dev, int fd, off_t off, int w) { (void)dev; (void)fd; (void)off; (void)w; return (off_t)-1; }

static int badge_msg_chr_access_cb(
    uint16_t conn_handle, uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > BT_MSG_MAX_LEN) len = BT_MSG_MAX_LEN;
    uint8_t buf[BT_MSG_MAX_LEN];
    ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
    xSemaphoreTake(iris_state.mutex, portMAX_DELAY);
    for (int i = 0; i < iris_state.num_paired; i++) {
        if (iris_state.paired[i].conn_handle == conn_handle) {
            xSemaphoreGive(iris_state.mutex);
            if (bt_badge_profile.on_data)
                bt_badge_profile.on_data(&iris_state.paired[i], buf, len);
            return 0;
        }
    }
    xSemaphoreGive(iris_state.mutex);
    return 0;
}

static const struct ble_gatt_svc_def badge_gatt_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &badge_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid       = &badge_msg_chr_uuid.u,
                .access_cb  = badge_msg_chr_access_cb,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &badge_msg_chr_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

void badge_profile_register_services(void) {
    ble_gatts_count_cfg(badge_gatt_svcs);
    ble_gatts_add_svcs(badge_gatt_svcs);
}

static void badge_profile_start_advertising(void) {
    struct ble_hs_adv_fields fields = {0};
    fields.flags                = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name                 = (uint8_t *)iris_state.own_name;
    fields.name_len             = strlen(iris_state.own_name);
    fields.name_is_complete     = 1;
    fields.uuids128             = &badge_svc_uuid;
    fields.num_uuids128         = 1;
    fields.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                      &adv_params, NULL, NULL);
}

static void on_ble_sync(void) {
    ble_hs_id_infer_auto(0, &(uint8_t){0});
    ble_svc_gap_device_name_set(iris_state.own_name);
    if (iris_state.status == BT_ENABLED)
        badge_profile_start_advertising();
}

static void nimble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bt_profile_t bt_hid_profile;
bt_profile_t bt_badge_profile;

device_t *bluetooth_create(void) {
    ESP_LOGI(TAG, "Initializing");

    bluetooth_device_t *dev  = calloc(1, sizeof(bluetooth_device_t));
    device_t           *base = (device_t *)dev;
    base->type   = DEVICE_TYPE_BLUETOOTH;
    base->_open  = bt_dev_open;
    base->_close = bt_dev_close;
    base->_write = bt_dev_write;
    base->_read  = bt_dev_read;
    base->_lseek = bt_dev_lseek;

    memset(&iris_state, 0, sizeof(iris_state));
    iris_state.mutex       = xSemaphoreCreateMutex();
    iris_state.event_group = xEventGroupCreate();
    iris_state.status      = BT_DISABLED;

    /* Derive default name from BT MAC */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(iris_state.own_name, sizeof(iris_state.own_name),
             "WHY2025-%02X%02X", mac[4], mac[5]);

    nvs_load();

    uint8_t enabled_flag = 1;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "enabled", &enabled_flag);
        nvs_close(h);
    }

    memset(&bt_hid_profile, 0, sizeof(bt_hid_profile));
    memset(&bt_badge_profile, 0, sizeof(bt_badge_profile));

    iris_queue = xQueueCreate(8, sizeof(bt_command_message_t *));
    create_kernel_task(iris, "Iris", 4096, NULL, 5, &iris_handle, 0);

    nimble_port_init();
    ble_hs_cfg.sync_cb = on_ble_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_store_config_init();

    badge_profile_register_services();

    nimble_port_freertos_init(nimble_host_task);

    if (enabled_flag) {
        iris_state.status = BT_ENABLED;
    }

    return base;
}

