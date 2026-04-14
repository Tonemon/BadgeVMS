#include "font.h"

#include <stdio.h>
#include <stdlib.h>

#include <badgevms/application.h>
#include <badgevms/compositor.h>
#include <badgevms/event.h>
#include <badgevms/keyboard.h>
#include <string.h>

#include <badgevms/process.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include "stb_image.h"
#include "cJSON.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

/* ---- Folder support types ---- */

typedef enum {
    ITEM_APP,
    ITEM_FOLDER,
} launcher_item_type_t;

typedef struct {
    launcher_item_type_t type;
    union {
        application_t *app;          /* type == ITEM_APP    */
        struct {
            char name[64];           /* folder display name */
            int  app_count;          /* installed apps in this folder */
        } folder;                    /* type == ITEM_FOLDER */
    };
} launcher_item_t;

/* --- Boot screen scan thread state --- */
static atomic_bool     g_scan_done  = false;
static application_t **g_scan_apps  = NULL;
static size_t          g_scan_count = 0;

static void scan_thread(void *unused) {
    (void)unused;

    application_t          *app;
    application_list_handle handle = application_list(&app);
    /* handle is intentionally not closed: the application_t* pointers in
     * g_scan_apps are owned by this handle and must stay valid for run_launcher(). */

    printf("Scan thread: scanning installed applications\n");

    size_t          count = 0;
    application_t **apps  = NULL;

    while (app) {
        printf("  Name: %s  UID: %s  Binary: %s\n",
               app->name, app->unique_identifier, app->binary_path);

        if (app->binary_path && strlen(app->binary_path) &&
            app->unique_identifier &&
            strcmp(app->unique_identifier, "badgevms_launcher")        != 0 &&
            strcmp(app->unique_identifier, "why2025_firmware_ota_c6") != 0) {
            application_t **tmp = realloc(apps, sizeof(application_t *) * (count + 1));
            if (!tmp) {
                printf("Scan thread: OOM, stopping after %zu apps\n", count);
                break;
            }
            apps          = tmp;
            apps[count++] = app;
        }
        app = application_list_get_next(handle);
    }

    printf("Scan thread: found %zu launchable apps\n", count);

    g_scan_apps  = apps;
    g_scan_count = count;
    /* seq-cst store: reader must use atomic_load(&g_scan_done) with acquire
     * semantics (the default) before reading g_scan_apps / g_scan_count. */
    atomic_store(&g_scan_done, true);
}

typedef struct {
    window_handle_t window;
    framebuffer_t  *framebuffer;
    uint16_t       *pixels;
    /* Full scanned app list (owned by scan_thread's list handle) */
    application_t **applications;
    size_t          num_apps;
    /* Current-view navigation */
    int             scroll_offset;
    int             selected_item;
    int             total_items;      /* length of the active list */
    int             items_per_page;
    bool            show_about;
    bool            quit;
    /* Home-screen item list */
    launcher_item_t *items;
    int              item_count;
    int              folder_start_index; /* index in items[] where folder rows begin; -1 if none */
    /* Folder view state */
    char            *current_folder;     /* NULL = home; pointer into items[i].folder.name */
    application_t  **folder_apps;
    int              folder_app_count;
    /* Saved home navigation for restore on leave */
    int              saved_home_selected;
    int              saved_home_scroll;
    /* Parsed folders.json kept alive for folder lookups */
    cJSON           *folders_json;
} Launcher_Context;

