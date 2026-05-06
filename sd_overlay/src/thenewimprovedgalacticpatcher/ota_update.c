#define _GNU_SOURCE // for strverscmp()
#include "ota_update.h"

#include "thirdparty/cJSON.h"

#include <stdio.h>
#include <stdlib.h>

#include <badgevms/ota.h>
#include <ctype.h>
#include <string.h>

#define FIRMWARE_PROJECT         "why2025_firmware"
#define HTTP_USERAGENT           "BadgeVMS-ota/1.0"
#define BADGEHUB_BASE_URL        "https://badge.why2025.org/api/v3"
#define BADGEHUB_PROJECT_DETAIL  BADGEHUB_BASE_URL "/projects/%s"
#define BADGEHUB_LATEST_REVISION BADGEHUB_BASE_URL "/project-latest-revisions/%s"
#define BADGEHUB_REVISION_FILE   BADGEHUB_BASE_URL "/projects/%s/rev%i/files/%s"
#define BADGEHUB_REVISION        BADGEHUB_BASE_URL "/projects/%s/rev%i"
#define BADGEHUB_PING            BADGEHUB_BASE_URL "/ping?id=%s-v1&mac=%s"
#define BADGEHUB_DEFAULT_APPS    BADGEHUB_BASE_URL "/project-summaries?category=Default"
#define BADGEHUB_FIRMWARE_URL    BADGEHUB_BASE_URL "/projects/" FIRMWARE_PROJECT "/rev%i/files/badgevms.bin"

char *source_to_name(application_source_t s) {
    switch (s) {
        case APPLICATION_SOURCE_BADGEHUB: return "Badgehub";
        default: return "Unknown";
    }
}

static size_t mem_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    debug_printf("mem_cb(%p, %zu, %zu, %p)\n", contents, size, nmemb, userp);

    size_t       realsize = size * nmemb;
    http_data_t *mem      = (http_data_t *)userp;

    mem->memory = realloc(mem->memory, mem->size + realsize + 1);
    if (mem->memory == NULL) {
        printf("Malloc failed\n");
        return 0;
    }

    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size              += realsize;
    mem->memory[mem->size]  = 0;

    return realsize;
}

static size_t file_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    debug_printf("file_cb(%p, %zu, %zu, %p)\n", contents, size, nmemb, userp);

    size_t       realsize = size * nmemb;
    http_file_t *file     = (http_file_t *)userp;
    FILE        *f        = file->fp;

    size_t s = fwrite(contents, 1, size * nmemb, f);
    if (s != size * nmemb) {
        printf("Failure writing to file\n");
        return 0;
    }

    file->size += realsize;
    return realsize;
}

static size_t firmware_cb(void *contents, size_t size, size_t nmemb, ota_handle_t handle) {
    debug_printf("firmware_cb(%p, %zu, %zu, %p)\n", contents, size, nmemb, handle);
    bool   err;
    size_t realsize = size * nmemb;

    char *buffer = (char *)calloc(realsize + 1, sizeof(char));
    memcpy(buffer, contents, realsize);

    err = ota_write(handle, buffer, realsize);
    if (err == false) {
        printf("ota_write() failed\n");
        return 0;
    }

    free(buffer);
    return realsize;
}

bool do_http(char const *url, http_data_t *response_data, http_file_t *http_file) {
    debug_printf("do_http(%s, %p, %p)\n", url, response_data, http_file);

    bool ret = false;

    if (!response_data && !http_file) {
        printf("No response data pointer provided\n");
        return false;
    }

    if (!url) {
        printf("No URL provided\n");
        return false;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        printf("Failed to allocate curl\n");
        return false;
    }

    if (response_data) {
        memset(response_data, 0, sizeof(http_data_t));
    }

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (response_data) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)response_data);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)http_file);
    }

    curl_easy_setopt(curl, CURLOPT_USERAGENT, HTTP_USERAGENT);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        char const *error_string = curl_easy_strerror(res);
        if (error_string) {
            printf("do_http(%s) curl_easy_perform() failed: %s\n", url, curl_easy_strerror(res));
        } else {
            printf("do_http(%s) curl_easy_perform() failed: %u\n", url, res);
        }
        goto out;
    }

    long response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    if (response_data) {
        debug_printf("do_http(%s) response code: %ld, bytes received: %zu\n", url, response_code, response_data->size);
        response_data->memory                      = realloc(response_data->memory, response_data->size + 1);
        response_data->memory[response_data->size] = 0;
    } else {
        debug_printf("do_http(%s) response code: %ld, bytes received: %zu\n", url, response_code, http_file->size);
    }

    if (response_code < 200 || response_code > 299) {
        if (response_data && response_data->size) {
            printf("do_http(%s) error: server response: '%s'\n", url, (char *)response_data->memory);
        } else {
            printf("do_http(%s) error: server response: <no response>\n", url);
        }
        goto out;
    }

    ret = true;
