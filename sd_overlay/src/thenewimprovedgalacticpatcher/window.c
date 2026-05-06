#include "font.h"
#include "ota_update.h"

#include <stdio.h>
#include <stdlib.h>

#include <badgevms/wifi.h>
#include <SDL3/SDL.h>
#include <string.h>

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
#define CDE_PROGRESS_BG   0x606060
#define CDE_PROGRESS_FG   0x0078D4
#define CDE_SUCCESS_COLOR 0x00AA00
#define CDE_ERROR_COLOR   0xA00000

typedef enum { UI_STATE_CHECKING, UI_STATE_NO_UPDATES, UI_STATE_LIST, UI_STATE_PROGRESS, UI_STATE_COMPLETE } UI_State;

typedef struct {
    SDL_Window    *window;
    SDL_Renderer  *renderer;
    SDL_Texture   *framebuffer;
    Uint16        *pixels;
    update_item_t *updates;
    bool           connected;
    bool           connection_failed;
    bool           firmware_updated;
    int            scroll_offset;
    int            selected_item;
    int            total_items;
    int            items_per_page;
    UI_State       state;
    int            updates_completed;
    int            current_update_index;
} UI_Context;

static inline Uint16 rgb888_to_rgb565(Uint32 rgb888) {
    Uint8 r = (rgb888 >> 16) & 0xFF;
    Uint8 g = (rgb888 >> 8) & 0xFF;
    Uint8 b = rgb888 & 0xFF;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

void draw_rect(UI_Context *ctx, int x, int y, int w, int h, Uint32 color) {
    Uint16 rgb565 = rgb888_to_rgb565(color);
    int    x2     = x + w;
    int    y2     = y + h;

    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x2 > SCREEN_WIDTH)
        x2 = SCREEN_WIDTH;
    if (y2 > SCREEN_HEIGHT)
        y2 = SCREEN_HEIGHT;

    for (int py = y; py < y2; py++) {
        Uint16 *row   = &ctx->pixels[py * SCREEN_WIDTH + x];
        int     width = x2 - x;
        for (int i = 0; i < width; i++) {
            row[i] = rgb565;
        }
    }
}

void draw_char(UI_Context *ctx, int x, int y, char c, Uint32 color) {
    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR)
        return;

    int             char_index = c - FONT_FIRST_CHAR;
    uint16_t const *char_data  = pixel_font[char_index];
    Uint16          rgb565     = rgb888_to_rgb565(color);

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint16_t row_data = char_data[row];
        int      py       = y + row;

        if (py < 0 || py >= SCREEN_HEIGHT)
            continue;

        for (int col = 0; col < FONT_WIDTH; col++) {
            if (row_data & (0x800 >> col)) { // Check bit from MSB
                int px = x + col;
                if (px >= 0 && px < SCREEN_WIDTH) {
                    ctx->pixels[py * SCREEN_WIDTH + px] = rgb565;
                }
            }
        }
    }
}

void draw_text(UI_Context *ctx, int x, int y, char const *text, Uint32 color) {
    int current_x = x;

    while (*text) {
        draw_char(ctx, current_x, y, *text, color);
        current_x += FONT_WIDTH;
        text++;
    }
}

void draw_text_bold(UI_Context *ctx, int x, int y, char const *text, Uint32 color) {
    draw_text(ctx, x, y, text, color);
    draw_text(ctx, x + 1, y, text, color);
}

int get_text_width(char const *text) {
    return strlen(text) * FONT_WIDTH;
}

void draw_text_centered(UI_Context *ctx, int x, int y, int width, char const *text, Uint32 color) {
    int text_w = get_text_width(text);
    int text_x = x + (width - text_w) / 2;
    draw_text(ctx, text_x, y, text, color);
}

