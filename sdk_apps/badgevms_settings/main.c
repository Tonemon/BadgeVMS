#include "font.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <badgevms/wifi.h>
#include <badgevms/application.h>
#include <SDL3/SDL.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"


#define SCREEN_WIDTH  720
#define SCREEN_HEIGHT 720

#define CDE_BG_COLOR      0x9CA0A0
#define CDE_PANEL_COLOR   0xAEB2B2
#define CDE_BORDER_LIGHT  0xFFFFFF
#define CDE_BORDER_DARK   0x636363
#define CDE_TEXT_COLOR    0x000000
#define CDE_SELECTED_BG   0x0078D4
#define CDE_SELECTED_TEXT 0xFFFFFF
#define CDE_BUTTON_COLOR  0xD4D0C8
#define CDE_TITLE_BG      0x808080
#define CDE_INACTIVE_TEXT 0x808080
#define CDE_SUCCESS_COLOR 0x00A000
#define CDE_ERROR_COLOR   0xA00000

typedef enum { SCREEN_MAIN, SCREEN_WIFI, SCREEN_DISPLAY, SCREEN_SYSTEM, SCREEN_ABOUT, SCREEN_REORDER } ScreenState;

#define NAV_STACK_MAX 8

typedef struct {
    ScreenState screen;
    int         selected_item;
    int         scroll_offset;
} NavEntry;

typedef enum {
    DIALOG_NONE,
    DIALOG_NEW_FOLDER,
    DIALOG_RENAME,
    DIALOG_DELETE_CONFIRM,
} reorder_dialog_type_t;

typedef struct {
    bool   is_folder;
    char   uid[64];           /* app uid (is_folder == false) */
    char   display_name[64];  /* app name or folder display name */
    char **folder_apps;       /* ordered uid list (is_folder == true) */
    int    folder_app_count;
    int    folder_app_cap;
} reorder_item_t;

typedef struct {
    char uid[64];
    char name[64];
} app_name_entry_t;

typedef struct {
    char ssid[64];
    int  signal_strength; // 0-100
    bool secured;
    bool connected;
} wifi_network;

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;
    uint16_t     *pixels;

    ScreenState current_screen;
    int         selected_item;
    int         scroll_offset;
    int         total_items;
    int         items_per_page;

    wifi_network *networks;
    int           network_count;
    bool          show_password_dialog;
    bool          show_connecting_dialog;
    int           selected_network;
    char          password_buffer[128];
    int           password_cursor;
    bool          show_password;

    char     status_message[256];
    uint32_t status_color;
    uint32_t status_timer;

    char connecting_ssid[64];
    char connection_status_text[128];
    bool connecting_secured;

    NavEntry nav_stack[NAV_STACK_MAX];
    int      nav_depth;

    /* Reorder screen working state */
    reorder_item_t       *reorder_items;
    int                   reorder_item_count;
    int                   reorder_item_cap;
    int                   reorder_selected;
    int                   reorder_scroll;
    int                   reorder_items_per_page;
    int                   reorder_held;          /* index of held app, -1 = none */
    int                   reorder_held_origin;   /* index before grab (for ESC cancel) */
    bool                  reorder_in_folder;
    int                   reorder_folder_idx;    /* index in reorder_items[] of open folder */
    int                   reorder_folder_sel;
    int                   reorder_folder_scroll;
    reorder_dialog_type_t reorder_dialog_type;
    char                  reorder_dialog_buf[64];
    int                   reorder_dialog_cursor;
    int                   reorder_rename_idx;    /* folder index for rename/delete dialogs */
    /* uid→name lookup built during reorder_init */
    app_name_entry_t     *app_name_table;
    int                   app_name_count;

    /* Launcher config (read from APPS:[badgevms_launcher]config.json) */
    bool  launch_default_app;
    char  launcher_default_uid[64];   /* UID of default app */
    char  launcher_default_name[64];  /* display name, looked up via application_list */

    /* App-chooser dialog (opened from "Default app" settings entry) */
    bool              show_app_chooser;
    app_name_entry_t *chooser_apps;
    int               chooser_app_count;
    int               chooser_selected;
    int               chooser_scroll;
    int               chooser_items_per_page;
} app_context;

static void render_screen(app_context *ctx);
static void app_chooser_close(app_context *ctx);

static inline uint16_t rgb888_to_rgb565_color(uint32_t rgb888) {
    uint8_t r = (rgb888 >> 16) & 0xFF;
    uint8_t g = (rgb888 >> 8) & 0xFF;
    uint8_t b = rgb888 & 0xFF;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static void draw_rect(app_context *ctx, int x, int y, int w, int h, uint32_t color) {
    uint16_t rgb565 = rgb888_to_rgb565_color(color);
    int      x2     = x + w;
    int      y2     = y + h;

    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x2 > SCREEN_WIDTH)
        x2 = SCREEN_WIDTH;
    if (y2 > SCREEN_HEIGHT)
        y2 = SCREEN_HEIGHT;

    for (int py = y; py < y2; py++) {
        uint16_t *row   = &ctx->pixels[py * SCREEN_WIDTH + x];
        int       width = x2 - x;
        for (int i = 0; i < width; i++) {
            row[i] = rgb565;
        }
    }
}

static void draw_char(app_context *ctx, int x, int y, char c, uint32_t color) {
    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR)
        return;

    int             char_index = c - FONT_FIRST_CHAR;
    uint16_t const *char_data  = pixel_font[char_index];
    uint16_t        rgb565     = rgb888_to_rgb565_color(color);

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint16_t row_data = char_data[row];
        int      py       = y + row;

        if (py < 0 || py >= SCREEN_HEIGHT)
            continue;

        for (int col = 0; col < FONT_WIDTH; col++) {
            if (row_data & (0x800 >> col)) {
                int px = x + col;
                if (px >= 0 && px < SCREEN_WIDTH) {
                    ctx->pixels[py * SCREEN_WIDTH + px] = rgb565;
                }
            }
        }
    }
}

static void draw_text(app_context *ctx, int x, int y, char const *text, uint32_t color) {
    int current_x = x;
    while (*text) {
        draw_char(ctx, current_x, y, *text, color);
        current_x += FONT_WIDTH;
        text++;
    }
}

static void draw_text_bold(app_context *ctx, int x, int y, char const *text, uint32_t color) {
    draw_text(ctx, x, y, text, color);
    draw_text(ctx, x + 1, y, text, color);
}

static int get_text_width(char const *text) {
    return strlen(text) * FONT_WIDTH;
}

static void draw_text_centered(app_context *ctx, int x, int y, int width, char const *text, uint32_t color) {
    int text_w = get_text_width(text);
    int text_x = x + (width - text_w) / 2;
    draw_text(ctx, text_x, y, text, color);
}

static void draw_3d_border(app_context *ctx, int x, int y, int w, int h, int inset) {
    uint32_t light_color = inset ? CDE_BORDER_DARK : CDE_BORDER_LIGHT;
    uint32_t dark_color  = inset ? CDE_BORDER_LIGHT : CDE_BORDER_DARK;

    draw_rect(ctx, x, y, w, 2, light_color);
    draw_rect(ctx, x, y, 2, h, light_color);

    draw_rect(ctx, x, y + h - 2, w, 2, dark_color);
    draw_rect(ctx, x + w - 2, y, 2, h, dark_color);
}

static void draw_button(app_context *ctx, int x, int y, int w, int h, char const *text, int pressed) {
    draw_rect(ctx, x, y, w, h, CDE_BUTTON_COLOR);
    draw_3d_border(ctx, x, y, w, h, pressed);

    int text_y = y + (h - FONT_HEIGHT) / 2;
    if (pressed) {
        text_y += 1;
    }
    draw_text_centered(ctx, x, text_y, w, text, CDE_TEXT_COLOR);
}

static void draw_signal_strength(app_context *ctx, int x, int y, int strength) {
    int bar_width   = 4;
    int bar_spacing = 2;
    int max_height  = 20;

    for (int i = 0; i < 4; i++) {
        int bar_x      = x + i * (bar_width + bar_spacing);
        int bar_height = (max_height * (i + 1)) / 4;
        int bar_y      = y + max_height - bar_height;

        uint32_t color = CDE_INACTIVE_TEXT;
        if (strength >= (i + 1) * 25) {
            color = (strength > 75) ? CDE_SUCCESS_COLOR : (strength > 50) ? CDE_TEXT_COLOR : CDE_INACTIVE_TEXT;
        }

        draw_rect(ctx, bar_x, bar_y, bar_width, bar_height, color);
    }
}

static void populate_wifi_networks(app_context *ctx) {
    ctx->network_count = wifi_scan_get_num_results();
    free(ctx->networks);
    ctx->networks = calloc(ctx->network_count, sizeof(wifi_network));

    wifi_station_handle connected = wifi_get_connection_station();

    for (int i = 0; i < ctx->network_count; i++) {
        wifi_station_handle station = wifi_scan_get_result(i);
        strcpy(ctx->networks[i].ssid, wifi_station_get_ssid(station));
        ctx->networks[i].signal_strength = wifi_station_get_rssi(station) + 150;
        ctx->networks[i].secured         = wifi_station_get_mode(station) == WIFI_AUTH_OPEN ? false : true;
        if (connected) {
            ctx->networks[i].connected = strcmp(wifi_station_get_ssid(connected), wifi_station_get_ssid(station)) == 0;
        } else {
            ctx->networks[i].connected = false;
        }
        wifi_scan_free_station(station);
    }

    if (connected) {
        wifi_scan_free_station(connected);
    }
}