out:
    curl_easy_cleanup(curl);
    debug_printf("do_http(%s) returned %i\n", url, ret);
    return ret;
}

void badgehub_ping() {
    char       *url = NULL;
    http_data_t response_data;
    char const *mac = get_mac_address();

    asprintf(&url, BADGEHUB_PING, mac, mac);
    if (!do_http(url, &response_data, NULL)) {
        printf("Failed to ping badgehub\n");
    }

    free(response_data.memory);
    free(url);
}

int get_project_latest_revision(char const *unique_identifier) {
    if (!unique_identifier) {
        return false;
    }

    int         ret = -1;
    char       *url = NULL;
    http_data_t response_data;

    debug_printf("Checking latest revision for %s\n", unique_identifier);
    asprintf(&url, BADGEHUB_LATEST_REVISION, unique_identifier);

    if (!do_http(url, &response_data, NULL)) {
        printf("Failed to retrieve project revision\n");
        goto out;
    }

    ret = atoi(response_data.memory);
out:
    free(url);
    free(response_data.memory);
    return ret;
}

bool get_project_latest_version(char const *unique_identifier, int revision, char **version) {
    if (!unique_identifier || !version) {
        return false;
    }

    bool        ret = false;
    char       *url = NULL;
    http_data_t response_data;

    debug_printf("Checking latest version for %s\n", unique_identifier);
    asprintf(&url, BADGEHUB_REVISION_FILE, unique_identifier, revision, "version.txt");
    if (!do_http(url, &response_data, NULL)) {
        printf("Failed to retrieve project version.txt\n");
        goto out;
    }

    *version = strdup(response_data.memory);
    // Strip any whitespace and such
    int k    = 0;
    for (int i = 0; i < strlen(response_data.memory); ++i) {
        if (isgraph(response_data.memory[i])) {
            (*version)[k++] = response_data.memory[i];
        }
    }

    (*version)[k] = 0;
    ret           = true;

out:
    free(url);
    free(response_data.memory);
    return ret;
}

bool update_application_file(application_t *app, char const *relative_file_name, char const *file_url) {
    bool  ret                = false;
    char *absolute_file_name = NULL;
    char *tmpfile            = NULL;
    FILE *f                  = NULL;

    absolute_file_name = application_create_file_string(app, relative_file_name);
    if (!absolute_file_name) {
        printf("Illegal file name %s\n", relative_file_name);
        goto out;
    }

    asprintf(&tmpfile, "%s.inst", absolute_file_name);
    if (!tmpfile) {
        printf("Unable to allocate tmpfilename\n");
        goto out;
    }

    f = fopen(tmpfile, "w");
    if (!f) {
        printf("Unable to open tmpfile %s\n", tmpfile);
        goto out;
    }

    http_file_t file_op;
    file_op.size = 0;
    file_op.fp   = f;

    if (!do_http(file_url, NULL, &file_op)) {
        printf("Unable to write save tmpfile %s\n", tmpfile);
        goto out;
    }

    fclose(f);
    f = NULL;

    remove(absolute_file_name);

    if (rename(tmpfile, absolute_file_name)) {
        printf("Unable to move tmpfile to final %s -> %s\n", tmpfile, absolute_file_name);
        goto out;
    }

    ret = true;

out:
    free(tmpfile);
    free(absolute_file_name);
    if (f) {
        fclose(f);
    }
    return ret;
}

