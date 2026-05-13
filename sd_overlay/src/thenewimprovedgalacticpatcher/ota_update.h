#include <stdbool.h>

#include <badgevms/application.h>
#include <badgevms/misc_funcs.h>
#include <badgevms/process.h>
#include <curl/curl.h>

extern bool debug;

#define debug_printf(fmt, ...)                                                                                         \
    do {                                                                                                               \
        if (debug) {                                                                                                   \
            printf(fmt, ##__VA_ARGS__);                                                                                \
        }                                                                                                              \
    } while (0)

typedef struct {
    application_t *app;
    char          *name;
    char          *version;
    char          *description;
    bool           is_firmware;
} update_item_t;

typedef struct http_data {
    char  *memory;
    size_t size;
} http_data_t;

typedef struct http_file {
    FILE  *fp;
    size_t size;
} http_file_t;

bool   check_for_updates(application_t *app, char **version);
bool   check_for_firmware_updates(char **version);
bool   update_firmware();
bool   do_http(char const *url, http_data_t *response_data, http_file_t *http_file);
bool   get_project_latest_version(char const *unique_identifier, int revision, char **version);
bool   update_application(application_t *app, char const *version);
bool   update_application_file(application_t *app, char const *relative_file_name, char const *file_url);
size_t list_default_applications(char ***app_slugs);
char  *source_to_name(application_source_t s);
void   badgehub_ping();

bool   run_update_window(update_item_t *updates, size_t num);
bool   run_update_window_with_check(void);
size_t perform_update_check(update_item_t **updates);