static void nav_push(app_context *ctx, ScreenState screen) {
    if (ctx->nav_depth < NAV_STACK_MAX) {
        ctx->nav_stack[ctx->nav_depth].screen        = ctx->current_screen;
        ctx->nav_stack[ctx->nav_depth].selected_item = ctx->selected_item;
        ctx->nav_stack[ctx->nav_depth].scroll_offset = ctx->scroll_offset;
        ctx->nav_depth++;
    }
    ctx->current_screen = screen;
    ctx->selected_item  = 0;
    ctx->scroll_offset  = 0;
}

static void nav_pop(app_context *ctx) {
    if (ctx->nav_depth > 0) {
        ctx->nav_depth--;
        ctx->current_screen = ctx->nav_stack[ctx->nav_depth].screen;
        ctx->selected_item  = ctx->nav_stack[ctx->nav_depth].selected_item;
        ctx->scroll_offset  = ctx->nav_stack[ctx->nav_depth].scroll_offset;
    } else {
        SDL_Event quit_event;
        quit_event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit_event);
    }
}

/* Grow reorder_items by one and return a pointer to the new zeroed entry. */
static reorder_item_t *reorder_append(app_context *ctx) {
    if (ctx->reorder_item_count >= ctx->reorder_item_cap) {
        int new_cap = ctx->reorder_item_cap == 0 ? 16 : ctx->reorder_item_cap * 2;
        reorder_item_t *tmp = realloc(ctx->reorder_items,
                                      (size_t)new_cap * sizeof(reorder_item_t));
        if (!tmp) return NULL;
        ctx->reorder_items    = tmp;
        ctx->reorder_item_cap = new_cap;
    }
    reorder_item_t *it = &ctx->reorder_items[ctx->reorder_item_count++];
    memset(it, 0, sizeof(*it));
    return it;
}

/* Append a uid string to a folder's folder_apps list. */
static void reorder_folder_append(reorder_item_t *folder, const char *uid) {
    if (folder->folder_app_count >= folder->folder_app_cap) {
        int new_cap = folder->folder_app_cap == 0 ? 8 : folder->folder_app_cap * 2;
        char **tmp = realloc(folder->folder_apps, (size_t)new_cap * sizeof(char *));
        if (!tmp) return;
        folder->folder_apps    = tmp;
        folder->folder_app_cap = new_cap;
    }
    char *dup = malloc(64);
    if (!dup) return;
    strncpy(dup, uid, 63);
    dup[63] = '\0';
    folder->folder_apps[folder->folder_app_count++] = dup;
}

static void reorder_free(app_context *ctx) {
    if (ctx->reorder_items) {
        for (int i = 0; i < ctx->reorder_item_count; i++) {
            reorder_item_t *it = &ctx->reorder_items[i];
            if (it->is_folder) {
                for (int j = 0; j < it->folder_app_count; j++)
                    free(it->folder_apps[j]);
                free(it->folder_apps);
            }
        }
        free(ctx->reorder_items);
        ctx->reorder_items      = NULL;
        ctx->reorder_item_count = 0;
        ctx->reorder_item_cap   = 0;
    }
    free(ctx->app_name_table);
    ctx->app_name_table  = NULL;
    ctx->app_name_count  = 0;
    ctx->reorder_held    = -1;
    ctx->reorder_in_folder = false;
    ctx->reorder_dialog_type = DIALOG_NONE;
}

static void reorder_save(app_context *ctx) {
    cJSON *root  = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    for (int i = 0; i < ctx->reorder_item_count; i++) {
        reorder_item_t *it  = &ctx->reorder_items[i];
        cJSON          *obj = cJSON_CreateObject();
        if (!it->is_folder) {
            cJSON_AddStringToObject(obj, "type", "app");
            cJSON_AddStringToObject(obj, "uid",  it->uid);
        } else {
            cJSON_AddStringToObject(obj, "type", "folder");
            cJSON_AddStringToObject(obj, "name", it->display_name);
            cJSON *apps_arr = cJSON_CreateArray();
            for (int j = 0; j < it->folder_app_count; j++)
                cJSON_AddItemToArray(apps_arr, cJSON_CreateString(it->folder_apps[j]));
            cJSON_AddItemToObject(obj, "apps", apps_arr);
        }
        cJSON_AddItemToArray(items, obj);
    }
    cJSON_AddItemToObject(root, "items", items);
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (json_str) {
        FILE *f = fopen("APPS:[badgevms_launcher]apps.json", "w");
        if (f) { fputs(json_str, f); fclose(f); }
        free(json_str);
    }
}

static void reorder_init(app_context *ctx) {
    reorder_free(ctx);   /* clear any leftover state */

    ctx->reorder_selected      = 0;
    ctx->reorder_scroll        = 0;
    ctx->reorder_held          = -1;
    ctx->reorder_held_origin   = -1;
    ctx->reorder_in_folder     = false;
    ctx->reorder_folder_idx    = -1;
    ctx->reorder_folder_sel    = 0;
    ctx->reorder_folder_scroll = 0;
    ctx->reorder_dialog_type   = DIALOG_NONE;

    /* Build uid→name lookup from installed apps.
     * Skip badgevms_launcher and why2025_firmware_ota_c6 — they are never shown
     * in the reorder screen and must not end up in apps.json. */
    static const char * const SKIP_UIDS[] = {
        "badgevms_launcher", "why2025_firmware_ota_c6", NULL
    };
    application_t          *app;
    application_list_handle handle = application_list(&app);
    while (app) {
        /* Filter out system UIDs that should never appear in the reorder list */
        bool skip = false;
        for (int si = 0; SKIP_UIDS[si]; si++) {
            if (strcmp(app->unique_identifier, SKIP_UIDS[si]) == 0) { skip = true; break; }
        }
        if (!skip) {
            app_name_entry_t *tmp = realloc(ctx->app_name_table,
                sizeof(app_name_entry_t) * (size_t)(ctx->app_name_count + 1));
            if (!tmp) break;
            ctx->app_name_table = tmp;
            strncpy(ctx->app_name_table[ctx->app_name_count].uid,
                    app->unique_identifier, 63);
            ctx->app_name_table[ctx->app_name_count].uid[63] = '\0';
            strncpy(ctx->app_name_table[ctx->app_name_count].name,
                    app->name, 63);
            ctx->app_name_table[ctx->app_name_count].name[63] = '\0';
            ctx->app_name_count++;
        }
        app = application_list_get_next(handle);
    }
    /* handle intentionally not closed — pointers stay valid for process lifetime */

    /* Helper: look up display name for a uid */
    #define LOOKUP_NAME(uid_str, out_name) do { \
        (out_name)[0] = '\0'; \
        for (int _i = 0; _i < ctx->app_name_count; _i++) { \
            if (strcmp(ctx->app_name_table[_i].uid, (uid_str)) == 0) { \
                strncpy((out_name), ctx->app_name_table[_i].name, 63); \
                (out_name)[63] = '\0'; \
                break; \
            } \
        } \
        if (!(out_name)[0]) { strncpy((out_name), (uid_str), 63); (out_name)[63] = '\0'; } \
    } while(0)

    /* Track which installed apps have been placed */
    bool *seen = calloc((size_t)ctx->app_name_count, sizeof(bool));

    /* Read and parse apps.json */
    FILE *f = fopen("APPS:[badgevms_launcher]apps.json", "r");
    cJSON *root = NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        if (sz > 0) {
            char *buf = malloc((size_t)sz + 1);
            if (buf) {
                size_t n = fread(buf, 1, (size_t)sz, f);
                buf[n] = '\0';
                root = cJSON_Parse(buf);
                free(buf);
            }
        }
        fclose(f);
    }

    if (root) {
        cJSON *items_arr = cJSON_GetObjectItem(root, "items");
        if (items_arr) {
            cJSON *item_obj;
            cJSON_ArrayForEach(item_obj, items_arr) {
                cJSON      *type_j   = cJSON_GetObjectItem(item_obj, "type");
                const char *type_str = cJSON_GetStringValue(type_j);
                if (!type_str) continue;

                if (strcmp(type_str, "app") == 0) {
                    const char *uid = cJSON_GetStringValue(cJSON_GetObjectItem(item_obj, "uid"));
                    if (!uid) continue;
                    /* Only add if installed */
                    int installed_idx = -1;
                    for (int i = 0; i < ctx->app_name_count; i++) {
                        if (strcmp(ctx->app_name_table[i].uid, uid) == 0) {
                            installed_idx = i;
                            break;
                        }
                    }
                    if (installed_idx < 0) continue;
                    if (seen) seen[installed_idx] = true;
                    reorder_item_t *it = reorder_append(ctx);
                    if (!it) continue;
                    it->is_folder = false;
                    strncpy(it->uid, uid, 63); it->uid[63] = '\0';
                    LOOKUP_NAME(uid, it->display_name);

                } else if (strcmp(type_str, "folder") == 0) {
                    const char *folder_name = cJSON_GetStringValue(
                        cJSON_GetObjectItem(item_obj, "name"));
                    cJSON *apps_j = cJSON_GetObjectItem(item_obj, "apps");
                    if (!folder_name || !apps_j) continue;

                    reorder_item_t *it = reorder_append(ctx);
                    if (!it) continue;
                    it->is_folder = true;
                    strncpy(it->display_name, folder_name, 63);
                    it->display_name[63] = '\0';

                    cJSON *uid_item;
                    cJSON_ArrayForEach(uid_item, apps_j) {
                        const char *uid = cJSON_GetStringValue(uid_item);
                        if (!uid) continue;
                        for (int i = 0; i < ctx->app_name_count; i++) {
                            if (strcmp(ctx->app_name_table[i].uid, uid) == 0) {
                                if (seen) seen[i] = true;
                                reorder_folder_append(it, uid);
                                break;
                            }
                        }
                    }
                }
            }
        }
        cJSON_Delete(root);
    }

    /* Append any installed apps not seen in apps.json */
    for (int i = 0; i < ctx->app_name_count; i++) {
        if (!seen || !seen[i]) {
            reorder_item_t *it = reorder_append(ctx);
            if (!it) continue;
            it->is_folder = false;
            strncpy(it->uid,          ctx->app_name_table[i].uid,  63); it->uid[63]          = '\0';
            strncpy(it->display_name, ctx->app_name_table[i].name, 63); it->display_name[63] = '\0';
        }
    }

    free(seen);
    #undef LOOKUP_NAME
}