bool update_application(application_t *app, char const *version) {
    FILE       *file             = NULL;
    char       *application_data = NULL;
    char       *url              = NULL;
    http_data_t response_data;

    cJSON *json             = NULL;
    cJSON *files_array      = NULL;
    cJSON *file_item        = NULL;
    cJSON *app_version      = NULL;
    cJSON *app_metadata     = NULL;
    cJSON *app_application  = NULL;
    cJSON *name_field       = NULL;
    cJSON *executable_field = NULL;
    char  *name             = NULL;
    char  *executable       = NULL;
    long   file_size        = 0;
    bool   result           = false;

    int revision = get_project_latest_revision(app->unique_identifier);
    if (revision < 0) {
        printf("Failed to get project revision\n");
        goto out;
    }

    asprintf(&url, BADGEHUB_REVISION, app->unique_identifier, revision);
    if (!do_http(url, &response_data, NULL)) {
        printf("Failed to read project revision data\n");
        goto out;
    }

    json = cJSON_Parse(response_data.memory);
    if (!json) {
        char const *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            printf("Error: JSON parse error before: %s\n", error_ptr);
        }
        goto out;
    }

    app_version = cJSON_GetObjectItemCaseSensitive(json, "version");
    if (!app_version) {
        printf("Error 'version' object not found in JSON\n");
        goto out;
    }

    app_metadata = cJSON_GetObjectItemCaseSensitive(app_version, "app_metadata");
    if (app_metadata && cJSON_IsObject(app_metadata)) {
        name_field = cJSON_GetObjectItemCaseSensitive(app_metadata, "name");
        if (name_field && cJSON_IsString(name_field)) {
            name = name_field->valuestring;
        }

        app_application = cJSON_GetObjectItemCaseSensitive(app_metadata, "application");
        debug_printf("Found 'application' in 'app_metadata'\n");
        if (app_application && cJSON_IsArray(app_application)) {
            debug_printf("Found 'application' in 'app_metadata' is array\n");
            cJSON *application_item = NULL;
            cJSON_ArrayForEach(application_item, app_application) {
                debug_printf("ForEach 'application'\n");
                if (cJSON_IsObject(application_item)) {
                    executable_field = cJSON_GetObjectItemCaseSensitive(application_item, "executable");
                    debug_printf("Found 'executable'\n");
                    if (executable_field && cJSON_IsString(executable_field)) {
                        executable = executable_field->valuestring;
                        debug_printf("Executable field %s\n", executable);
                    }
                }
            }
        }
    }

    files_array = cJSON_GetObjectItemCaseSensitive(app_version, "files");
    if (!files_array) {
        printf("Error: 'files' array not found in JSON\n");
        goto out;
    }

    if (!cJSON_IsArray(files_array)) {
        printf("Error: 'files' is not an array\n");
        goto out;
    }

    result = true;
    cJSON_ArrayForEach(file_item, files_array) {
        if (!cJSON_IsObject(file_item)) {
            printf("Warning: Skipping non-object item in files array\n");
            continue;
        }

        cJSON *file_url  = cJSON_GetObjectItemCaseSensitive(file_item, "url");
        cJSON *full_path = cJSON_GetObjectItemCaseSensitive(file_item, "full_path");

        if (!file_url || !cJSON_IsString(file_url)) {
            printf("Warning: Missing or invalid 'url' field\n");
            result = false;
            continue;
        }

        if (!full_path || !cJSON_IsString(full_path)) {
            printf("Warning: Missing or invalid 'full_path' field\n");
            result = false;
            continue;
        }

        if (!update_application_file(app, full_path->valuestring, file_url->valuestring)) {
            printf("Unable to update file %s\n", full_path->valuestring);
            result = false;
        }
    }

    if (result) {
        application_set_version(app, version);
        application_set_metadata(app, "metadata.json");

        if (name) {
            application_set_name(app, name);
        }

        if (executable) {
            application_set_binary_path(app, executable);
        }
    }
out:
    cJSON_Delete(json);
    free(url);
    free(response_data.memory);
    return result;
}