void draw_3d_border(UI_Context *ctx, int x, int y, int w, int h, int inset) {
    Uint32 light_color = inset ? CDE_BORDER_DARK : CDE_BORDER_LIGHT;
    Uint32 dark_color  = inset ? CDE_BORDER_LIGHT : CDE_BORDER_DARK;

    draw_rect(ctx, x, y, w, 3, light_color);
    draw_rect(ctx, x, y, 3, h, light_color);

    draw_rect(ctx, x, y + h - 3, w, 3, dark_color);
    draw_rect(ctx, x + w - 3, y, 3, h, dark_color);
}

void draw_completion_window(UI_Context *ctx) {
    int window_x = 30;
    int window_y = 30;
    int window_w = SCREEN_WIDTH - 60;
    int window_h = SCREEN_HEIGHT - 60;

    draw_rect(ctx, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, CDE_BG_COLOR);

    draw_rect(ctx, window_x, window_y, window_w, window_h, CDE_PANEL_COLOR);
    draw_3d_border(ctx, window_x, window_y, window_w, window_h, 0);

    int title_h = 45;
    draw_rect(ctx, window_x + 3, window_y + 3, window_w - 6, title_h, CDE_TITLE_BG);
    draw_text_bold(ctx, window_x + 15, window_y + 11, "System Update Complete", CDE_SELECTED_TEXT);

    int content_y = window_y + window_h / 2 - 120;

    int icon_size = 80;
    int icon_x    = window_x + (window_w - icon_size) / 2;
    int icon_y    = content_y;

    content_y += icon_size + 40;
    draw_text_centered(ctx, window_x, content_y, window_w, "All Updates Completed Successfully!", CDE_TEXT_COLOR);

    char summary_text[128];
    if (ctx->total_items == 1) {
        snprintf(summary_text, sizeof(summary_text), "1 application has been updated");
    } else {
        snprintf(summary_text, sizeof(summary_text), "%d applications have been updated", ctx->total_items);
    }
    draw_text_centered(ctx, window_x, content_y + 40, window_w, summary_text, CDE_TEXT_COLOR);

    if (ctx->firmware_updated) {
        draw_text_centered(
            ctx,
            window_x,
            content_y + 80,
            window_w,
            "Firmware update installed, please reboot",
            CDE_TEXT_COLOR
        );
    }

    draw_text_centered(
        ctx,
        window_x,
        window_y + window_h - 80,
        window_w,
        "Your badge is now up to date.",
        CDE_TEXT_COLOR
    );
    draw_text_centered(ctx, window_x, window_y + window_h - 45, window_w, "Press ESC to exit", CDE_TEXT_COLOR);
}

void draw_progress_window(UI_Context *ctx) {
    int window_x = 30;
    int window_y = 30;
    int window_w = SCREEN_WIDTH - 60;
    int window_h = SCREEN_HEIGHT - 60;

    draw_rect(ctx, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, CDE_BG_COLOR);

    draw_rect(ctx, window_x, window_y, window_w, window_h, CDE_PANEL_COLOR);
    draw_3d_border(ctx, window_x, window_y, window_w, window_h, 0);

    int title_h = 45;
    draw_rect(ctx, window_x + 3, window_y + 3, window_w - 6, title_h, CDE_TITLE_BG);
    draw_text_bold(ctx, window_x + 15, window_y + 11, "System Update Progress", CDE_SELECTED_TEXT);

    int content_y = window_y + window_h / 2 - 100;

    draw_text_centered(ctx, window_x, content_y, window_w, "Updating...", CDE_TEXT_COLOR);

    int bar_width  = window_w - 100;
    int bar_height = 40;
    int bar_x      = window_x + 50;
    int bar_y      = content_y + 60;

    draw_rect(ctx, bar_x, bar_y, bar_width, bar_height, CDE_PROGRESS_BG);
    draw_3d_border(ctx, bar_x, bar_y, bar_width, bar_height, 1);

    float progress   = (float)ctx->updates_completed / (float)ctx->total_items;
    int   fill_width = (int)((bar_width - 6) * progress);

    if (fill_width > 0) {
        draw_rect(ctx, bar_x + 3, bar_y + 3, fill_width, bar_height - 6, CDE_PROGRESS_FG);
        draw_rect(ctx, bar_x + 3, bar_y + 3, fill_width, 2, CDE_BORDER_LIGHT);
    }

    char progress_text[128];
    int  remaining = ctx->total_items - ctx->updates_completed;
    if (remaining == 1) {
        snprintf(progress_text, sizeof(progress_text), "1 application remaining");
    } else {
        snprintf(progress_text, sizeof(progress_text), "%d applications remaining", remaining);
    }
    draw_text_centered(ctx, window_x, bar_y + bar_height + 30, window_w, progress_text, CDE_TEXT_COLOR);

    if (ctx->current_update_index < ctx->total_items) {
        char current_text[256];
        snprintf(
            current_text,
            sizeof(current_text),
            "Currently updating: %s",
            ctx->updates[ctx->current_update_index].name
        );
        draw_text_centered(ctx, window_x, bar_y + bar_height + 60, window_w, current_text, CDE_TEXT_COLOR);
    }

    draw_text(
        ctx,
        window_x + 15,
        window_y + window_h - 35,
        "Please wait while updates are being installed...",
        CDE_TEXT_COLOR
    );
}