static void draw_reorder_dialog(app_context *ctx) {
    int dialog_w = 500;
    int dialog_h = (ctx->reorder_dialog_type == DIALOG_DELETE_CONFIRM) ? 200 : 220;
    int dialog_x = (SCREEN_WIDTH  - dialog_w) / 2;
    int dialog_y = (SCREEN_HEIGHT - dialog_h) / 2;

    draw_rect(ctx, dialog_x + 5, dialog_y + 5, dialog_w, dialog_h, 0x505050);
    draw_rect(ctx, dialog_x, dialog_y, dialog_w, dialog_h, CDE_PANEL_COLOR);
    draw_3d_border(ctx, dialog_x, dialog_y, dialog_w, dialog_h, 0);

    int title_h = 30;
    draw_rect(ctx, dialog_x + 2, dialog_y + 2, dialog_w - 4, title_h, CDE_TITLE_BG);

    if (ctx->reorder_dialog_type == DIALOG_DELETE_CONFIRM) {
        draw_text_bold(ctx, dialog_x + 10, dialog_y + 8, "Delete Folder", CDE_SELECTED_TEXT);
        char msg1[128];
        snprintf(msg1, sizeof(msg1), "Delete folder '%s'?",
                 ctx->reorder_items[ctx->reorder_rename_idx].display_name);
        draw_text(ctx, dialog_x + 20, dialog_y + title_h + 25, msg1, CDE_TEXT_COLOR);
        draw_text(ctx, dialog_x + 20, dialog_y + title_h + 55,
                  "All apps will be moved to the home screen.", CDE_INACTIVE_TEXT);
        draw_text_centered(ctx, dialog_x, dialog_y + dialog_h - 35, dialog_w,
                           "ENTER: Confirm   ESC: Cancel", CDE_INACTIVE_TEXT);
    } else {
        const char *title_text = (ctx->reorder_dialog_type == DIALOG_NEW_FOLDER)
            ? "New Folder" : "Rename Folder";
        draw_text_bold(ctx, dialog_x + 10, dialog_y + 8, title_text, CDE_SELECTED_TEXT);

        int cy = dialog_y + title_h + 25;
        draw_text(ctx, dialog_x + 20, cy, "Folder name:", CDE_TEXT_COLOR);

        int field_x = dialog_x + 20;
        int field_y = cy + 30;
        int field_w = dialog_w - 40;
        int field_h = 35;
        draw_rect(ctx, field_x, field_y, field_w, field_h, 0xFFFFFF);
        draw_3d_border(ctx, field_x, field_y, field_w, field_h, 1);
        draw_text(ctx, field_x + 5, field_y + 7, ctx->reorder_dialog_buf, CDE_TEXT_COLOR);
        int cursor_x = field_x + 5 + get_text_width(ctx->reorder_dialog_buf);
        if (SDL_GetTicks() % 1000 < 500)
            draw_rect(ctx, cursor_x, field_y + 7, 2, FONT_HEIGHT, CDE_TEXT_COLOR);

        draw_text_centered(ctx, dialog_x, dialog_y + dialog_h - 35, dialog_w,
                           "ENTER: Confirm   ESC: Cancel", CDE_INACTIVE_TEXT);
    }
}

static void draw_reorder_screen(app_context *ctx) {
    int window_x = 30, window_y = 30;
    int window_w = SCREEN_WIDTH - 60, window_h = SCREEN_HEIGHT - 60;

    draw_rect(ctx, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, CDE_BG_COLOR);
    draw_rect(ctx, window_x, window_y, window_w, window_h, CDE_PANEL_COLOR);
    draw_3d_border(ctx, window_x, window_y, window_w, window_h, 0);

    int title_h = 45;
    draw_rect(ctx, window_x + 3, window_y + 3, window_w - 6, title_h, CDE_TITLE_BG);
    if (ctx->reorder_in_folder) {
        char title[128];
        snprintf(title, sizeof(title), "Reorder Apps > %s",
                 ctx->reorder_items[ctx->reorder_folder_idx].display_name);
        draw_text_bold(ctx, window_x + 15, window_y + 11, title, CDE_SELECTED_TEXT);
    } else {
        draw_text_bold(ctx, window_x + 15, window_y + 11, "Reorder Apps", CDE_SELECTED_TEXT);
    }

    int list_y      = window_y + title_h + 15;
    int list_h      = window_h - title_h - 70;
    int item_height = 70;

    draw_rect(ctx, window_x + 15, list_y, window_w - 30, list_h, 0xFFFFFF);
    draw_3d_border(ctx, window_x + 15, list_y, window_w - 30, list_h, 1);

    int ipp = (list_h - 6) / item_height;
    ctx->reorder_items_per_page = ipp;

    if (ctx->reorder_in_folder) {
        /* ---- Folder sub-view ---- */
        reorder_item_t *folder = &ctx->reorder_items[ctx->reorder_folder_idx];
        int total  = folder->folder_app_count;
        int scroll = ctx->reorder_folder_scroll;
        int sel    = ctx->reorder_folder_sel;
        int ve     = scroll + ipp;
        if (ve > total) ve = total;

        for (int i = scroll; i < ve; i++) {
            int item_y = list_y + 3 + (i - scroll) * item_height;
            int item_x = window_x + 18;
            int item_w = window_w - 36;
            bool selected = (i == sel);

            if (selected)
                draw_rect(ctx, item_x, item_y, item_w, item_height - 2, CDE_SELECTED_BG);
            uint32_t tc = selected ? CDE_SELECTED_TEXT : CDE_TEXT_COLOR;
            uint32_t sc = selected ? CDE_SELECTED_TEXT : CDE_INACTIVE_TEXT;

            int icon_x = item_x + 10;
            int icon_y = item_y + (item_height - 48) / 2;
            draw_rect(ctx, icon_x, icon_y, 48, 48, selected ? CDE_SELECTED_TEXT : CDE_BUTTON_COLOR);
            draw_3d_border(ctx, icon_x, icon_y, 48, 48, 1);

            const char *uid = folder->folder_apps[i];
            const char *name = uid;
            for (int j = 0; j < ctx->app_name_count; j++) {
                if (strcmp(ctx->app_name_table[j].uid, uid) == 0) {
                    name = ctx->app_name_table[j].name;
                    break;
                }
            }
            int text_x = icon_x + 48 + 15;
            draw_text_bold(ctx, text_x, item_y + 10, name, tc);
            draw_text(ctx, text_x, item_y + 35, uid, sc);

            if (i < ve - 1)
                draw_rect(ctx, item_x, item_y + item_height - 2, item_w, 1, CDE_BORDER_DARK);
        }

        /* Footer */
        draw_rect(ctx, window_x + 3, window_y + window_h - 42, window_w - 6, 39, CDE_BUTTON_COLOR);
        draw_3d_border(ctx, window_x + 3, window_y + window_h - 42, window_w - 6, 39, 1);
        draw_text(ctx, window_x + 15, window_y + window_h - 35,
                  "UP/DOWN: Navigate  BACKSPACE: Remove from folder  ESC: Back",
                  CDE_TEXT_COLOR);

    } else {
        /* ---- Home view ---- */
        int total  = ctx->reorder_item_count;
        int scroll = ctx->reorder_scroll;
        int sel    = ctx->reorder_selected;
        int held   = ctx->reorder_held;
        int ve     = scroll + ipp;
        if (ve > total) ve = total;

        for (int i = scroll; i < ve; i++) {
            int item_y = list_y + 3 + (i - scroll) * item_height;
            int item_x = window_x + 18;
            int item_w = window_w - 36;
            reorder_item_t *it = &ctx->reorder_items[i];
            bool is_sel  = (i == sel);
            bool is_held = (i == held && held >= 0);

            uint32_t bg = 0;
            if (is_sel)       bg = CDE_SELECTED_BG;
            else if (is_held) bg = CDE_TITLE_BG;
            if (bg) draw_rect(ctx, item_x, item_y, item_w, item_height - 2, bg);

            uint32_t tc = (is_sel || is_held) ? CDE_SELECTED_TEXT : CDE_TEXT_COLOR;
            uint32_t sc = (is_sel || is_held) ? CDE_SELECTED_TEXT : CDE_INACTIVE_TEXT;

            int icon_x = item_x + 10;
            int icon_y = item_y + (item_height - 48) / 2;
            draw_rect(ctx, icon_x, icon_y, 48, 48, (is_sel || is_held) ? CDE_SELECTED_TEXT : CDE_BUTTON_COLOR);
            draw_3d_border(ctx, icon_x, icon_y, 48, 48, 1);

            /* Show [>] inside icon box for held-but-not-cursor item */
            if (is_held && !is_sel)
                draw_text(ctx, icon_x + 8, icon_y + 16, "[>]", CDE_SELECTED_TEXT);

            int text_x = icon_x + 48 + 15;
            if (it->is_folder) {
                if (!is_held || is_sel)
                    draw_text(ctx, icon_x + 8, icon_y + 16, "[F]", tc);
                draw_text_bold(ctx, text_x, item_y + 10, it->display_name, tc);
                char sub[48];
                snprintf(sub, sizeof(sub), "%d app%s",
                         it->folder_app_count, it->folder_app_count == 1 ? "" : "s");
                draw_text(ctx, text_x, item_y + 35, sub, sc);
            } else {
                draw_text_bold(ctx, text_x, item_y + 10, it->display_name, tc);
                draw_text(ctx, text_x, item_y + 35, it->uid, sc);
            }

            if (i < ve - 1)
                draw_rect(ctx, item_x, item_y + item_height - 2, item_w, 1, CDE_BORDER_DARK);
        }

        /* Scrollbar */
        if (total > ipp) {
            int sbx = window_x + window_w - 35;
            int sby = list_y + 3;
            int sbh = list_h - 6;
            draw_rect(ctx, sbx, sby, 20, sbh, CDE_BUTTON_COLOR);
            draw_3d_border(ctx, sbx, sby, 20, sbh, 1);
            int th = (sbh * ipp) / total;
            if (th < 20) th = 20;
            int ty = sby + (total > ipp ? ((sbh - th) * scroll) / (total - ipp) : 0);
            draw_rect(ctx, sbx + 3, ty, 14, th, CDE_PANEL_COLOR);
            draw_3d_border(ctx, sbx + 3, ty, 14, th, 0);
        }

        /* Footer */
        draw_rect(ctx, window_x + 3, window_y + window_h - 42, window_w - 6, 39, CDE_BUTTON_COLOR);
        draw_3d_border(ctx, window_x + 3, window_y + window_h - 42, window_w - 6, 39, 1);
        const char *hint = (held >= 0)
            ? "UP/DOWN: Navigate  ENTER: Drop/Place in folder  ESC: Cancel"
            : "UP/DOWN: Navigate  ENTER: Grab/Enter  N: New  R: Rename  BACKSPACE: Delete  ESC: Back";
        draw_text(ctx, window_x + 15, window_y + window_h - 35, hint, CDE_TEXT_COLOR);
    }

    /* Dialog overlay */
    if (ctx->reorder_dialog_type != DIALOG_NONE)
        draw_reorder_dialog(ctx);
}

