#include "font.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <badgevms/wifi.h>
#include <SDL3/SDL.h>
#include <string.h>
#include <time.h>

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

typedef enum { SCREEN_MAIN, SCREEN_WIFI, SCREEN_DISPLAY, SCREEN_SYSTEM, SCREEN_ABOUT } ScreenState;

#define NAV_STACK_MAX 8

typedef struct {
    ScreenState screen;
    int         selected_item;
    int         scroll_offset;
} NavEntry;

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
} app_context;

static void render_screen(app_context *ctx);

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
    if (ctx->nav_depth < NAV_STACK_MAX - 1) {
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

    // char const *categories[]   = {"WiFi Settings", "Display Settings", "System Information", "About"};
    char const *categories[]   = {"WiFi Settings", "About"};
    char const *descriptions[] = {
        "Configure wireless network connection",
        // "Adjust screen brightness and timeout",
        // "View system status and diagnostics",
        "Application version and credits"
    };
    ctx->total_items = 2;

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
    int dialog_w = 450;
    int dialog_h = 250;
    int dialog_x = (SCREEN_WIDTH - dialog_w) / 2;
    int dialog_y = (SCREEN_HEIGHT - dialog_h) / 2;

    draw_rect(ctx, dialog_x + 5, dialog_y + 5, dialog_w, dialog_h, 0x505050);

    draw_rect(ctx, dialog_x, dialog_y, dialog_w, dialog_h, CDE_PANEL_COLOR);
    draw_3d_border(ctx, dialog_x, dialog_y, dialog_w, dialog_h, 0);

    int title_h = 30;
    draw_rect(ctx, dialog_x + 2, dialog_y + 2, dialog_w - 4, title_h, CDE_TITLE_BG);
    draw_text_bold(ctx, dialog_x + 10, dialog_y + 8, "About Settings", CDE_SELECTED_TEXT);

    int content_y = dialog_y + title_h + 30;
    draw_text_centered(ctx, dialog_x, content_y, dialog_w, "System Settings", CDE_TEXT_COLOR);
    draw_text_centered(ctx, dialog_x, content_y + 30, dialog_w, "Version 1.0", CDE_TEXT_COLOR);
    draw_text_centered(ctx, dialog_x, content_y + 60, dialog_w, "BadgeVMS system settings", CDE_INACTIVE_TEXT);

    draw_text_centered(ctx, dialog_x, content_y + 120, dialog_w, "Press ENTER or ESC to close", CDE_INACTIVE_TEXT);
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

static void handle_key_event(app_context *ctx, SDL_Event *event) {
    SDL_Keycode key = event->key.key;

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
                        nav_push(ctx, SCREEN_ABOUT);
                        break;
                        // case 1: nav_push(ctx, SCREEN_DISPLAY); break;
                        // case 2: nav_push(ctx, SCREEN_SYSTEM); break;
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
        case SCREEN_MAIN: draw_main_settings(ctx); break;
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
    SDL_DestroyTexture(ctx.texture);
    SDL_DestroyRenderer(ctx.renderer);
    SDL_DestroyWindow(ctx.window);
    SDL_Quit();

    return 0;
}