size_t list_default_applications(char ***app_slugs) {
    if (!app_slugs) {
        return 0;
    }
    size_t num      = 0;
    cJSON *json     = NULL;
    cJSON *app_item = NULL;
    cJSON *app_slug = NULL;

    http_data_t response_data;

    if (!do_http(BADGEHUB_DEFAULT_APPS, &response_data, NULL)) {
        printf("Failed to read default application list\n");
        goto out;
    }

    json = cJSON_Parse(response_data.memory);
    if (!json) {
        char const *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            printf("Error: JSON parse error before: %s\n", error_ptr);
        }
        goto out;
    }

    if (!cJSON_IsArray(json)) {
        printf("Error: root is not an array\n");
        goto out;
    }

    cJSON_ArrayForEach(app_item, json) {
        if (!cJSON_IsObject(app_item)) {
            printf("Warning: Skipping non-object item in applications array\n");
            continue;
        }

        cJSON *app_slug = cJSON_GetObjectItemCaseSensitive(app_item, "slug");

        if (!app_slug || !cJSON_IsString(app_slug)) {
            printf("Warning: Missing or invalid 'slug' field\n");
            continue;
        }

        ++num;
        *app_slugs            = realloc(*app_slugs, sizeof(char *) * num);
        (*app_slugs)[num - 1] = strdup(app_slug->valuestring);
    }

out:
    cJSON_Delete(json);
    return num;
}

bool check_for_updates(application_t *app, char **version) {
    if (!app) {
        return false;
    }

    bool ret = false;

    printf("Checking for updates for %s\n", app->unique_identifier);
    int revision = get_project_latest_revision(app->unique_identifier);
    if (revision < 0) {
        printf("Failed to get project revision\n");
        goto out;
    }

    if (!get_project_latest_version(app->unique_identifier, revision, version)) {
        printf("Failed to get project version\n");
        goto out;
    }

    int vers = strverscmp(app->version, *version);
    if (vers < 0) {
        ret = true;
    }
out:
    return ret;
}

bool do_firmware_http(char const *url, ota_handle_t ota_session) {
    debug_printf("do_firmware_http(%s, %p)\n", url, ota_session);

    bool ret = false;

    if (!url) {
        printf("No URL provided\n");
        return false;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        printf("Failed to allocate curl\n");
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 1024);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, firmware_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)ota_session);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, HTTP_USERAGENT);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        char const *error_string = curl_easy_strerror(res);
        if (error_string) {
            printf("do_firmware_http(%s) curl_easy_perform() failed: %s\n", url, curl_easy_strerror(res));
        } else {
            printf("do_firmware_http(%s) curl_easy_perform() failed: %u\n", url, res);
        }
        goto out;
    }

    long response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    debug_printf("do_firmware_http(%s) response code: %ld\n", url, response_code);

    if (response_code < 200 || response_code > 299) {
        printf("do_firmware_http(%s) error: server response: <no response>\n", url);
        goto out;
    }

    ret = true;
out:
    curl_easy_cleanup(curl);
    debug_printf("do_firmware_http(%s) returned %i\n", url, ret);
    return ret;
}

bool update_firmware() {
    bool  ret = false;
    char *url = NULL;

    ota_handle_t ota_session = ota_session_open();
    if (!ota_session) {
        printf("Failed to open OTA session\n");
        goto out;
    }

    char const *firmware_name = FIRMWARE_PROJECT;
    printf("getting revision for %s\n", firmware_name);
    int revision = get_project_latest_revision(firmware_name);
    if (revision < 0) {
        printf("Failed to get firmware revision\n");
        goto out;
    }

    asprintf(&url, BADGEHUB_FIRMWARE_URL, revision);
    if (!do_firmware_http(url, ota_session)) {
        printf("Failed to update firmware\n");
        goto out;
    }

    ota_session_commit(ota_session);
    debug_printf("Firmware updated!\n");
out:
    free(url);
    return ret;
}

bool check_for_firmware_updates(char **version) {
    if (!version) {
        return false;
    }
    bool  ret          = false;
    char *running      = NULL;
    char *running_back = NULL;

    char const *firmware_name = FIRMWARE_PROJECT;
    printf("Checking for updates for %s\n", firmware_name);
    int revision = get_project_latest_revision(firmware_name);
    if (revision < 0) {
        printf("Failed to get firmware revision\n");
        goto out;
    }

    if (!get_project_latest_version(firmware_name, revision, version)) {
        printf("Failed to get firmware version\n");
        goto out;
    }

    // Work around a bug in the v1 OTA apis
    running      = calloc(1, 32);
    // Old versions just overwrite running[0] with a single byte
    // new versions give us back an stdup()'d string
    running_back = running;

    ota_get_running_version(&running);

    int vers = strverscmp(running, *version);
    if (vers < 0) {
        ret = true;
    }
out:
    if (running_back != running) {
        free(running);
    }
    free(running_back);
    return ret;
}