static void app_chooser_open(app_context *ctx) {
    app_chooser_close(ctx); /* free any prior allocation if called while open */

    /* Populate chooser_apps from installed app list, excluding the launcher itself */
    ctx->chooser_app_count = 0;
    ctx->chooser_apps      = NULL;

    application_t          *app;
    application_list_handle handle = application_list(&app);
    while (app) {
        if (app->unique_identifier &&
            strcmp(app->unique_identifier, "badgevms_launcher") != 0 &&
            strcmp(app->unique_identifier, "why2025_firmware_ota_c6") != 0 &&
            app->binary_path && strlen(app->binary_path) > 0) {

            app_name_entry_t *tmp = realloc(ctx->chooser_apps,
                (size_t)(ctx->chooser_app_count + 1) * sizeof(app_name_entry_t));
            if (tmp) {
                ctx->chooser_apps = tmp;
                strncpy(ctx->chooser_apps[ctx->chooser_app_count].uid,
                        app->unique_identifier,
                        sizeof(ctx->chooser_apps[0].uid) - 1);
                ctx->chooser_apps[ctx->chooser_app_count].uid[sizeof(ctx->chooser_apps[0].uid) - 1] = '\0';
                strncpy(ctx->chooser_apps[ctx->chooser_app_count].name,
                        app->name ? app->name : app->unique_identifier,
                        sizeof(ctx->chooser_apps[0].name) - 1);
                ctx->chooser_apps[ctx->chooser_app_count].name[sizeof(ctx->chooser_apps[0].name) - 1] = '\0';
                ctx->chooser_app_count++;
            }
        }
        app = application_list_get_next(handle);
    }
    application_list_close(handle);

    /* Pre-select the currently configured app, if present */
    ctx->chooser_selected = 0;
    for (int i = 0; i < ctx->chooser_app_count; i++) {
        if (strcmp(ctx->chooser_apps[i].uid, ctx->launcher_default_uid) == 0) {
            ctx->chooser_selected = i;
            break;
        }
    }
    ctx->chooser_scroll          = 0;
    ctx->chooser_items_per_page  = 8;
    /* Scroll so pre-selected item is visible */
    if (ctx->chooser_selected >= ctx->chooser_items_per_page)
        ctx->chooser_scroll = ctx->chooser_selected - ctx->chooser_items_per_page + 1;

    ctx->show_app_chooser = true;
}

static void app_chooser_close(app_context *ctx) {
    free(ctx->chooser_apps);
    ctx->chooser_apps      = NULL;
    ctx->chooser_app_count = 0;
    ctx->show_app_chooser  = false;
}