void draw_update_window(UI_Context *ctx) {
    int window_x = 30;
    int window_y = 30;
    int window_w = SCREEN_WIDTH - 60;
    int window_h = SCREEN_HEIGHT - 60;

    draw_rect(ctx, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, CDE_BG_COLOR);

    draw_rect(ctx, window_x, window_y, window_w, window_h, CDE_PANEL_COLOR);
    draw_3d_border(ctx, window_x, window_y, window_w, window_h, 0);

    int title_h = 45;
    draw_rect(ctx, window_x + 3, window_y + 3, window_w - 6, title_h, CDE_TITLE_BG);
    draw_text_bold(ctx, window_x + 15, window_y + 11, "System Updates", CDE_SELECTED_TEXT);

    char count_text[64];
    snprintf(count_text, sizeof(count_text), "Updates Available: %d", ctx->total_items);
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

        Uint32 text_color = (i == ctx->selected_item) ? CDE_SELECTED_TEXT : CDE_TEXT_COLOR;

        draw_text_bold(ctx, item_x + 8, item_y + 6, ctx->updates[i].name, text_color);

        char version_text[64];
        snprintf(version_text, sizeof(version_text), "Version: %s", ctx->updates[i].version);
        draw_text(ctx, item_x + 8, item_y + 30, version_text, text_color);

        char desc[60]       = {0};
        int  max_desc_chars = (item_w - 16) / FONT_WIDTH;
        if (max_desc_chars > 59)
            max_desc_chars = 59;
        if (ctx->updates[i].description) {
            strncpy(desc, ctx->updates[i].description, max_desc_chars);
            desc[max_desc_chars] = '\0';
            if (strlen(ctx->updates[i].description) > max_desc_chars) {
                desc[max_desc_chars - 3] = '.';
                desc[max_desc_chars - 2] = '.';
                desc[max_desc_chars - 1] = '.';
            }
        }
        draw_text(ctx, item_x + 8, item_y + 54, desc, text_color);

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
            thumb_h = 30; // Minimum thumb size
        int thumb_y = scrollbar_y;
        if (ctx->total_items > ctx->items_per_page) {
            thumb_y += ((scrollbar_h - thumb_h) * ctx->scroll_offset) / (ctx->total_items - ctx->items_per_page);
        }

        draw_rect(ctx, scrollbar_x + 3, thumb_y, 14, thumb_h, CDE_PANEL_COLOR);
        draw_3d_border(ctx, scrollbar_x + 3, thumb_y, 14, thumb_h, 0);
    }

    draw_text(
        ctx,
        window_x + 15,
        window_y + window_h - 35,
        "UP/DOWN to navigate, SPACE to update, ESC to exit",
        CDE_TEXT_COLOR
    );
}

