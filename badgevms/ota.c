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

#include "badgevms/ota.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "task.h"

#include <stdatomic.h>

#define TAG "why_ota"

struct ota_session_t {
    esp_partition_t const *configured;
    esp_partition_t const *running;
    esp_partition_t const *update_partition;
    esp_ota_handle_t       update_handle;
    atomic_flag            open;
    atomic_bool            error;
};

static ota_session_t session = {
    .configured       = NULL,
    .running          = NULL,
    .update_partition = NULL,
    .update_handle    = 0,
    .open             = ATOMIC_FLAG_INIT,
    .error            = ATOMIC_VAR_INIT(false),
};

ota_handle_t ota_session_open() {
    esp_err_t err;

    if (atomic_flag_test_and_set(&session.open)) {
        return NULL;
    }

    task_record_resource_alloc(RES_OTA, (ota_handle_t)&session);

    session.configured = esp_ota_get_boot_partition();
    session.running    = esp_ota_get_running_partition();

    ESP_LOGW(
        TAG,
        "Configured OTA boot partition at offset 0x%08" PRIx32 ", running from offset 0x%08" PRIx32,
        session.configured->address,
        session.running->address
    );

    session.update_partition = esp_ota_get_next_update_partition(NULL);
    if (session.update_partition == NULL) {
        atomic_flag_clear(&session.open);
        return NULL;
    }

    err = esp_ota_begin(session.update_partition, OTA_WITH_SEQUENTIAL_WRITES, &(session.update_handle));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        ota_session_abort((ota_handle_t)&session);
        return NULL;
    }

    atomic_store(&session.error, false);

    return (ota_handle_t)&session;
}

bool ota_write(ota_handle_t session, void *buffer, int block_size) {
    esp_err_t err;
    if (atomic_load(&session->error)) {
        return false;
    }
    err = esp_ota_write(session->update_handle, (void const *)buffer, block_size);
    if (err != ESP_OK) {
        ota_session_abort(session);
        return false;
    }
    return true;
}

bool ota_session_commit(ota_handle_t session) {
    esp_err_t err;
    if (atomic_load(&session->error)) {
        return false;
    }
    err = esp_ota_end(session->update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
        }
        return false;
    }
    err = esp_ota_set_boot_partition(session->update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        return false;
    }
    task_record_resource_free(RES_OTA, session);
    atomic_flag_clear(&session->open);
    return true;
}

bool ota_session_abort(ota_handle_t session) {
    esp_ota_abort(session->update_handle);
    task_record_resource_free(RES_OTA, session);
    atomic_flag_clear(&session->open);
    atomic_store(&session->error, true);
    return true;
}

/*
version is a pointer of type char[32].
If function returns true then the passed pointer should contain the version of the running app
*/
bool ota_get_running_version(char **version) {
    esp_app_desc_t running_app_info;

    esp_partition_t const *running = esp_ota_get_running_partition();

    if (esp_ota_get_partition_description(running, &running_app_info) != ESP_OK) {
        ESP_LOGE(TAG, "Could not get running partition");
        return false;
    }

    *version = strdup(running_app_info.version);
    return true;
}

/*
version is a pointer of type char[32].
If function returns true then the passed pointer should contain the last invalid ota app
Returns false if there was a problem or no invalid partition found
*/
bool ota_get_invalid_version(char **version) {
    esp_app_desc_t invalid_app_info;

    esp_partition_t const *last_invalid_app = esp_ota_get_last_invalid_partition();

    if (last_invalid_app == NULL) {
        return false;
    }
    if (esp_ota_get_partition_description(last_invalid_app, &invalid_app_info) != ESP_OK) {
        ESP_LOGE(TAG, "Could not get invalid partition info");
        return false;
    }

    *version = strdup(invalid_app_info.version);

    return true;
}

bool validate_ota_partition() {
    esp_err_t err;

    esp_partition_t const *running = esp_ota_get_running_partition();
    ESP_LOGW(
        TAG,
        "Running partition type %d subtype %d (offset 0x%08" PRIx32 ")",
        running->type,
        running->subtype,
        running->address
    );

    esp_ota_img_states_t ota_state;
    err = esp_ota_get_state_partition(running, &ota_state);
    if (err != ESP_OK) {
        return false;
    }

    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "Marking running partition as valid and cancelling rollback");
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            return false;
        }
    }
    return true;
}

void invalidate_ota_partition() {
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

size_t ota_get_firmware_size(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return 0;
    return running->size;
}

bool ota_read_firmware(size_t offset, void *buf, size_t len) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return false;
    return esp_partition_read(running, offset, buf, len) == ESP_OK;
}