static void launcher_config_load(app_context *ctx) {
    ctx->launch_default_app      = false;
    ctx->launcher_default_uid[0] = '\0';
    ctx->launcher_default_name[0] = '\0';

    FILE *f = fopen("APPS:[badgevms_launcher]config.json", "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 4096) { fclose(f); return; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);

    cJSON *cfg = cJSON_Parse(buf);
    free(buf);
    if (!cfg) return;

    cJSON *lda = cJSON_GetObjectItem(cfg, "launch_default_app");
    cJSON *da  = cJSON_GetObjectItem(cfg, "default_app");
    if (cJSON_IsBool(lda))
        ctx->launch_default_app = cJSON_IsTrue(lda);
    if (cJSON_IsString(da) && da->valuestring)
        strncpy(ctx->launcher_default_uid, da->valuestring,
                sizeof(ctx->launcher_default_uid) - 1);
    cJSON_Delete(cfg);

    /* Resolve display name by walking the installed app list.
     * We avoid application_get()+application_free() because application_free
     * is not exported in the BadgeVMS runtime symbol table. */
    if (ctx->launcher_default_uid[0]) {
        application_t          *a;
        application_list_handle h = application_list(&a);
        while (a) {
            if (a->unique_identifier &&
                strcmp(a->unique_identifier, ctx->launcher_default_uid) == 0) {
                if (a->name)
                    strncpy(ctx->launcher_default_name, a->name,
                            sizeof(ctx->launcher_default_name) - 1);
                break;
            }
            a = application_list_get_next(h);
        }
        application_list_close(h);
        /* Fall back to uid if display name not found */
        if (!ctx->launcher_default_name[0])
            strncpy(ctx->launcher_default_name, ctx->launcher_default_uid,
                    sizeof(ctx->launcher_default_name) - 1);
    }
}

static void launcher_config_save(app_context *ctx) {
    cJSON *cfg = cJSON_CreateObject();
    if (!cfg) return;
    cJSON_AddBoolToObject(cfg, "launch_default_app", ctx->launch_default_app);
    cJSON_AddStringToObject(cfg, "default_app", ctx->launcher_default_uid);
    char *json_str = cJSON_Print(cfg);
    cJSON_Delete(cfg);
    if (!json_str) return;
    FILE *f = fopen("APPS:[badgevms_launcher]config.json", "w");
    if (f) { fputs(json_str, f); fclose(f); }
    free(json_str);
}

static void draw_app_chooser_dialog(app_context *ctx) {
    int dialog_w = 500;
    int dialog_h = 420;
    int dialog_x = (SCREEN_WIDTH  - dialog_w) / 2;
    int dialog_y = (SCREEN_HEIGHT - dialog_h) / 2;

    /* Drop shadow */
    draw_rect(ctx, dialog_x + 5, dialog_y + 5, dialog_w, dialog_h, 0x505050);

    /* Panel */
    draw_rect(ctx, dialog_x, dialog_y, dialog_w, dialog_h, CDE_PANEL_COLOR);
    draw_3d_border(ctx, dialog_x, dialog_y, dialog_w, dialog_h, 0);

    /* Title bar */
    int title_h = 30;
    draw_rect(ctx, dialog_x + 2, dialog_y + 2, dialog_w - 4, title_h, CDE_TITLE_BG);
    draw_text_bold(ctx, dialog_x + 10, dialog_y + 8, "Select Default App", CDE_SELECTED_TEXT);

    /* App list */
    int list_x      = dialog_x + 10;
    int list_y      = dialog_y + title_h + 8;
    int list_w      = dialog_w - 20;
    int list_h      = dialog_h - title_h - 50;
    int item_height = 40;
    ctx->chooser_items_per_page = list_h / item_height;

    draw_rect(ctx, list_x, list_y, list_w, list_h, 0xFFFFFF);
    draw_3d_border(ctx, list_x, list_y, list_w, list_h, 1);

    int visible_start = ctx->chooser_scroll;
    int visible_end   = visible_start + ctx->chooser_items_per_page;
    if (visible_end > ctx->chooser_app_count)
        visible_end = ctx->chooser_app_count;

    for (int i = visible_start; i < visible_end; i++) {
        int row_y = list_y + 3 + (i - visible_start) * item_height;
        int row_x = list_x + 3;
        int row_w = list_w - 6;

        bool selected = (i == ctx->chooser_selected);
        if (selected)
            draw_rect(ctx, row_x, row_y, row_w, item_height - 2, CDE_SELECTED_BG);

        uint32_t text_color = selected ? CDE_SELECTED_TEXT : CDE_TEXT_COLOR;
        draw_text_bold(ctx, row_x + 8, row_y + 10, ctx->chooser_apps[i].name, text_color);

        if (i < visible_end - 1)
            draw_rect(ctx, row_x, row_y + item_height - 2, row_w, 1, CDE_BORDER_DARK);
    }

    /* Footer */
    int footer_y = dialog_y + dialog_h - 38;
    draw_rect(ctx, dialog_x + 2, footer_y, dialog_w - 4, 36, CDE_BUTTON_COLOR);
    draw_3d_border(ctx, dialog_x + 2, footer_y, dialog_w - 4, 36, 1);
    draw_text(ctx, dialog_x + 10, footer_y + 8,
              "UP/DOWN: Navigate  ENTER: Select  ESC: Cancel",
              CDE_TEXT_COLOR);
}

static void draw_main_settings(app_context *ctx) {
    int window_x = 30;
    int window_y = 30;
    int window_w = SCREEN_WIDTH - 60;
    int window_h = SCREEN_HEIGHT - 60;

    draw_rect(ctx, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, CDE_BG_COLOR);

    draw_rect(ctx, window_x, window_y, window_w, window_h, CDE_PANEL_COLOR);
    draw_3d_border(ctx, window_x, window_y, window_w, window_h, 0);

    int title_h = 45;
    draw_rect(ctx, window_x + 3, window_y + 3, window_w - 6, title_h, CDE_TITLE_BG);
    draw_text_bold(ctx, window_x + 15, window_y + 11, "System Settings", CDE_SELECTED_TEXT);

    char const *categories[]   = {
        "WiFi Settings",
        "Reorder Apps",
        "Launch default app",
        "Default app",
        "About"
    };
    char const *descriptions[] = {
        "Configure wireless network connection",
        "Organise launcher home screen and folders",
        "Launch an application at startup",
        "The application launched at boot",
        "Badge specifications"
    };
    ctx->total_items = 5;

    int list_y      = window_y + title_h + 20;
    int list_h      = window_h - title_h - 80;
    int item_height = 80;

    draw_rect(ctx, window_x + 15, list_y, window_w - 30, list_h, 0xFFFFFF);
    draw_3d_border(ctx, window_x + 15, list_y, window_w - 30, list_h, 1);

    for (int i = 0; i < ctx->total_items; i++) {
        int item_y = list_y + 3 + i * item_height;
        int item_x = window_x + 18;
        int item_w = window_w - 36;

        if (i == ctx->selected_item) {
            draw_rect(ctx, item_x, item_y, item_w, item_height - 2, CDE_SELECTED_BG);
        }

        uint32_t text_color = (i == ctx->selected_item) ? CDE_SELECTED_TEXT : CDE_TEXT_COLOR;
        uint32_t desc_color = (i == ctx->selected_item) ? CDE_SELECTED_TEXT : CDE_INACTIVE_TEXT;

        int icon_size  = 48;
        int icon_x     = item_x + 10;
        int icon_y_pos = item_y + (item_height - icon_size) / 2;

        uint32_t icon_color = (i == ctx->selected_item) ? CDE_SELECTED_TEXT : CDE_BUTTON_COLOR;
        draw_rect(ctx, icon_x, icon_y_pos, icon_size, icon_size, icon_color);
        draw_3d_border(ctx, icon_x, icon_y_pos, icon_size, icon_size, 1);

        int text_x = icon_x + icon_size + 15;
        draw_text_bold(ctx, text_x, item_y + 15, categories[i], text_color);
        draw_text(ctx, text_x, item_y + 45, descriptions[i], desc_color);

        /* Right-side value for config entries */
        if (i == 2) { /* Launch default app: show [ON] or [OFF] */
            const char *toggle_str = ctx->launch_default_app ? "[ON] " : "[OFF]";
            int toggle_w = get_text_width(toggle_str);
            int toggle_x = item_x + item_w - toggle_w - 15;
            uint32_t toggle_color;
            if (i == ctx->selected_item)
                toggle_color = CDE_SELECTED_TEXT;
            else
                toggle_color = ctx->launch_default_app ? CDE_SUCCESS_COLOR : CDE_INACTIVE_TEXT;
            draw_text_bold(ctx, toggle_x, item_y + 15, toggle_str, toggle_color);
        } else if (i == 3) { /* Default app: show app display name */
            const char *app_name = ctx->launcher_default_name[0]
                ? ctx->launcher_default_name : "(none)";
            int name_w = get_text_width(app_name);
            int name_x = item_x + item_w - name_w - 15;
            uint32_t name_color = (i == ctx->selected_item) ? CDE_SELECTED_TEXT : CDE_INACTIVE_TEXT;
            draw_text(ctx, name_x, item_y + 15, app_name, name_color);
        }

        if (i < ctx->total_items - 1) {
            draw_rect(ctx, item_x, item_y + item_height - 2, item_w, 1, CDE_BORDER_DARK);
        }
    }

    draw_rect(ctx, window_x + 3, window_y + window_h - 42, window_w - 6, 39, CDE_BUTTON_COLOR);
    draw_3d_border(ctx, window_x + 3, window_y + window_h - 42, window_w - 6, 39, 1);
    draw_text(
        ctx,
        window_x + 15,
        window_y + window_h - 35,
        "UP/DOWN: Navigate  ENTER: Select  ESC: Exit",
        CDE_TEXT_COLOR
    );
}

static void draw_wifi_settings(app_context *ctx) {
    int window_x = 30;
    int window_y = 30;
    int window_w = SCREEN_WIDTH - 60;
    int window_h = SCREEN_HEIGHT - 60;

    draw_rect(ctx, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, CDE_BG_COLOR);

    draw_rect(ctx, window_x, window_y, window_w, window_h, CDE_PANEL_COLOR);
    draw_3d_border(ctx, window_x, window_y, window_w, window_h, 0);

    int title_h = 45;
    draw_rect(ctx, window_x + 3, window_y + 3, window_w - 6, title_h, CDE_TITLE_BG);
    draw_text_bold(ctx, window_x + 15, window_y + 11, "WiFi Settings", CDE_SELECTED_TEXT);

    bool is_connected = false;
    for (int i = 0; i < ctx->network_count; i++) {
        if (ctx->networks[i].connected) {
            snprintf(ctx->connection_status_text, sizeof(ctx->connection_status_text), "Connected");
            is_connected = true;
            break;
        }
    }

    draw_text(ctx, window_x + 15, window_y + title_h + 15, "Status:", CDE_TEXT_COLOR);
    draw_text_bold(
        ctx,
        window_x + 90,
        window_y + title_h + 15,
        ctx->connection_status_text,
        is_connected ? CDE_SUCCESS_COLOR : CDE_INACTIVE_TEXT
    );

    draw_text(ctx, window_x + 15, window_y + title_h + 45, "Available Networks:", CDE_TEXT_COLOR);

    int list_y      = window_y + title_h + 75;
    int list_h      = window_h - title_h - 130;
    int item_height = 60;

    draw_rect(ctx, window_x + 15, list_y, window_w - 30, list_h, 0xFFFFFF);
    draw_3d_border(ctx, window_x + 15, list_y, window_w - 30, list_h, 1);

    ctx->items_per_page = (list_h - 6) / item_height;
    int visible_start   = ctx->scroll_offset;
    int visible_end     = visible_start + ctx->items_per_page;
    if (visible_end > ctx->network_count)
        visible_end = ctx->network_count;

    for (int i = visible_start; i < visible_end; i++) {
        int item_y = list_y + 3 + (i - visible_start) * item_height;
        int item_x = window_x + 18;
        int item_w = window_w - 36;

        if (i == ctx->selected_item) {
            draw_rect(ctx, item_x, item_y, item_w, item_height - 2, CDE_SELECTED_BG);
        }

        uint32_t text_color = (i == ctx->selected_item) ? CDE_SELECTED_TEXT : CDE_TEXT_COLOR;

        draw_text_bold(ctx, item_x + 10, item_y + 10, ctx->networks[i].ssid, text_color);

        if (ctx->networks[i].connected) {
            draw_text(ctx, item_x + 10, item_y + 35, "[Connected]", CDE_SUCCESS_COLOR);
        }

        draw_signal_strength(ctx, item_x + item_w - 50, item_y + 20, ctx->networks[i].signal_strength);

        if (i < visible_end - 1) {
            draw_rect(ctx, item_x, item_y + item_height - 2, item_w, 1, CDE_BORDER_DARK);
        }
    }

    if (ctx->network_count > ctx->items_per_page) {
        int scrollbar_x = window_x + window_w - 35;
        int scrollbar_y = list_y + 3;
        int scrollbar_h = list_h - 6;

        draw_rect(ctx, scrollbar_x, scrollbar_y, 20, scrollbar_h, CDE_BUTTON_COLOR);
        draw_3d_border(ctx, scrollbar_x, scrollbar_y, 20, scrollbar_h, 1);

        int thumb_h = (scrollbar_h * ctx->items_per_page) / ctx->network_count;
        if (thumb_h < 30)
            thumb_h = 30;

        int thumb_y = scrollbar_y;
        if (ctx->network_count > ctx->items_per_page) {
            thumb_y += ((scrollbar_h - thumb_h) * ctx->scroll_offset) / (ctx->network_count - ctx->items_per_page);
        }

        draw_rect(ctx, scrollbar_x + 3, thumb_y, 14, thumb_h, CDE_PANEL_COLOR);
        draw_3d_border(ctx, scrollbar_x + 3, thumb_y, 14, thumb_h, 0);
    }

    draw_rect(ctx, window_x + 3, window_y + window_h - 42, window_w - 6, 39, CDE_BUTTON_COLOR);
    draw_3d_border(ctx, window_x + 3, window_y + window_h - 42, window_w - 6, 39, 1);

    if (!ctx->network_count && (SDL_GetTicks() >= ctx->status_timer)) {
        ctx->status_timer = SDL_GetTicks() + 3000;
        strcpy(ctx->status_message, "Scanning for networks...");
    }

    if (ctx->status_message[0] && SDL_GetTicks() < ctx->status_timer) {
        draw_text(ctx, window_x + 15, window_y + window_h - 35, ctx->status_message, ctx->status_color);
    } else {
        draw_text(
            ctx,
            window_x + 15,
            window_y + window_h - 35,
            "UP/DOWN: Navigate  ENTER: Go  S: Scan  ESC: Back",
            CDE_TEXT_COLOR
        );
    }

    if (!ctx->network_count) {
        populate_wifi_networks(ctx);
    }
}

static void draw_connecting_screen(app_context *ctx) {
    int window_x = 100;
    int window_y = 200;
    int window_w = SCREEN_WIDTH - 200;
    int window_h = 200;

    draw_rect(ctx, window_x + 5, window_y + 5, window_w, window_h, 0x505050);

    draw_rect(ctx, window_x, window_y, window_w, window_h, CDE_PANEL_COLOR);
    draw_3d_border(ctx, window_x, window_y, window_w, window_h, 0);

    int title_h = 35;
    draw_rect(ctx, window_x + 2, window_y + 2, window_w - 4, title_h, CDE_TITLE_BG);
    draw_text_bold(ctx, window_x + 15, window_y + 10, "Connecting to WiFi", CDE_SELECTED_TEXT);

    int content_y = window_y + title_h + 40;

    char network_text[128];
    snprintf(network_text, sizeof(network_text), "Network: %s", ctx->connecting_ssid);
    draw_text_centered(ctx, window_x, content_y, window_w, network_text, CDE_TEXT_COLOR);

    content_y += 35;
    char security_text[64];
    snprintf(security_text, sizeof(security_text), "Security: %s", ctx->connecting_secured ? "WPA/WPA2" : "Open");
    draw_text_centered(ctx, window_x, content_y, window_w, security_text, CDE_INACTIVE_TEXT);

    content_y += 50;
    draw_text_centered(ctx, window_x, content_y, window_w, "Connecting...", CDE_TEXT_COLOR);
}

static void draw_password_dialog(app_context *ctx) {
    int dialog_w = 600;
    int dialog_h = 300;
    int dialog_x = (SCREEN_WIDTH - dialog_w) / 2;
    int dialog_y = (SCREEN_HEIGHT - dialog_h) / 2;

    draw_rect(ctx, dialog_x + 5, dialog_y + 5, dialog_w, dialog_h, 0x505050);

    draw_rect(ctx, dialog_x, dialog_y, dialog_w, dialog_h, CDE_PANEL_COLOR);
    draw_3d_border(ctx, dialog_x, dialog_y, dialog_w, dialog_h, 0);

    int title_h = 30;
    draw_rect(ctx, dialog_x + 2, dialog_y + 2, dialog_w - 4, title_h, CDE_TITLE_BG);
    draw_text_bold(ctx, dialog_x + 10, dialog_y + 8, "Enter WiFi Password", CDE_SELECTED_TEXT);

    char ssid_text[128];
    snprintf(ssid_text, sizeof(ssid_text), "Network: %s", ctx->networks[ctx->selected_network].ssid);
    draw_text(ctx, dialog_x + 20, dialog_y + title_h + 25, ssid_text, CDE_TEXT_COLOR);

    int field_x = dialog_x + 20;
    int field_y = dialog_y + title_h + 60;
    int field_w = dialog_w - 40;
    int field_h = 35;

    draw_rect(ctx, field_x, field_y, field_w, field_h, 0xFFFFFF);
    draw_3d_border(ctx, field_x, field_y, field_w, field_h, 1);

    char display_buffer[128];
    if (ctx->show_password) {
        strcpy(display_buffer, ctx->password_buffer);
    } else {
        int len = strlen(ctx->password_buffer);
        for (int i = 0; i < len; i++) {
            display_buffer[i] = '*';
        }
        display_buffer[len] = '\0';
    }

    draw_text(ctx, field_x + 5, field_y + 7, display_buffer, CDE_TEXT_COLOR);

    int cursor_x = field_x + 5 + get_text_width(display_buffer);
    if (SDL_GetTicks() % 1000 < 500) { // Blinking cursor
        draw_rect(ctx, cursor_x, field_y + 7, 2, FONT_HEIGHT, CDE_TEXT_COLOR);
    }

    draw_text_centered(
        ctx,
        dialog_x,
        dialog_y + dialog_h - 60,
        dialog_w,
        "TAB: Toggle show  ENTER: Connect  ESC: Cancel",
        CDE_INACTIVE_TEXT
    );
}

static void draw_about_dialog(app_context *ctx) {
    int dialog_w = 620;
    int dialog_h = 350;
    int dialog_x = (SCREEN_WIDTH  - dialog_w) / 2;
    int dialog_y = (SCREEN_HEIGHT - dialog_h) / 2;

    draw_rect(ctx, dialog_x + 5, dialog_y + 5, dialog_w, dialog_h, 0x505050);
    draw_rect(ctx, dialog_x, dialog_y, dialog_w, dialog_h, CDE_PANEL_COLOR);
    draw_3d_border(ctx, dialog_x, dialog_y, dialog_w, dialog_h, 0);

    int title_h = 30;
    draw_rect(ctx, dialog_x + 2, dialog_y + 2, dialog_w - 4, title_h, CDE_TITLE_BG);
    draw_text_bold(ctx, dialog_x + 10, dialog_y + 8, "About the WHY2025 Badge", CDE_SELECTED_TEXT);

    int x  = dialog_x + 20;
    int lh = FONT_HEIGHT + 6;
    int y  = dialog_y + title_h + 12;

    draw_text_bold(ctx, x, y, "Compute Unit", CDE_SELECTED_TEXT);
    y += lh;
    draw_text(ctx, x + 12, y, "ESP32-P4  -  Main processor", CDE_TEXT_COLOR);
    y += lh;
    draw_text(ctx, x + 12, y, "SD Slot   -  Additional storage", CDE_TEXT_COLOR);
    y += lh + 6;

    draw_text_bold(ctx, x, y, "Carrier Board", CDE_SELECTED_TEXT);
    y += lh;
    draw_text(ctx, x + 12, y, "ESP32-C6  -  Wi-Fi & Bluetooth connectivity", CDE_TEXT_COLOR);
    y += lh;
    draw_text(ctx, x + 12, y, "Display   -  4\" square 720x720 MIPI DSI", CDE_TEXT_COLOR);
    y += lh;
    draw_text(ctx, x + 12, y, "BMI270    -  Accelerometer & gyroscope", CDE_TEXT_COLOR);
    y += lh;
    draw_text(ctx, x + 12, y, "BME690    -  Air quality, temperature, humidity", CDE_TEXT_COLOR);
    y += lh + 6;

    draw_rect(ctx, dialog_x + 15, y, dialog_w - 30, 1, CDE_BORDER_DARK);
    y += 8;
    draw_text_centered(ctx, dialog_x, y, dialog_w, "ENTER or ESC to close", CDE_INACTIVE_TEXT);
}

static void attempt_wifi_connection(app_context *ctx) {
    strcpy(ctx->connecting_ssid, ctx->networks[ctx->selected_network].ssid);
    ctx->connecting_secured = ctx->networks[ctx->selected_network].secured;

    ctx->show_password_dialog   = false;
    ctx->show_connecting_dialog = true;
    render_screen(ctx);

    wifi_disconnect();
    wifi_set_connection_parameters(
        ctx->networks[ctx->selected_network].ssid,
        ctx->networks[ctx->selected_network].secured ? ctx->password_buffer : ""
    );
    wifi_connection_status_t result = wifi_connect();

    switch (result) {
        case WIFI_CONNECTED:
            for (int i = 0; i < ctx->network_count; i++) {
                ctx->networks[i].connected = (i == ctx->selected_network);
            }
            break;

        case WIFI_ERROR_WRONG_CREDENTIALS:
            snprintf(ctx->connection_status_text, sizeof(ctx->connection_status_text), "Not connected, wrong password");
            break;

        case WIFI_ERROR:
        case WIFI_DISCONNECTED:
        default: snprintf(ctx->connection_status_text, sizeof(ctx->connection_status_text), "Connection failed"); break;
    }

    memset(ctx->password_buffer, 0, sizeof(ctx->password_buffer));
    ctx->password_cursor        = 0;
    ctx->show_connecting_dialog = false;

    ctx->current_screen = SCREEN_WIFI;

    ctx->network_count = 0;
}

static void handle_key_reorder(app_context *ctx, SDL_Event *event) {
    SDL_Keycode key = event->key.key;
    int ipp = ctx->reorder_items_per_page;

    /* ---- Text input for name dialogs ---- */
    if (event->type == SDL_EVENT_TEXT_INPUT) {
        if (ctx->reorder_dialog_type == DIALOG_NEW_FOLDER ||
            ctx->reorder_dialog_type == DIALOG_RENAME) {
            int len = (int)strlen(ctx->reorder_dialog_buf);
            if (len < 63) {
                strncat(ctx->reorder_dialog_buf, event->text.text, (size_t)(63 - len));
                ctx->reorder_dialog_cursor = (int)strlen(ctx->reorder_dialog_buf);
            }
        }
        return;
    }

    /* ---- Active dialog ---- */
    if (ctx->reorder_dialog_type != DIALOG_NONE) {
        if (key == SDLK_ESCAPE) {
            ctx->reorder_dialog_type = DIALOG_NONE;
            SDL_StopTextInput(ctx->window);
            return;
        }
        if (key == SDLK_BACKSPACE) {
            int len = (int)strlen(ctx->reorder_dialog_buf);
            if (len > 0) {
                ctx->reorder_dialog_buf[len - 1] = '\0';
                ctx->reorder_dialog_cursor = len - 1;
            }
            return;
        }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            switch (ctx->reorder_dialog_type) {

                case DIALOG_NEW_FOLDER: {
                    if (ctx->reorder_dialog_buf[0] != '\0') {
                        reorder_item_t *it = reorder_append(ctx);
                        if (it) {
                            it->is_folder = true;
                            strncpy(it->display_name, ctx->reorder_dialog_buf, 63);
                            it->display_name[63] = '\0';
                            reorder_save(ctx);
                        }
                    }
                    break;
                }

                case DIALOG_RENAME: {
                    if (ctx->reorder_dialog_buf[0] != '\0') {
                        strncpy(ctx->reorder_items[ctx->reorder_rename_idx].display_name,
                                ctx->reorder_dialog_buf, 63);
                        ctx->reorder_items[ctx->reorder_rename_idx].display_name[63] = '\0';
                        reorder_save(ctx);
                    }
                    break;
                }

                case DIALOG_DELETE_CONFIRM: {
                    int idx = ctx->reorder_rename_idx;
                    reorder_item_t *folder = &ctx->reorder_items[idx];

                    /* Copy folder app uids to temp array before any realloc */
                    int    n        = folder->folder_app_count;
                    char **app_uids = malloc((size_t)n * sizeof(char *));
                    for (int j = 0; j < n; j++) {
                        app_uids[j] = malloc(64);
                        if (app_uids[j]) {
                            strncpy(app_uids[j], folder->folder_apps[j], 63);
                            app_uids[j][63] = '\0';
                        }
                    }

                    /* Free folder's app list then remove folder from items */
                    for (int j = 0; j < folder->folder_app_count; j++)
                        free(folder->folder_apps[j]);
                    free(folder->folder_apps);
                    memmove(&ctx->reorder_items[idx], &ctx->reorder_items[idx + 1],
                            (size_t)(ctx->reorder_item_count - idx - 1) * sizeof(reorder_item_t));
                    ctx->reorder_item_count--;

                    /* Append former folder apps as home-screen entries */
                    for (int j = 0; j < n; j++) {
                        if (!app_uids[j]) continue;
                        reorder_item_t *new_it = reorder_append(ctx);
                        if (new_it) {
                            new_it->is_folder = false;
                            strncpy(new_it->uid, app_uids[j], 63);
                            new_it->uid[63] = '\0';
                            new_it->display_name[0] = '\0';
                            for (int k = 0; k < ctx->app_name_count; k++) {
                                if (strcmp(ctx->app_name_table[k].uid, app_uids[j]) == 0) {
                                    strncpy(new_it->display_name,
                                            ctx->app_name_table[k].name, 63);
                                    break;
                                }
                            }
                            if (!new_it->display_name[0])
                                strncpy(new_it->display_name, app_uids[j], 63);
                        }
                        free(app_uids[j]);
                    }
                    free(app_uids);

                    if (ctx->reorder_selected >= ctx->reorder_item_count &&
                        ctx->reorder_selected > 0)
                        ctx->reorder_selected--;
                    reorder_save(ctx);
                    break;
                }

                default: break;
            }
            ctx->reorder_dialog_type = DIALOG_NONE;
            SDL_StopTextInput(ctx->window);
        }
        return;
    }

    /* ---- Folder sub-view ---- */
    if (ctx->reorder_in_folder) {
        reorder_item_t *folder = &ctx->reorder_items[ctx->reorder_folder_idx];
        int total = folder->folder_app_count;

        if (key == SDLK_UP) {
            if (ctx->reorder_folder_sel > 0) {
                ctx->reorder_folder_sel--;
                if (ctx->reorder_folder_sel < ctx->reorder_folder_scroll)
                    ctx->reorder_folder_scroll = ctx->reorder_folder_sel;
            }
        } else if (key == SDLK_DOWN) {
            if (ctx->reorder_folder_sel < total - 1) {
                ctx->reorder_folder_sel++;
                if (ctx->reorder_folder_sel >= ctx->reorder_folder_scroll + ipp)
                    ctx->reorder_folder_scroll = ctx->reorder_folder_sel - ipp + 1;
            }
        } else if (key == SDLK_BACKSPACE) {
            if (total > 0) {
                int sel_f = ctx->reorder_folder_sel;
                char uid[64];
                strncpy(uid, folder->folder_apps[sel_f], 63); uid[63] = '\0';

                /* Remove app from folder */
                free(folder->folder_apps[sel_f]);
                memmove(&folder->folder_apps[sel_f], &folder->folder_apps[sel_f + 1],
                        (size_t)(folder->folder_app_count - sel_f - 1) * sizeof(char *));
                folder->folder_app_count--;

                /* Append as home entry (may realloc; re-fetch folder pointer after) */
                reorder_item_t *new_it = reorder_append(ctx);
                folder = &ctx->reorder_items[ctx->reorder_folder_idx]; /* re-fetch */
                if (new_it) {
                    new_it->is_folder = false;
                    strncpy(new_it->uid, uid, 63); new_it->uid[63] = '\0';
                    new_it->display_name[0] = '\0';
                    for (int k = 0; k < ctx->app_name_count; k++) {
                        if (strcmp(ctx->app_name_table[k].uid, uid) == 0) {
                            strncpy(new_it->display_name, ctx->app_name_table[k].name, 63);
                            break;
                        }
                    }
                    if (!new_it->display_name[0]) strncpy(new_it->display_name, uid, 63);
                }

                if (ctx->reorder_folder_sel >= folder->folder_app_count &&
                    ctx->reorder_folder_sel > 0)
                    ctx->reorder_folder_sel--;
                reorder_save(ctx);
            }
        } else if (key == SDLK_ESCAPE) {
            ctx->reorder_in_folder = false;
        }
        return;
    }

    /* ---- Home view ---- */
    int total = ctx->reorder_item_count;
    int sel   = ctx->reorder_selected;
    int held  = ctx->reorder_held;

    if (key == SDLK_UP) {
        if (sel > 0) {
            ctx->reorder_selected--;
            if (ctx->reorder_selected < ctx->reorder_scroll)
                ctx->reorder_scroll = ctx->reorder_selected;
        }
    } else if (key == SDLK_DOWN) {
        if (sel < total - 1) {
            ctx->reorder_selected++;
            if (ctx->reorder_selected >= ctx->reorder_scroll + ipp)
                ctx->reorder_scroll = ctx->reorder_selected - ipp + 1;
        }
    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        if (held >= 0) {
            int H = held, D = sel;
            if (D == H) {
                /* Drop in place */
                ctx->reorder_held = -1;
                reorder_save(ctx);
            } else if (total > 0 && ctx->reorder_items[D].is_folder) {
                /* Move held app into folder */
                char held_uid[64];
                strncpy(held_uid, ctx->reorder_items[H].uid, 63); held_uid[63] = '\0';
                memmove(&ctx->reorder_items[H], &ctx->reorder_items[H + 1],
                        (size_t)(ctx->reorder_item_count - H - 1) * sizeof(reorder_item_t));
                ctx->reorder_item_count--;
                int fidx = D > H ? D - 1 : D;
                reorder_folder_append(&ctx->reorder_items[fidx], held_uid);
                if (ctx->reorder_selected >= ctx->reorder_item_count && ctx->reorder_selected > 0)
                    ctx->reorder_selected--;
                ctx->reorder_held = -1;
                reorder_save(ctx);
            } else {
                /* Reorder: move item from H to D */
                reorder_item_t held_item = ctx->reorder_items[H];
                memmove(&ctx->reorder_items[H], &ctx->reorder_items[H + 1],
                        (size_t)(ctx->reorder_item_count - H - 1) * sizeof(reorder_item_t));
                ctx->reorder_item_count--;
                int insert_at = D > H ? D - 1 : D;
                memmove(&ctx->reorder_items[insert_at + 1], &ctx->reorder_items[insert_at],
                        (size_t)(ctx->reorder_item_count - insert_at) * sizeof(reorder_item_t));
                ctx->reorder_items[insert_at] = held_item;
                ctx->reorder_item_count++;
                ctx->reorder_selected = insert_at;
                ctx->reorder_held     = -1;
                reorder_save(ctx);
            }
        } else {
            if (total > 0) {
                if (ctx->reorder_items[sel].is_folder) {
                    ctx->reorder_in_folder     = true;
                    ctx->reorder_folder_idx    = sel;
                    ctx->reorder_folder_sel    = 0;
                    ctx->reorder_folder_scroll = 0;
                } else {
                    ctx->reorder_held        = sel;
                    ctx->reorder_held_origin = sel;
                }
            }
        }
    } else if (key == SDLK_N) {
        if (held < 0) {
            ctx->reorder_dialog_type   = DIALOG_NEW_FOLDER;
            ctx->reorder_dialog_buf[0] = '\0';
            ctx->reorder_dialog_cursor = 0;
            SDL_FlushEvent(SDL_EVENT_TEXT_INPUT);
        }
    } else if (key == SDLK_R) {
        if (held < 0 && total > 0 && ctx->reorder_items[sel].is_folder) {
            ctx->reorder_dialog_type = DIALOG_RENAME;
            ctx->reorder_rename_idx  = sel;
            strncpy(ctx->reorder_dialog_buf, ctx->reorder_items[sel].display_name, 63);
            ctx->reorder_dialog_buf[63] = '\0';
            ctx->reorder_dialog_cursor  = (int)strlen(ctx->reorder_dialog_buf);
            SDL_FlushEvent(SDL_EVENT_TEXT_INPUT);
        }
    } else if (key == SDLK_BACKSPACE) {
        if (held < 0 && total > 0 && ctx->reorder_items[sel].is_folder) {
            ctx->reorder_dialog_type = DIALOG_DELETE_CONFIRM;
            ctx->reorder_rename_idx  = sel;
        }
    } else if (key == SDLK_ESCAPE) {
        if (held >= 0) {
            /* Cancel grab — restore item to its original position */
            int H      = held;
            int origin = ctx->reorder_held_origin;
            if (H != origin) {
                reorder_item_t held_item = ctx->reorder_items[H];
                memmove(&ctx->reorder_items[H], &ctx->reorder_items[H + 1],
                        (size_t)(ctx->reorder_item_count - H - 1) * sizeof(reorder_item_t));
                ctx->reorder_item_count--;
                int insert_at = origin > H ? origin - 1 : origin;
                memmove(&ctx->reorder_items[insert_at + 1], &ctx->reorder_items[insert_at],
                        (size_t)(ctx->reorder_item_count - insert_at) * sizeof(reorder_item_t));
                ctx->reorder_items[insert_at] = held_item;
                ctx->reorder_item_count++;
                ctx->reorder_selected = insert_at;
            }
            ctx->reorder_held = -1;
        } else {
            reorder_free(ctx);
            nav_pop(ctx);
        }
    }
}