void handle_keyboard(UI_Context *ctx, SDL_Scancode key_code) {
    if (ctx->state == UI_STATE_COMPLETE || ctx->state == UI_STATE_NO_UPDATES || ctx->connection_failed) {
        if (key_code == SDL_SCANCODE_ESCAPE) {
            SDL_Event quit_event;
            quit_event.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit_event);
        }
        return;
    }

    if (ctx->state == UI_STATE_CHECKING || ctx->state == UI_STATE_PROGRESS) {
        return;
    }

    if (ctx->state == UI_STATE_LIST) {
        switch (key_code) {
            case SDL_SCANCODE_UP:
                if (ctx->selected_item > 0) {
                    ctx->selected_item--;
                    if (ctx->selected_item < ctx->scroll_offset) {
                        ctx->scroll_offset = ctx->selected_item;
                    }
                }
                break;

            case SDL_SCANCODE_DOWN:
                if (ctx->selected_item < ctx->total_items - 1) {
                    ctx->selected_item++;
                    if (ctx->selected_item >= ctx->scroll_offset + ctx->items_per_page) {
                        ctx->scroll_offset = ctx->selected_item - ctx->items_per_page + 1;
                    }
                }
                break;

            case SDL_SCANCODE_SPACE:
            case SDL_SCANCODE_RETURN:
                printf("Starting updates...\n");
                ctx->state                = UI_STATE_PROGRESS;
                ctx->updates_completed    = 0;
                ctx->current_update_index = 0;
                break;

            case SDL_SCANCODE_ESCAPE: {
                SDL_Event quit_event;
                quit_event.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit_event);
            } break;
        }
    }
}

void update_progress(UI_Context *ctx) {
    if (ctx->state == UI_STATE_PROGRESS && ctx->updates_completed < ctx->total_items) {
        if (!ctx->updates[ctx->current_update_index].is_firmware) {
            update_application(
                ctx->updates[ctx->current_update_index].app,
                ctx->updates[ctx->current_update_index].version
            );
        } else {
            update_firmware();
        }

        ctx->updates_completed++;
        ctx->current_update_index++;

        if (ctx->updates_completed >= ctx->total_items) {
            ctx->state = UI_STATE_COMPLETE;
        }
    }
}

void draw_checking_window(UI_Context *ctx) {
    int window_x = 30;
    int window_y = 30;
    int window_w = SCREEN_WIDTH - 60;
    int window_h = SCREEN_HEIGHT - 60;

    draw_rect(ctx, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, CDE_BG_COLOR);

    draw_rect(ctx, window_x, window_y, window_w, window_h, CDE_PANEL_COLOR);
    draw_3d_border(ctx, window_x, window_y, window_w, window_h, 0);

    int title_h = 45;
    draw_rect(ctx, window_x + 3, window_y + 3, window_w - 6, title_h, CDE_TITLE_BG);
    draw_text_bold(ctx, window_x + 15, window_y + 11, "System Update Check", CDE_SELECTED_TEXT);

    int content_y = window_y + window_h / 2 - 60;

    draw_text_centered(ctx, window_x, content_y, window_w, "Checking for updates...", CDE_TEXT_COLOR);

    static int    animation_frame = 0;
    static Uint32 last_frame_time = 0;
    Uint32        current_time    = SDL_GetTicks();

    if (current_time - last_frame_time > 500) { // Update every 500ms
        animation_frame = (animation_frame + 1) % 4;
        last_frame_time = current_time;
    }

    char dots[5] = "    ";
    for (int i = 0; i < animation_frame; i++) {
        dots[i] = '.';
    }

    char progress_text[128];

    if (!ctx->connection_failed) {
        snprintf(progress_text, sizeof(progress_text), "Please wait%s", dots);
        draw_text_centered(ctx, window_x, content_y + 40, window_w, progress_text, CDE_TEXT_COLOR);

        if (ctx->connected) {
            draw_text_centered(
                ctx,
                window_x,
                window_y + window_h - 45,
                window_w,
                "Connecting to update server...",
                CDE_TEXT_COLOR
            );
        } else {
            draw_text_centered(
                ctx,
                window_x,
                window_y + window_h - 45,
                window_w,
                "Connecting to WiFi...",
                CDE_TEXT_COLOR
            );
        }
    } else {
        snprintf(progress_text, sizeof(progress_text), "Press escape to exit");
        draw_text_centered(ctx, window_x, content_y + 40, window_w, progress_text, CDE_ERROR_COLOR);
        draw_text_centered(
            ctx,
            window_x,
            window_y + window_h - 45,
            window_w,
            "Wifi connection failed",
            CDE_ERROR_COLOR
        );
    }
}