static inline uint16_t rgb888_to_rgb565_color(uint32_t rgb888) {
    uint8_t r = (rgb888 >> 16) & 0xFF;
    uint8_t g = (rgb888 >> 8) & 0xFF;
    uint8_t b = rgb888 & 0xFF;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static void draw_rect(Launcher_Context *ctx, int x, int y, int w, int h, uint32_t color) {
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

static void draw_char(Launcher_Context *ctx, int x, int y, char c, uint32_t color) {
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

static void draw_text(Launcher_Context *ctx, int x, int y, char const *text, uint32_t color) {
    int current_x = x;
    while (*text) {
        draw_char(ctx, current_x, y, *text, color);
        current_x += FONT_WIDTH;
        text++;
    }
}

static void draw_text_bold(Launcher_Context *ctx, int x, int y, char const *text, uint32_t color) {
    draw_text(ctx, x, y, text, color);
    draw_text(ctx, x + 1, y, text, color);
}

static int get_text_width(char const *text) {
    return strlen(text) * FONT_WIDTH;
}

static void draw_text_centered(Launcher_Context *ctx, int x, int y, int width, char const *text, uint32_t color) {
    int text_w = get_text_width(text);
    int text_x = x + (width - text_w) / 2;
    draw_text(ctx, text_x, y, text, color);
}

static void draw_3d_border(Launcher_Context *ctx, int x, int y, int w, int h, int inset) {
    uint32_t light_color = inset ? CDE_BORDER_DARK : CDE_BORDER_LIGHT;
    uint32_t dark_color  = inset ? CDE_BORDER_LIGHT : CDE_BORDER_DARK;

    draw_rect(ctx, x, y, w, 2, light_color);
    draw_rect(ctx, x, y, 2, h, light_color);

    draw_rect(ctx, x, y + h - 2, w, 2, dark_color);
    draw_rect(ctx, x + w - 2, y, 2, h, dark_color);
}

static void draw_button(Launcher_Context *ctx, int x, int y, int w, int h, char const *text, int pressed) {
    draw_rect(ctx, x, y, w, h, CDE_BUTTON_COLOR);
    draw_3d_border(ctx, x, y, w, h, pressed);

    int text_y = y + (h - FONT_HEIGHT) / 2;
    if (pressed) {
        text_y += 1;
    }
    draw_text_centered(ctx, x, text_y, w, text, CDE_TEXT_COLOR);
}

static void draw_about_dialog(Launcher_Context *ctx) {
    int dialog_w = 450;
    int dialog_h = 350;
    int dialog_x = (SCREEN_WIDTH - dialog_w) / 2;
    int dialog_y = (SCREEN_HEIGHT - dialog_h) / 2;

    draw_rect(ctx, dialog_x + 5, dialog_y + 5, dialog_w, dialog_h, 0x505050);

    draw_rect(ctx, dialog_x, dialog_y, dialog_w, dialog_h, CDE_PANEL_COLOR);
    draw_3d_border(ctx, dialog_x, dialog_y, dialog_w, dialog_h, 0);

    int title_h = 30;
    draw_rect(ctx, dialog_x + 2, dialog_y + 2, dialog_w - 4, title_h, CDE_TITLE_BG);
    draw_text_bold(ctx, dialog_x + 10, dialog_y + 8, "About BadgeVMS", CDE_SELECTED_TEXT);

    int content_y = dialog_y + title_h + 30;
    draw_text_centered(ctx, dialog_x, content_y, dialog_w, "BadgeVMS", CDE_TEXT_COLOR);
    draw_text_centered(ctx, dialog_x, content_y + 30, dialog_w, "Version 1.0", CDE_TEXT_COLOR);
    draw_text_centered(
        ctx,
        dialog_x,
        content_y + 60,
        dialog_w,
        "A Virtual Memory System for badges",
        CDE_INACTIVE_TEXT
    );

    draw_text_centered(ctx, dialog_x, content_y + 120, dialog_w, "Press ENTER or ESC to close", CDE_INACTIVE_TEXT);
}

static void draw_launcher_window(Launcher_Context *ctx) {
    int window_x = 30;
    int window_y = 30;
    int window_w = SCREEN_WIDTH - 60;
    int window_h = SCREEN_HEIGHT - 60;

    draw_rect(ctx, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, CDE_BG_COLOR);

    draw_rect(ctx, window_x, window_y, window_w, window_h, CDE_PANEL_COLOR);
    draw_3d_border(ctx, window_x, window_y, window_w, window_h, 0);

    int title_h = 45;
    draw_rect(ctx, window_x + 3, window_y + 3, window_w - 6, title_h, CDE_TITLE_BG);
    draw_text_bold(ctx, window_x + 15, window_y + 11, "WHY Application Launcher", CDE_SELECTED_TEXT);

    char count_text[64];
    if (ctx->total_items == 1) {
        snprintf(count_text, sizeof(count_text), "1 Application Available");
    } else {
        snprintf(count_text, sizeof(count_text), "%d Applications Available", ctx->total_items);
    }
    draw_text(ctx, window_x + 15, window_y + title_h + 20, count_text, CDE_TEXT_COLOR);

    int list_y      = window_y + title_h + 55;
    int list_h      = window_h - title_h - 110;
    int item_height = 80;

    draw_rect(ctx, window_x + 15, list_y, window_w - 30, list_h, 0xFFFFFF);
    draw_3d_border(ctx, window_x + 15, list_y, window_w - 30, list_h, 1);

    ctx->items_per_page = (list_h - 6) / item_height;
    int visible_start   = ctx->scroll_offset;
    int visible_end     = visible_start + ctx->items_per_page;
    if (visible_end > ctx->total_items)
        visible_end = ctx->total_items;

    for (int i = visible_start; i < visible_end; i++) {
        int item_y = list_y + 3 + (i - visible_start) * item_height;
        int item_x = window_x + 18;
        int item_w = window_w - 36;

        if (i == ctx->selected_item) {
            draw_rect(ctx, item_x, item_y, item_w, item_height - 2, CDE_SELECTED_BG);
        }

        uint32_t text_color = (i == ctx->selected_item) ? CDE_SELECTED_TEXT : CDE_TEXT_COLOR;

        int icon_size = 48;
        int icon_x    = item_x + 10;
        int icon_y    = item_y + (item_height - icon_size) / 2;

        uint32_t icon_color = (i == ctx->selected_item) ? CDE_SELECTED_TEXT : CDE_BUTTON_COLOR;
        draw_rect(ctx, icon_x, icon_y, icon_size, icon_size, icon_color);
        draw_3d_border(ctx, icon_x, icon_y, icon_size, icon_size, 1);

        int text_x = icon_x + icon_size + 15;
        draw_text_bold(ctx, text_x, item_y + 10, ctx->applications[i]->name, text_color);

        if (ctx->applications[i]->version) {
            char version_text[64];
            snprintf(version_text, sizeof(version_text), "v%s", ctx->applications[i]->version);
            draw_text(ctx, text_x, item_y + 35, version_text, text_color);
        }

#if 0
        if (ctx->applications[i].description) {
            char desc[60] = {0};
            int max_desc_chars = ((item_w - text_x + item_x - 16) / FONT_WIDTH);
            if (max_desc_chars > 59) max_desc_chars = 59;
            
            strncpy(desc, ctx->applications[i].description, max_desc_chars);
            desc[max_desc_chars] = '\0';
            
            if (strlen(ctx->applications[i].description) > max_desc_chars) {
                desc[max_desc_chars - 3] = '.';
                desc[max_desc_chars - 2] = '.';
                desc[max_desc_chars - 1] = '.';
            }
            draw_text(ctx, text_x, item_y + 58, desc, text_color);
        }
#endif

        if (i < visible_end - 1) {
            draw_rect(ctx, item_x, item_y + item_height - 2, item_w, 1, CDE_BORDER_DARK);
        }
    }

    if (ctx->total_items > ctx->items_per_page) {
        int scrollbar_x = window_x + window_w - 35;
        int scrollbar_y = list_y + 3;
        int scrollbar_h = list_h - 6;

        draw_rect(ctx, scrollbar_x, scrollbar_y, 20, scrollbar_h, CDE_BUTTON_COLOR);
        draw_3d_border(ctx, scrollbar_x, scrollbar_y, 20, scrollbar_h, 1);

        int thumb_h = (scrollbar_h * ctx->items_per_page) / ctx->total_items;
        if (thumb_h < 30)
            thumb_h = 30;

        int thumb_y = scrollbar_y;
        if (ctx->total_items > ctx->items_per_page) {
            thumb_y += ((scrollbar_h - thumb_h) * ctx->scroll_offset) / (ctx->total_items - ctx->items_per_page);
        }

        draw_rect(ctx, scrollbar_x + 3, thumb_y, 14, thumb_h, CDE_PANEL_COLOR);
        draw_3d_border(ctx, scrollbar_x + 3, thumb_y, 14, thumb_h, 0);
    }

    draw_rect(ctx, window_x + 3, window_y + window_h - 42, window_w - 6, 39, CDE_BUTTON_COLOR);
    draw_3d_border(ctx, window_x + 3, window_y + window_h - 42, window_w - 6, 39, 1);

    draw_text(
        ctx,
        window_x + 15,
        window_y + window_h - 35,
        "UP/DOWN: Navigate  ENTER: Launch  A: About  ESC: Exit",
        CDE_TEXT_COLOR
    );
}

/* enter_folder — switch from home view into a named folder's app list */
static void enter_folder(Launcher_Context *ctx, const char *folder_name) {
    /* Save home navigation state */
    ctx->saved_home_selected = ctx->selected_item;
    ctx->saved_home_scroll   = ctx->scroll_offset;

    /* Collect apps that belong to this folder (JSON order) */
    ctx->folder_apps      = NULL;
    ctx->folder_app_count = 0;

    cJSON *folders_arr = ctx->folders_json
        ? cJSON_GetObjectItem(ctx->folders_json, "folders") : NULL;

    if (folders_arr) {
        cJSON *folder_obj;
        cJSON_ArrayForEach(folder_obj, folders_arr) {
            cJSON *fname = cJSON_GetObjectItem(folder_obj, "name");
            if (!fname) continue;
            if (strcmp(cJSON_GetStringValue(fname), folder_name) != 0) continue;

            cJSON *uids = cJSON_GetObjectItem(folder_obj, "apps");
            if (!uids) break;

            int n = cJSON_GetArraySize(uids);
            ctx->folder_apps = malloc((size_t)n * sizeof(application_t *));
            if (!ctx->folder_apps) break;

            cJSON *uid_item;
            cJSON_ArrayForEach(uid_item, uids) {
                const char *uid = cJSON_GetStringValue(uid_item);
                if (!uid) continue;
                for (size_t i = 0; i < ctx->num_apps; i++) {
                    if (strcmp(ctx->applications[i]->unique_identifier, uid) == 0) {
                        ctx->folder_apps[ctx->folder_app_count++] = ctx->applications[i];
                        break;
                    }
                }
            }
            break; /* found the folder */
        }
    }

    ctx->current_folder = (char *)folder_name; /* points into items[].folder.name — stable lifetime, not owned, do not free */
    ctx->selected_item  = 0;
    ctx->scroll_offset  = 0;
    ctx->total_items    = ctx->folder_app_count;
}

/* leave_folder — return from folder view to home screen */
static void leave_folder(Launcher_Context *ctx) {
    free(ctx->folder_apps);
    ctx->folder_apps      = NULL;
    ctx->folder_app_count = 0;
    ctx->current_folder   = NULL;
    ctx->selected_item    = ctx->saved_home_selected;
    ctx->scroll_offset    = ctx->saved_home_scroll;
    ctx->total_items      = ctx->item_count;
}

static void handle_keyboard(Launcher_Context *ctx, keyboard_scancode_t key_code) {
    if (ctx->show_about) {
        if (key_code == KEY_SCANCODE_ESCAPE || key_code == KEY_SCANCODE_RETURN || key_code == KEY_SCANCODE_SPACE) {
            ctx->show_about = false;
        }
        return;
    }

    switch (key_code) {
        case KEY_SCANCODE_UP:
            if (ctx->selected_item > 0) {
                ctx->selected_item--;
                if (ctx->selected_item < ctx->scroll_offset) {
                    ctx->scroll_offset = ctx->selected_item;
                }
            }
            break;

        case KEY_SCANCODE_DOWN:
            if (ctx->selected_item < ctx->total_items - 1) {
                ctx->selected_item++;
                if (ctx->selected_item >= ctx->scroll_offset + ctx->items_per_page) {
                    ctx->scroll_offset = ctx->selected_item - ctx->items_per_page + 1;
                }
            }
            break;

        case KEY_SCANCODE_RETURN:
        case KEY_SCANCODE_SPACE:
            printf("Launching: %s\n", ctx->applications[ctx->selected_item]->name);
            application_launch(ctx->applications[ctx->selected_item]->unique_identifier);
            break;

        case KEY_SCANCODE_A: ctx->show_about = true; break;

        case KEY_SCANCODE_ESCAPE: {
            ctx->quit = true;
        } break;
    }
}

static void draw_boot_screen(
    Launcher_Context *ctx,
    unsigned char    *logo_data,
    int               logo_w,
    int               logo_h,
    int               logo_ch,
    float             bright,
    int               dot_count
) {
    if (bright > 1.0f) bright = 1.0f;
    if (bright < 0.0f) bright = 0.0f;

    /* Clear to black */
    memset(ctx->pixels, 0, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));

    /* Draw logo centred horizontally, slightly above vertical centre */
    int dest_x = (SCREEN_WIDTH  - logo_w) / 2;
    int dest_y = (SCREEN_HEIGHT - logo_h) / 2 - 40;

    if (logo_data && logo_ch >= 3) {

        for (int y = 0; y < logo_h; y++) {
            for (int x = 0; x < logo_w; x++) {
                int     idx = (y * logo_w + x) * logo_ch;
                uint8_t a   = (logo_ch == 4) ? logo_data[idx + 3] : 255;
                if (a < 128)
                    continue;
                uint8_t r = (uint8_t)(logo_data[idx]     * bright);
                uint8_t g = (uint8_t)(logo_data[idx + 1] * bright);
                uint8_t b = (uint8_t)(logo_data[idx + 2] * bright);
                int px = dest_x + x;
                int py = dest_y + y;
                if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT)
                    ctx->pixels[py * SCREEN_WIDTH + px] =
                        rgb888_to_rgb565_color(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
            }
        }
    }

    /* Draw animated status text below the logo */
    static char const *dots[] = {"", ".", "..", "..."};
    char status[48];
    snprintf(status, sizeof(status), "Initializing WHY2025 badge%s", dots[dot_count & 3]);

    int logo_top    = (logo_data && logo_ch >= 3) ? dest_y : SCREEN_HEIGHT / 2 - 40;
    int logo_bottom = logo_top + ((logo_data && logo_ch >= 3) ? logo_h : 0);
    int text_y      = logo_bottom + 32;

    draw_text_centered(ctx, 0, text_y, SCREEN_WIDTH, status, 0xAAAAAA);
}

/*
 * build_item_list — parse folders.json and assemble ctx->items[].
 *
 * Home-screen layout:
 *   [0 .. folder_start_index-1]  ITEM_APP  — apps not assigned to any folder
 *   [folder_start_index .. item_count-1]  ITEM_FOLDER — non-empty folders
 *
 * If folders.json is missing or malformed, all apps appear as ITEM_APP rows
 * and folder_start_index is set to -1.
 */
static void build_item_list(Launcher_Context *ctx) {
    application_t **apps     = ctx->applications;
    size_t          num_apps = ctx->num_apps;

    /* --- Read and parse folders.json --- */
    FILE *f = fopen("APPS:[badgevms_launcher]/folders.json", "r");
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
    if (!root) {
        printf("build_item_list: folders.json missing or invalid; all apps shown unassigned\n");
    }
    ctx->folders_json = root;

    cJSON *folders_arr = root ? cJSON_GetObjectItem(root, "folders") : NULL;

    /* --- Determine folder assignment per app (index into folders_arr) ---
     * app_folder_name[i] points into the live cJSON string; NULL = unassigned. */
    char const **app_folder_name = calloc(num_apps, sizeof(char *));
    /* OOM: app_folder_name stays NULL → all apps treated as unassigned */

    if (folders_arr && app_folder_name) {
        cJSON *folder_obj;
        cJSON_ArrayForEach(folder_obj, folders_arr) {
            cJSON *fname = cJSON_GetObjectItem(folder_obj, "name");
            cJSON *uids  = cJSON_GetObjectItem(folder_obj, "apps");
            if (!fname || !uids) continue;
            char const *folder_name = cJSON_GetStringValue(fname);

            cJSON *uid_item;
            cJSON_ArrayForEach(uid_item, uids) {
                char const *uid = cJSON_GetStringValue(uid_item);
                if (!uid) continue;
                for (size_t i = 0; i < num_apps; i++) {
                    if (strcmp(apps[i]->unique_identifier, uid) == 0) {
                        app_folder_name[i] = folder_name;
                        break;
                    }
                }
            }
        }
    }

    /* --- Allocate items array (worst case: all apps + all folder entries) --- */
    int max_folders = folders_arr ? cJSON_GetArraySize(folders_arr) : 0;
    ctx->items = malloc((num_apps + (size_t)max_folders) * sizeof(launcher_item_t));
    if (!ctx->items) {
        free(app_folder_name);
        ctx->item_count          = 0;
        ctx->folder_start_index  = -1;
        ctx->total_items         = 0;
        return;
    }
    ctx->item_count = 0;

    /* --- Unassigned apps first --- */
    for (size_t i = 0; i < num_apps; i++) {
        if (!app_folder_name || !app_folder_name[i]) {
            launcher_item_t *it = &ctx->items[ctx->item_count++];
            it->type    = ITEM_APP;
            it->app     = apps[i];
        }
    }

    /* --- Record where folder rows start --- */
    ctx->folder_start_index = ctx->item_count; /* tentative */

    /* --- Folder rows (non-empty folders only) --- */
    if (folders_arr) {
        cJSON *folder_obj;
        cJSON_ArrayForEach(folder_obj, folders_arr) {
            cJSON *fname = cJSON_GetObjectItem(folder_obj, "name");
            cJSON *uids  = cJSON_GetObjectItem(folder_obj, "apps");
            if (!fname || !uids) continue;
            char const *folder_name = cJSON_GetStringValue(fname);

            int installed = 0;
            for (size_t i = 0; i < num_apps; i++) {
                if (app_folder_name && app_folder_name[i] &&
                    strcmp(app_folder_name[i], folder_name) == 0) {
                    installed++;
                }
            }
            if (installed == 0) continue; /* hidden: no apps installed */

            launcher_item_t *it = &ctx->items[ctx->item_count++];
            it->type = ITEM_FOLDER;
            strncpy(it->folder.name, folder_name, sizeof(it->folder.name) - 1);
            it->folder.name[sizeof(it->folder.name) - 1] = '\0';
            it->folder.app_count = installed;
        }
    }

    free(app_folder_name);

    /* If no folders were added, clear the marker */
    if (ctx->folder_start_index == ctx->item_count) {
        ctx->folder_start_index = -1;
    }

    ctx->total_items = ctx->item_count;
    printf("build_item_list: %d home items (%d folders)\n",
           ctx->item_count,
           ctx->folder_start_index >= 0 ? ctx->item_count - ctx->folder_start_index : 0);
}

static bool run_launcher(
    window_handle_t window,
    framebuffer_t  *framebuffer,
    application_t **applications,
    size_t          num
) {
    if (!window || !framebuffer) {
        printf("run_launcher: null window or framebuffer\n");
        return false;
    }

    printf("Starting application launcher with %zu applications\n", num);

    Launcher_Context ctx = {0};
    ctx.applications     = applications;
    ctx.num_apps         = num;
    ctx.selected_item    = 0;
    ctx.scroll_offset    = 0;
    ctx.show_about       = false;
    ctx.quit             = false;
    ctx.folder_start_index = -1;
    ctx.current_folder   = NULL;
    ctx.folders_json     = NULL;
    ctx.items            = NULL;

    ctx.window      = window;
    ctx.framebuffer = framebuffer;
    ctx.pixels      = framebuffer->pixels;

    build_item_list(&ctx);
    event_t e;

    while (!ctx.quit) {
        memset(ctx.pixels, 0, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));

        draw_launcher_window(&ctx);

        if (ctx.show_about) {
            draw_about_dialog(&ctx);
        }

        window_present(ctx.window, true, NULL, 0);

        e = window_event_poll(ctx.window, true, 0);
        if (e.type == EVENT_QUIT) {
            ctx.quit = true;
        } else if (e.type == EVENT_KEY_DOWN) {
            handle_keyboard(&ctx, e.keyboard.scancode);
        }
    }

    free(ctx.items);
    free(ctx.folder_apps);
    cJSON_Delete(ctx.folders_json);

    return true;
}

int main(int argc, char *argv[]) {
    /* 1. Create window and framebuffer once — reused by boot screen and launcher */
    window_handle_t window = window_create(
        "Application Launcher",
        (window_size_t){SCREEN_WIDTH, SCREEN_HEIGHT},
        WINDOW_FLAG_DOUBLE_BUFFERED | WINDOW_FLAG_FULLSCREEN | WINDOW_FLAG_LOW_PRIORITY
    );
    if (!window) {
        printf("Window could not be created\n");
        return 1;
    }

    framebuffer_t *framebuffer = window_framebuffer_create(
        window,
        (window_size_t){SCREEN_WIDTH, SCREEN_HEIGHT},
        BADGEVMS_PIXELFORMAT_RGB565
    );
    if (!framebuffer) {
        printf("Framebuffer could not be created\n");
        window_destroy(window);
        return 1;
    }

    /* 2. Load WHY logo (failure is non-fatal — boot screen will be text-only) */
    int            logo_w = 0, logo_h = 0, logo_ch_in_file = 0;
    unsigned char *logo_data = stbi_load(
        "APPS:[badgevms_launcher]logo.png",
        &logo_w, &logo_h, &logo_ch_in_file, 4
    );
    int logo_ch = logo_data ? 4 : 0;
    if (!logo_data) {
        printf("Warning: could not load logo.png, boot screen will be text-only\n");
    }

    /* 3. Record boot start time */
    struct timespec boot_start;
    clock_gettime(CLOCK_MONOTONIC, &boot_start);

    /* 4. Spawn background scan thread */
    atomic_store(&g_scan_done, false);
    if (thread_create(scan_thread, NULL, 16384) == -1) {
        /* Fallback: scan synchronously then animate for the minimum period */
        printf("Warning: thread_create failed, scanning synchronously\n");
        scan_thread(NULL);
    }

    /* 5. Boot animation loop — runs until scan done AND 2000 ms elapsed */
    Launcher_Context boot_ctx = {0};
    boot_ctx.pixels = framebuffer->pixels;

    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long sec  = now.tv_sec  - boot_start.tv_sec;
        long nsec = now.tv_nsec - boot_start.tv_nsec;
        if (nsec < 0) { sec--; nsec += 1000000000L; }
        uint32_t elapsed_ms = (uint32_t)(sec * 1000 + nsec / 1000000);

        /* Brightness: sine wave 0.75 → 1.0, period 2 s */
        float bright    = 0.875f + 0.125f * sinf(2.0f * (float)M_PI * elapsed_ms / 2000.0f);
        int   dot_count = (int)(elapsed_ms / 500) % 4;

        draw_boot_screen(&boot_ctx, logo_data, logo_w, logo_h, logo_ch, bright, dot_count);
        window_present(window, true, NULL, 0);

        if (atomic_load(&g_scan_done) && elapsed_ms >= 2000)
            break;

        usleep(33 * 1000); /* ~30 fps */
    }

    /* 6. Clean up logo pixels — no longer needed */
    if (logo_data)
        stbi_image_free(logo_data);

    /* 7. Hand off to launcher with pre-created window and scanned app list */
    run_launcher(window, framebuffer, g_scan_apps, g_scan_count);

    return 0;
}