static void handle_key_app_chooser(app_context *ctx, SDL_Keycode key) {
    if (ctx->chooser_app_count == 0) {
        if (key == SDLK_ESCAPE || key == SDLK_RETURN || key == SDLK_KP_ENTER)
            app_chooser_close(ctx);
        return;
    }

    if (key == SDLK_UP) {
        if (ctx->chooser_selected > 0) {
            ctx->chooser_selected--;
            if (ctx->chooser_selected < ctx->chooser_scroll)
                ctx->chooser_scroll = ctx->chooser_selected;
        }
    } else if (key == SDLK_DOWN) {
        if (ctx->chooser_selected < ctx->chooser_app_count - 1) {
            ctx->chooser_selected++;
            if (ctx->chooser_selected >= ctx->chooser_scroll + ctx->chooser_items_per_page)
                ctx->chooser_scroll = ctx->chooser_selected - ctx->chooser_items_per_page + 1;
        }
    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        /* Save selection */
        strncpy(ctx->launcher_default_uid,
                ctx->chooser_apps[ctx->chooser_selected].uid,
                sizeof(ctx->launcher_default_uid) - 1);
        ctx->launcher_default_uid[sizeof(ctx->launcher_default_uid) - 1] = '\0';
        strncpy(ctx->launcher_default_name,
                ctx->chooser_apps[ctx->chooser_selected].name,
                sizeof(ctx->launcher_default_name) - 1);
        ctx->launcher_default_name[sizeof(ctx->launcher_default_name) - 1] = '\0';
        launcher_config_save(ctx);
        app_chooser_close(ctx);
    } else if (key == SDLK_ESCAPE) {
        app_chooser_close(ctx);
    }
}