void draw_no_updates_window(UI_Context *ctx) {
    int window_x = 30;
    int window_y = 30;
    int window_w = SCREEN_WIDTH - 60;
    int window_h = SCREEN_HEIGHT - 60;

    draw_rect(ctx, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, CDE_BG_COLOR);

    draw_rect(ctx, window_x, window_y, window_w, window_h, CDE_PANEL_COLOR);
    draw_3d_border(ctx, window_x, window_y, window_w, window_h, 0);

    int title_h = 45;
    draw_rect(ctx, window_x + 3, window_y + 3, window_w - 6, title_h, CDE_TITLE_BG);
    draw_text_bold(ctx, window_x + 15, window_y + 11, "System Update Check", CDE_SELECTED_TEXT);

    int content_y = window_y + window_h / 2 - 80;

    int icon_size = 60;
    int icon_x    = window_x + (window_w - icon_size) / 2;
    int icon_y    = content_y;

    draw_rect(ctx, icon_x + 10, icon_y + 30, 15, 4, CDE_SUCCESS_COLOR);
    draw_rect(ctx, icon_x + 22, icon_y + 27, 4, 10, CDE_SUCCESS_COLOR);
    draw_rect(ctx, icon_x + 25, icon_y + 20, 4, 10, CDE_SUCCESS_COLOR);
    draw_rect(ctx, icon_x + 28, icon_y + 15, 4, 10, CDE_SUCCESS_COLOR);
    draw_rect(ctx, icon_x + 31, icon_y + 10, 4, 10, CDE_SUCCESS_COLOR);
    draw_rect(ctx, icon_x + 34, icon_y + 5, 4, 10, CDE_SUCCESS_COLOR);

    content_y += icon_size + 40;
    draw_text_centered(ctx, window_x, content_y, window_w, "Your system is up to date!", CDE_TEXT_COLOR);

    draw_text_centered(
        ctx,
        window_x,
        content_y + 40,
        window_w,
        "No updates are available at this time.",
        CDE_TEXT_COLOR
    );

    draw_text_centered(ctx, window_x, window_y + window_h - 45, window_w, "Press ESC to exit", CDE_TEXT_COLOR);
}

