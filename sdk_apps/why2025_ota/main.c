#define _GNU_SOURCE // for strverscmp()
#include "ota_update.h"
#include "thirdparty/cJSON.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <badgevms/wifi.h>
#include <string.h>

bool debug  = false;
bool window = true;

size_t perform_update_check(update_item_t **updates, check_status_cb_t cb, void *userdata, bool force_reinstall) {
    size_t num_updates = 0;

    if (cb) cb("Reaching update server...", userdata);
    if (cb) cb("Fetching default app list...", userdata);

    char **default_apps     = NULL;
    size_t num_default_apps = list_default_applications(&default_apps);

    if (num_default_apps) {
        debug_printf("Default apps: \n");
        for (int i = 0; i < num_default_apps; ++i) {
            application_t *installed = application_get(default_apps[i]);
            if (installed) {
                printf(" %s already installed\n", default_apps[i]);
                application_free(installed);
                continue;
            }

            printf(" %s not yet installed\n", default_apps[i]);
            application_t *new_app =
                application_create(default_apps[i], default_apps[i], NULL, "-1", NULL, APPLICATION_SOURCE_BADGEHUB);
            if (!new_app) {
                printf("Failed to create an application for %s\n", default_apps[i]);
                continue;
            }
            application_create_file_string(new_app, "dummy");
            application_free(new_app);
        }
    }

    application_t          *app;
    application_list_handle app_list = application_list(&app);

    debug_printf("Currently installed applications: \n");
    while (app) {
        debug_printf("Name: %s\n", app->name);
        debug_printf("  Version: %s\n", app->version);
        debug_printf("  Source: %s\n", source_to_name(app->source));
        if (app->source == APPLICATION_SOURCE_BADGEHUB) {
            char cb_msg[128];
            snprintf(cb_msg, sizeof(cb_msg), "Checking for updates: %s", app->unique_identifier);
            if (cb) cb(cb_msg, userdata);

            char *version    = NULL;
            bool  has_update = check_for_updates(app, &version);
            if (has_update || force_reinstall) {
                ++num_updates;
                *updates                                = realloc(*updates, sizeof(update_item_t) * num_updates);
                (*updates)[num_updates - 1].app            = app;
                (*updates)[num_updates - 1].name           = strdup(app->name);
                (*updates)[num_updates - 1].version        = strdup(version ? version : app->version);
                (*updates)[num_updates - 1].description    = NULL;
                (*updates)[num_updates - 1].is_firmware    = false;
                (*updates)[num_updates - 1].is_new_install = (strcmp(app->version, "-1") == 0);
                (*updates)[num_updates - 1].selected       = true;
                debug_printf(
                    "New version available for %s (%s < %s)\n",
                    app->name,
                    app->version,
                    (*updates)[num_updates - 1].version
                );
            } else {
                debug_printf("No updates available for  %s (%s >= %s)\n", app->name, app->version, version);
            }
            free(version);
        }
        app = application_list_get_next(app_list);
    }

    if (cb) cb("Checking firmware update...", userdata);

    char *firmware_version = NULL;
    if (check_for_firmware_updates(&firmware_version)) {
        debug_printf("New firmware version available!");
        ++num_updates;
        *updates                                = realloc(*updates, sizeof(update_item_t) * num_updates);
        (*updates)[num_updates - 1].app            = NULL;
        (*updates)[num_updates - 1].name           = strdup("BadgeVMS Firmware");
        (*updates)[num_updates - 1].version        = strdup(firmware_version);
        (*updates)[num_updates - 1].description    = strdup("Main badge firmware");
        (*updates)[num_updates - 1].is_firmware    = true;
        (*updates)[num_updates - 1].is_new_install = false;
        (*updates)[num_updates - 1].selected       = true;
    }

    // application_list_close(app_list);
    return num_updates;
}

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--debug") == 0) {
            debug = true;
        }
        if (strcmp(argv[i], "--background") == 0) {
            window = false;
        }
    }

    debug_printf("BadgeVMS OTA starting...\n");
    debug_printf("Connecting to wifi\n");


    debug_printf("Initializing curl\n");
    curl_global_init(0);

    if (window) {
        debug_printf("Showing update check window\n");
        run_update_window_with_check();
    } else {
        wifi_connection_status_t status = wifi_connect();
        if (status != WIFI_CONNECTED) {
            printf("Unable to connect to wifi\n");
            return 1;
        }

        debug_printf("Pinging badgehub\n");
        badgehub_ping();

        update_item_t *updates     = NULL;
        size_t         num_updates = perform_update_check(&updates, NULL, NULL, false);

        if (num_updates) {
            printf("Updates available!\n");
            if (get_num_tasks() <= 2) {
                run_update_window(updates, num_updates);
            } else {
                printf("Other tasks running, trying again later\n");
            }
            free(updates);
        } else {
            printf("No updates available\n");
        }
    }

    debug_printf("All done!\n");
    return 0;
}