static void handle_key_event(app_context *ctx, SDL_Event *event) {
    SDL_Keycode key = event->key.key;

    if (ctx->show_app_chooser) {
        handle_key_app_chooser(ctx, key);
        return;
    }

    if (ctx->show_password_dialog) {
        if (key == SDLK_ESCAPE) {
            ctx->show_password_dialog = false;
            memset(ctx->password_buffer, 0, sizeof(ctx->password_buffer));
            ctx->password_cursor = 0;
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            attempt_wifi_connection(ctx);
        } else if (key == SDLK_TAB) {
            ctx->show_password = !ctx->show_password;
        } else if (key == SDLK_BACKSPACE) {
            if (ctx->password_cursor > 0) {
                ctx->password_cursor--;
                ctx->password_buffer[ctx->password_cursor] = '\0';
            }
        } else if (event->type == SDL_EVENT_TEXT_INPUT) {
            if (ctx->password_cursor < 127) {
                strcat(ctx->password_buffer, event->text.text);
                ctx->password_cursor = strlen(ctx->password_buffer);
            }
        }
        return;
    }

    switch (ctx->current_screen) {
        case SCREEN_MAIN:
            if (key == SDLK_UP) {
                if (ctx->selected_item > 0)
                    ctx->selected_item--;
            } else if (key == SDLK_DOWN) {
                if (ctx->selected_item < ctx->total_items - 1)
                    ctx->selected_item++;
            } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                switch (ctx->selected_item) {
                    case 0: nav_push(ctx, SCREEN_WIFI); break;
                    case 1:
                        reorder_init(ctx);
                        nav_push(ctx, SCREEN_REORDER);
                        break;
                    case 2: /* Launch default app: toggle bool and save */
                        ctx->launch_default_app = !ctx->launch_default_app;
                        launcher_config_save(ctx);
                        break;
                    case 3: /* Default app: open chooser */
                        app_chooser_open(ctx);
                        break;
                    case 4: nav_push(ctx, SCREEN_ABOUT); break;
                }
            } else if (key == SDLK_ESCAPE) {
                nav_pop(ctx);
            }
            break;

        case SCREEN_WIFI:
            if (key == SDLK_UP) {
                if (ctx->selected_item > 0) {
                    ctx->selected_item--;
                    if (ctx->selected_item < ctx->scroll_offset) {
                        ctx->scroll_offset = ctx->selected_item;
                    }
                }
            } else if (key == SDLK_DOWN) {
                if (ctx->selected_item < ctx->network_count - 1) {
                    ctx->selected_item++;
                    if (ctx->selected_item >= ctx->scroll_offset + ctx->items_per_page) {
                        ctx->scroll_offset = ctx->selected_item - ctx->items_per_page + 1;
                    }
                }
            } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                ctx->selected_network = ctx->selected_item;
                if (ctx->networks[ctx->selected_item].secured) {
                    ctx->show_password_dialog = true;
                    memset(ctx->password_buffer, 0, sizeof(ctx->password_buffer));
                    ctx->password_cursor = 0;
                    ctx->show_password   = false;
                } else {
                    attempt_wifi_connection(ctx);
                }
            } else if (key == SDLK_S) {
                ctx->network_count = 0;
            } else if (key == SDLK_ESCAPE) {
                nav_pop(ctx);
            }
            break;

        case SCREEN_ABOUT:
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_ESCAPE) {
                nav_pop(ctx);
            }
            break;

        case SCREEN_REORDER:
            handle_key_reorder(ctx, event);
            break;

        default:
            if (key == SDLK_ESCAPE) {
                nav_pop(ctx);
            }
            break;
    }
}