bool run_update_window_with_check(void) {
    debug_printf("run_update_window_with_check()\n");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return false;
    }
    debug_printf("SDL Init complete\n");

    UI_Context ctx           = {0};
    ctx.updates              = NULL;
    ctx.total_items          = 0;
    ctx.selected_item        = 0;
    ctx.scroll_offset        = 0;
    ctx.state                = UI_STATE_CHECKING; // Start in checking state
    ctx.updates_completed    = 0;
    ctx.current_update_index = 0;

    ctx.window = SDL_CreateWindow("System Updates", SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_FULLSCREEN);

    if (ctx.window == NULL) {
        printf("Window could not be created! SDL_Error: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    ctx.renderer = SDL_CreateRenderer(ctx.window, NULL);
    if (ctx.renderer == NULL) {
        printf("Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
        SDL_DestroyWindow(ctx.window);
        SDL_Quit();
        return false;
    }

    ctx.framebuffer = SDL_CreateTexture(
        ctx.renderer,
        SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING,
        SCREEN_WIDTH,
        SCREEN_HEIGHT
    );

    if (ctx.framebuffer == NULL) {
        printf("Framebuffer texture could not be created! SDL_Error: %s\n", SDL_GetError());
        SDL_DestroyRenderer(ctx.renderer);
        SDL_DestroyWindow(ctx.window);
        SDL_Quit();
        return false;
    }

    ctx.pixels = malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(Uint16));
    if (ctx.pixels == NULL) {
        printf("Could not allocate pixel buffer!\n");
        SDL_DestroyTexture(ctx.framebuffer);
        SDL_DestroyRenderer(ctx.renderer);
        SDL_DestroyWindow(ctx.window);
        SDL_Quit();
        return false;
    }

    int          quit = 0;
    SDL_Event    e;
    Uint32       frame_start, frame_time;
    Uint32 const frame_delay = 1000 / 60; // 60 FPS cap

    bool         check_started          = false;
    bool         check_complete         = false;
    Uint32       check_start_time       = 0;
    Uint32 const min_check_display_time = 1500; // Show checking screen for at least 1.5 seconds

    while (!quit) {
        frame_start = SDL_GetTicks();

        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                quit = 1;
            } else if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                    if (ctx.state == UI_STATE_COMPLETE || ctx.state == UI_STATE_LIST ||
                        ctx.state == UI_STATE_NO_UPDATES || ctx.connection_failed) {
                        quit = 1;
                    }
                } else {
                    handle_keyboard(&ctx, e.key.scancode);
                }
            }
        }

        memset(ctx.pixels, 0, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(Uint16));

        if (ctx.state == UI_STATE_CHECKING) {
            draw_checking_window(&ctx);

            if (!check_started && ctx.connected) {
                check_started    = true;
                check_start_time = SDL_GetTicks();
            }

            if (check_started && !check_complete && (SDL_GetTicks() - check_start_time > 100)) {
                size_t num_updates = perform_update_check(&ctx.updates);
                ctx.total_items    = num_updates;
                check_complete     = true;
            }

            if (check_complete && (SDL_GetTicks() - check_start_time > min_check_display_time)) {
                if (ctx.total_items > 0) {
                    ctx.state = UI_STATE_LIST;
                } else {
                    ctx.state = UI_STATE_NO_UPDATES;
                }
            }

        } else if (ctx.state == UI_STATE_NO_UPDATES) {
            draw_no_updates_window(&ctx);
        } else if (ctx.state == UI_STATE_LIST) {
            draw_update_window(&ctx);
        } else if (ctx.state == UI_STATE_PROGRESS) {
            draw_progress_window(&ctx);
        } else if (ctx.state == UI_STATE_COMPLETE) {
            draw_completion_window(&ctx);
        }

        SDL_UpdateTexture(ctx.framebuffer, NULL, ctx.pixels, SCREEN_WIDTH * sizeof(Uint16));
        SDL_SetRenderDrawColor(ctx.renderer, 0, 0, 0, 255);
        SDL_RenderClear(ctx.renderer);
        SDL_RenderTexture(ctx.renderer, ctx.framebuffer, NULL, NULL);
        SDL_RenderPresent(ctx.renderer);

        if (ctx.state == UI_STATE_PROGRESS) {
            update_progress(&ctx);
        }

        frame_time = SDL_GetTicks() - frame_start;
        if (frame_delay > frame_time) {
            SDL_Delay(frame_delay - frame_time);
        }

        if (!ctx.connected && !ctx.connection_failed) {
            wifi_connection_status_t result = wifi_connect();
            if (result != WIFI_CONNECTED) {
                ctx.connected         = false;
                ctx.connection_failed = true;
            } else {
                ctx.connected = true;
            }
        }
    }

    if (ctx.updates) {
        free(ctx.updates);
    }

    free(ctx.pixels);
    SDL_DestroyTexture(ctx.framebuffer);
    SDL_DestroyRenderer(ctx.renderer);
    SDL_DestroyWindow(ctx.window);
    SDL_Quit();
    return true;
}

bool run_update_window(update_item_t *updates, size_t num) {
    debug_printf("run_update_window()\n");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return false;
    }
    debug_printf("SDK Init complete\n");

    UI_Context ctx           = {0};
    ctx.updates              = updates;
    ctx.firmware_updated     = false;
    ctx.total_items          = num;
    ctx.selected_item        = 0;
    ctx.scroll_offset        = 0;
    ctx.state                = UI_STATE_LIST;
    ctx.updates_completed    = 0;
    ctx.current_update_index = 0;
    ctx.connected            = true;
    ctx.connection_failed    = false;

    debug_printf("Starting update window with %u updates\n", num);

    ctx.window = SDL_CreateWindow("System Updates", SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_FULLSCREEN);

    if (ctx.window == NULL) {
        printf("Window could not be created! SDL_Error: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    ctx.renderer = SDL_CreateRenderer(ctx.window, NULL);
    if (ctx.renderer == NULL) {
        printf("Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
        SDL_DestroyWindow(ctx.window);
        SDL_Quit();
        return false;
    }

    ctx.framebuffer = SDL_CreateTexture(
        ctx.renderer,
        SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING,
        SCREEN_WIDTH,
        SCREEN_HEIGHT
    );

    if (ctx.framebuffer == NULL) {
        printf("Framebuffer texture could not be created! SDL_Error: %s\n", SDL_GetError());
        SDL_DestroyRenderer(ctx.renderer);
        SDL_DestroyWindow(ctx.window);
        SDL_Quit();
        return false;
    }

    ctx.pixels = malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(Uint16));
    if (ctx.pixels == NULL) {
        printf("Could not allocate pixel buffer!\n");
        SDL_DestroyTexture(ctx.framebuffer);
        SDL_DestroyRenderer(ctx.renderer);
        SDL_DestroyWindow(ctx.window);
        SDL_Quit();
        return false;
    }

    int          quit = 0;
    SDL_Event    e;
    Uint32       frame_start, frame_time;
    Uint32 const frame_delay = 1000 / 60; // 60 FPS cap

    while (!quit) {
        frame_start = SDL_GetTicks();

        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                quit = 1;
            } else if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                    if (ctx.state == UI_STATE_COMPLETE || ctx.state == UI_STATE_LIST) {
                        quit = 1;
                    }
                } else {
                    handle_keyboard(&ctx, e.key.scancode);
                }
            }
        }

        memset(ctx.pixels, 0, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(Uint16));

        if (ctx.state == UI_STATE_LIST) {
            draw_update_window(&ctx);
        } else if (ctx.state == UI_STATE_PROGRESS) {
            draw_progress_window(&ctx);
        } else if (ctx.state == UI_STATE_COMPLETE) {
            draw_completion_window(&ctx);
        }

        SDL_UpdateTexture(ctx.framebuffer, NULL, ctx.pixels, SCREEN_WIDTH * sizeof(Uint16));
        SDL_SetRenderDrawColor(ctx.renderer, 0, 0, 0, 255);
        SDL_RenderClear(ctx.renderer);
        SDL_RenderTexture(ctx.renderer, ctx.framebuffer, NULL, NULL);
        SDL_RenderPresent(ctx.renderer);

        if (ctx.state == UI_STATE_PROGRESS) {
            update_progress(&ctx);
        }

        frame_time = SDL_GetTicks() - frame_start;
        if (frame_delay > frame_time) {
            SDL_Delay(frame_delay - frame_time);
        }
    }

    free(ctx.pixels);
    SDL_DestroyTexture(ctx.framebuffer);
    SDL_DestroyRenderer(ctx.renderer);
    SDL_DestroyWindow(ctx.window);
    return true;
}