static void render_screen(app_context *ctx) {
    memset(ctx->pixels, 0, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));

    switch (ctx->current_screen) {
        case SCREEN_MAIN:
            draw_main_settings(ctx);
            if (ctx->show_app_chooser)
                draw_app_chooser_dialog(ctx);
            break;
        case SCREEN_WIFI:
            draw_wifi_settings(ctx);
            if (ctx->show_password_dialog) {
                draw_password_dialog(ctx);
            }
            if (ctx->show_connecting_dialog) {
                draw_connecting_screen(ctx);
            }
            break;
        case SCREEN_DISPLAY: draw_main_settings(ctx); break;
        case SCREEN_SYSTEM: draw_main_settings(ctx); break;
        case SCREEN_ABOUT:
            draw_main_settings(ctx);
            draw_about_dialog(ctx);
            break;
        case SCREEN_REORDER: draw_reorder_screen(ctx); break;
    }

    SDL_UpdateTexture(ctx->texture, NULL, ctx->pixels, SCREEN_WIDTH * sizeof(uint16_t));
    SDL_RenderClear(ctx->renderer);
    SDL_RenderTexture(ctx->renderer, ctx->texture, NULL, NULL);
    SDL_RenderPresent(ctx->renderer);
}

int main(int argc, char *argv[]) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    app_context ctx = {0};

    ctx.window = SDL_CreateWindow("Settings App - CDE Style", SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_FULLSCREEN);
    if (!ctx.window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    ctx.renderer = SDL_CreateRenderer(ctx.window, NULL);
    if (!ctx.renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(ctx.window);
        SDL_Quit();
        return 1;
    }

    ctx.texture = SDL_CreateTexture(
        ctx.renderer,
        SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING,
        SCREEN_WIDTH,
        SCREEN_HEIGHT
    );
    if (!ctx.texture) {
        SDL_Log("SDL_CreateTexture failed: %s", SDL_GetError());
        SDL_DestroyRenderer(ctx.renderer);
        SDL_DestroyWindow(ctx.window);
        SDL_Quit();
        return 1;
    }

    ctx.pixels = calloc(SCREEN_WIDTH * SCREEN_HEIGHT, sizeof(uint16_t));
    if (!ctx.pixels) {
        SDL_Log("Failed to allocate pixel buffer");
        SDL_DestroyTexture(ctx.texture);
        SDL_DestroyRenderer(ctx.renderer);
        SDL_DestroyWindow(ctx.window);
        SDL_Quit();
        return 1;
    }

    ctx.current_screen = SCREEN_MAIN;
    ctx.selected_item  = 0;
    ctx.scroll_offset  = 0;
    ctx.nav_depth      = 0;
    launcher_config_load(&ctx);

    SDL_StartTextInput(ctx.window);

    bool      running = true;
    SDL_Event event;

    while (running) {
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_KEY_DOWN: handle_key_event(&ctx, &event); break;
                case SDL_EVENT_TEXT_INPUT:
                    if (ctx.show_password_dialog) {
                        handle_key_event(&ctx, &event);
                    } else if (ctx.current_screen == SCREEN_REORDER &&
                               ctx.reorder_dialog_type != DIALOG_NONE &&
                               ctx.reorder_dialog_type != DIALOG_DELETE_CONFIRM) {
                        handle_key_reorder(&ctx, &event);
                    }
                    break;
            }
        }

        render_screen(&ctx);

        SDL_Delay(16); // ~60 FPS
    }

    SDL_StopTextInput(ctx.window);
    free(ctx.pixels);
    free(ctx.networks);
    free(ctx.chooser_apps);
    SDL_DestroyTexture(ctx.texture);
    SDL_DestroyRenderer(ctx.renderer);
    SDL_DestroyWindow(ctx.window);
    SDL_Quit();

    return 0;
}
