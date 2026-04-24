#include <stdio.h>
#include <stdlib.h>

#include <badgevms/compositor.h>
#include <badgevms/event.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <time.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define WINDOW_WIDTH    720
#define WINDOW_HEIGHT   720
#define MAX_LINE_LENGTH 256
#define NUM_LINES       4
#define PADDING         40
#define MIN_FONT_SIZE   20
#define MAX_FONT_SIZE   200
#define CURSOR_BLINK_MS 500

#define PNG_WIDTH  213
#define PNG_HEIGHT 176
#define PNG_MARGIN 10

#define LINE1_MAX_HEIGHT 150.0f
#define LINE2_MAX_HEIGHT 150.0f
#define LINE3_MAX_HEIGHT 250.0f
#define LINE4_MAX_HEIGHT 80.0f

#define SAVE_FILE "APPS:[why2025_namebadge]badge_state.txt"

#define DRAFT_TIMEOUT_MS    1000
#define DRAFT_QUALITY_SCALE 2
#define SAVE_TIMEOUT_MS     5000

size_t      images_size = 13;
char const *images[]    = {
    "APPS:[why2025_namebadge]logo.png",
    "APPS:[why2025_namebadge]Asexual-flag.png",
    "APPS:[why2025_namebadge]Bisexual-flag.png",
    "APPS:[why2025_namebadge]Gender-Fluid-flag.png",
    "APPS:[why2025_namebadge]Genderqueer-flag.png",
    "APPS:[why2025_namebadge]Intersex-flag.png",
    "APPS:[why2025_namebadge]Lesbian-flag.png",
    "APPS:[why2025_namebadge]Non-Binary-flag.png",
    "APPS:[why2025_namebadge]Pansexual-flag-1.png",
    "APPS:[why2025_namebadge]Pride-flag.png",
    "APPS:[why2025_namebadge]Progress-Pride-flag.png",
    "APPS:[why2025_namebadge]Trans-flag.png",
    "APPS:[why2025_namebadge]Unity-Pride-flag.png",
};

size_t      greetings_size = 7;
char const *greetings[]    = {
    "ASSERT",
    "Hello",
    "Hello, world!",
    "Bonjour",
    "404 greeting not found",
    "/usr/bin/fortune",
    "ping",
    "int 21h",
    "printf(\"hello\");"
};

size_t      intros_size = 3;
char const *intros[]    = {
    "My name is",
    "My hostname is",
    "I am",
    "cat /etc/hostname",
};

size_t      names_size = 3;
char const *names[]    = {
    "Fabulous",
    "Fantasatic",
    "Gorgeous",
    "Incredible",
};

int             running = 1;
struct timespec start_time;

int render_png_with_alpha_scaled(
    uint16_t *framebuffer, int fb_width, int fb_height, char const *filename, int dest_x, int dest_y, int scale_factor
);

typedef struct {
    unsigned char r, g, b;
} color_t;

typedef struct {
    char     text[MAX_LINE_LENGTH];
    char     prev_text[MAX_LINE_LENGTH];
    color_t  color;
    float    font_size;
    float    max_height;
    bool     dirty;
    bool     draft_mode;
    uint32_t last_edit_time;
    bool     needs_hq_render;
} textline_t;

typedef struct {
    window_handle_t window;
    uint16_t       *framebuffer;
    framebuffer_t  *window_framebuffer;
    uint32_t        last_save_time;

    stbtt_fontinfo font;
    unsigned char *font_data;

    textline_t lines[NUM_LINES];
    int        current_line;
    int        prev_line;
    int        cursor_pos;
    uint32_t   last_cursor_blink;
    int        cursor_visible;
    int        prev_cursor_visible;
    int        image_index;

    color_t               cursor_color;
    bool                  needs_full_redraw;
    bool                  has_been_edited;
} state_t;

static void save_state(state_t *app) {
    // Work around the truncation bug by removing the file first
    remove(SAVE_FILE);

    FILE *file = fopen(SAVE_FILE, "w");
    if (!file) {
        printf("Failed to save state to %s\n", SAVE_FILE);
        return;
    }

    fprintf(file, "%d\n", app->image_index);

    for (int i = 0; i < NUM_LINES; i++) {
        fprintf(file, "%s\n", app->lines[i].text);
    }

    fclose(file);
    printf("State saved to %s\n", SAVE_FILE);
}

static bool load_state(state_t *app) {
    FILE *file = fopen(SAVE_FILE, "r");
    if (!file) {
        printf("No saved state found at %s\n", SAVE_FILE);
        return false;
    }

    char buffer[MAX_LINE_LENGTH];

    if (fgets(buffer, sizeof(buffer), file)) {
        int index = atoi(buffer);
        if (index >= 0 && index < images_size) {
            app->image_index = index;
        }
    }

    for (int i = 0; i < NUM_LINES; i++) {
        if (fgets(buffer, sizeof(buffer), file)) {
            size_t len = strlen(buffer);
            if (len > 0 && buffer[len - 1] == '\n') {
                buffer[len - 1] = '\0';
            }
            strcpy(app->lines[i].text, buffer);
            strcpy(app->lines[i].prev_text, "");
            app->lines[i].dirty = true;
        }
    }

    fclose(file);
    printf("State loaded from %s\n", SAVE_FILE);
    return true;
}

static uint16_t rgb_to_565(unsigned char r, unsigned char g, unsigned char b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static void set_pixel_565(uint16_t *fb, int x, int y, unsigned char r, unsigned char g, unsigned char b) {
    if (x < 0 || x >= WINDOW_WIDTH || y < 0 || y >= WINDOW_HEIGHT)
        return;

    fb[y * WINDOW_WIDTH + x] = rgb_to_565(r, g, b);
}

static int get_text_width(state_t *app, char const *text, float size) {
    if (!text || strlen(text) == 0)
        return 0;

    float scale = stbtt_ScaleForPixelHeight(&app->font, size);
    int   width = 0;

    for (int i = 0; text[i]; i++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&app->font, text[i], &advance, &lsb);
        width += (int)(advance * scale);
    }

    return width;
}

static float calculate_font_size(state_t *app, char const *text, int max_width, float max_height) {
    if (!text || strlen(text) == 0)
        return max_height;

    float size = max_height;

    float min_size = MIN_FONT_SIZE;
    float max_size = fminf(MAX_FONT_SIZE, max_height);

    while (max_size - min_size > 1.0f) {
        size      = (min_size + max_size) / 2.0f;
        int width = get_text_width(app, text, size);

        if (width > max_width) {
            max_size = size;
        } else {
            min_size = size;
        }
    }

    return min_size;
}

static void clear_line_area(state_t *app, int line_index) {
    int line_height = WINDOW_HEIGHT / NUM_LINES;
    int y_start     = line_index * line_height;
    int y_end       = y_start + line_height;

    int x_start, x_end;

    if (line_index == 0) {
        x_start = PNG_WIDTH + PNG_MARGIN;
        x_end   = WINDOW_WIDTH;
    } else {
        x_start = PADDING / 2;
        x_end   = WINDOW_WIDTH - (PADDING / 2);
    }

    uint16_t black = rgb_to_565(0, 0, 0);

    for (int y = y_start; y < y_end && y < WINDOW_HEIGHT; y++) {
        for (int x = x_start; x < x_end && x < WINDOW_WIDTH; x++) {
            app->framebuffer[y * WINDOW_WIDTH + x] = black;
        }
    }
}

static void render_text_draft(state_t *app, char const *text, int x, int y, float size, color_t color) {
    if (!text || strlen(text) == 0)
        return;

    float scale = stbtt_ScaleForPixelHeight(&app->font, size / DRAFT_QUALITY_SCALE);

    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&app->font, &ascent, &descent, &lineGap);
    int baseline = y + (int)(ascent * scale * DRAFT_QUALITY_SCALE);

    int text_x = x;

    for (int i = 0; text[i]; i++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&app->font, text[i], &advance, &lsb);

        int c_x1, c_y1, c_x2, c_y2;
        stbtt_GetCodepointBitmapBox(&app->font, text[i], scale, scale, &c_x1, &c_y1, &c_x2, &c_y2);

        int w = c_x2 - c_x1;
        int h = c_y2 - c_y1;

        if (w > 0 && h > 0) {
            unsigned char *bitmap = malloc(w * h);
            stbtt_MakeCodepointBitmap(&app->font, bitmap, w, h, w, scale, scale, text[i]);

            for (int py = 0; py < h; py += 1) {
                for (int px = 0; px < w; px += 1) {
                    if (bitmap[py * w + px] > 128) { // Simple threshold
                        for (int sy = 0; sy < DRAFT_QUALITY_SCALE; sy++) {
                            for (int sx = 0; sx < DRAFT_QUALITY_SCALE; sx++) {
                                int draw_x = text_x + (px * DRAFT_QUALITY_SCALE) + sx + (c_x1 * DRAFT_QUALITY_SCALE);
                                int draw_y = baseline + (py * DRAFT_QUALITY_SCALE) + sy + (c_y1 * DRAFT_QUALITY_SCALE);
                                set_pixel_565(app->framebuffer, draw_x, draw_y, color.r, color.g, color.b);
                            }
                        }
                    }
                }
            }

            free(bitmap);
        }

        text_x += (int)(advance * scale * DRAFT_QUALITY_SCALE);
    }
}

static void render_text_hq(state_t *app, char const *text, int x, int y, float size, color_t color) {
    if (!text || strlen(text) == 0)
        return;

    float scale = stbtt_ScaleForPixelHeight(&app->font, size);

    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&app->font, &ascent, &descent, &lineGap);
    int baseline = y + (int)(ascent * scale);

    int text_x = x;

    for (int i = 0; text[i]; i++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&app->font, text[i], &advance, &lsb);

        int c_x1, c_y1, c_x2, c_y2;
        stbtt_GetCodepointBitmapBox(&app->font, text[i], scale, scale, &c_x1, &c_y1, &c_x2, &c_y2);

        int w = c_x2 - c_x1;
        int h = c_y2 - c_y1;

        if (w > 0 && h > 0) {
            unsigned char *bitmap = malloc(w * h);
            stbtt_MakeCodepointBitmap(&app->font, bitmap, w, h, w, scale, scale, text[i]);

            for (int py = 0; py < h; py++) {
                for (int px = 0; px < w; px++) {
                    unsigned char alpha = bitmap[py * w + px];
                    if (alpha > 0) {
                        int draw_x = text_x + px + c_x1;
                        int draw_y = baseline + py + c_y1;

                        unsigned char r = (color.r * alpha) / 255;
                        unsigned char g = (color.g * alpha) / 255;
                        unsigned char b = (color.b * alpha) / 255;

                        set_pixel_565(app->framebuffer, draw_x, draw_y, r, g, b);
                    }
                }
            }

            free(bitmap);
        }

        text_x += (int)(advance * scale);
    }
}

static void render_line(state_t *app, int line_index) {
    textline_t *line = &app->lines[line_index];

    if (strlen(line->text) > 0) {
        int line_height = WINDOW_HEIGHT / NUM_LINES;
        int usable_width;
        int text_area_start;

        if (line_index == 0) {
            text_area_start = PNG_WIDTH + PNG_MARGIN;
            usable_width    = WINDOW_WIDTH - text_area_start - PADDING - PNG_MARGIN;
        } else {
            text_area_start = PADDING;
            usable_width    = WINDOW_WIDTH - (2 * PADDING);
        }

        float font_size = calculate_font_size(app, line->text, usable_width, line->max_height);
        line->font_size = font_size;

        int y = line_index * line_height + (line_height - (int)font_size) / 2;

        int text_width = get_text_width(app, line->text, font_size);
        int x;

        if (line_index == 0) {
            x = text_area_start + (usable_width - text_width) / 2;
        } else {
            x = (WINDOW_WIDTH - text_width) / 2;
        }

        if (line->draft_mode) {
            render_text_draft(app, line->text, x, y, font_size, line->color);
        } else {
            render_text_hq(app, line->text, x, y, font_size, line->color);
        }
    }

    strcpy(line->prev_text, line->text);
}

static void render_cursor(state_t *app) {
    if (app->cursor_visible) {
        int line_height = WINDOW_HEIGHT / NUM_LINES;
        int y           = app->current_line * line_height + line_height / 2;

        int x;
        if (strlen(app->lines[app->current_line].text) > 0) {
            char temp[MAX_LINE_LENGTH];
            strncpy(temp, app->lines[app->current_line].text, app->cursor_pos);
            temp[app->cursor_pos] = '\0';

            float font_size = app->lines[app->current_line].font_size;
            if (font_size == 0)
                font_size = 40; // Default size

            int text_width    = get_text_width(app, app->lines[app->current_line].text, font_size);
            int cursor_offset = get_text_width(app, temp, font_size);

            if (app->current_line == 0) {
                int text_area_start = PNG_WIDTH + PNG_MARGIN;
                int usable_width    = WINDOW_WIDTH - text_area_start - PADDING;
                x                   = text_area_start + (usable_width - text_width) / 2 + cursor_offset;
            } else {
                x = (WINDOW_WIDTH - text_width) / 2 + cursor_offset;
            }
        } else {
            if (app->current_line == 0) {
                int text_area_start = PNG_WIDTH + PNG_MARGIN;
                int usable_width    = WINDOW_WIDTH - text_area_start - PADDING;
                x                   = text_area_start + usable_width / 2;
            } else {
                x = WINDOW_WIDTH / 2;
            }
        }

        for (int cy = y - 20; cy < y + 20; cy++) {
            for (int cx = x; cx < x + 3; cx++) {
                set_pixel_565(
                    app->framebuffer,
                    cx,
                    cy,
                    app->lines[app->current_line].color.r,
                    app->lines[app->current_line].color.g,
                    app->lines[app->current_line].color.b
                );
            }
        }
    }
}

uint32_t get_ticks_ms() {
    struct timespec cur_time;
    clock_gettime(CLOCK_MONOTONIC, &cur_time);
    long elapsed_us =
        (cur_time.tv_sec - start_time.tv_sec) * 1000000L + (cur_time.tv_nsec - start_time.tv_nsec) / 1000L;

    return elapsed_us / 1000;
}

static void update_draft_modes(state_t *app) {
    uint32_t now = get_ticks_ms();

    for (int i = 0; i < NUM_LINES; i++) {
        textline_t *line = &app->lines[i];

        if (line->draft_mode && (now - line->last_edit_time > DRAFT_TIMEOUT_MS)) {
            line->draft_mode      = false;
            line->needs_hq_render = true;
            line->dirty           = true;
        }
    }
}

static void maybe_save(state_t *app) {
    uint32_t now = get_ticks_ms();

    if (app->has_been_edited) {
        if (app->last_save_time > SAVE_TIMEOUT_MS) {
            app->last_save_time = now;
            save_state(app);
            app->has_been_edited = false;
        }
    }
}

static int load_font(state_t *app, char const *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        printf("Failed to open font file: %s\n", filename);
        return 0;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    app->font_data = malloc(size);
    fread(app->font_data, size, 1, file);
    fclose(file);

    if (!stbtt_InitFont(&app->font, app->font_data, 0)) {
        printf("Failed to initialize font\n");
        free(app->font_data);
        return 0;
    }

    return 1;
}

static state_t *init_app(char const *font_path) {
    state_t *app = calloc(1, sizeof(state_t));

    app->window = window_create(
        "Badge Application",
        (window_size_t){WINDOW_WIDTH, WINDOW_HEIGHT},
        WINDOW_FLAG_FULLSCREEN | WINDOW_FLAG_DOUBLE_BUFFERED
    );
    if (!app->window) {
        printf("window_create failed\n");
        free(app);
        return NULL;
    }

    app->window_framebuffer = window_framebuffer_create(
        app->window,
        (window_size_t){WINDOW_WIDTH, WINDOW_HEIGHT},
        BADGEVMS_PIXELFORMAT_RGB565
    );
    if (!app->window_framebuffer) {
        printf("window_framebuffer_create failed\n");
        free(app);
        return NULL;
    }

    app->framebuffer = malloc(WINDOW_WIDTH * WINDOW_HEIGHT * 2);
    if (!app->framebuffer) {
        printf("allocation of framebuffer failed\n");
        free(app);
        return NULL;
    }

    if (!load_font(app, font_path)) {
        free(app);
        return NULL;
    }

    app->lines[0].color = (color_t){0x46, 0xad, 0xb6};
    app->lines[1].color = (color_t){0xee, 0x5d, 0x93};
    app->lines[2].color = (color_t){0xf9, 0xf5, 0x93};
    app->lines[3].color = (color_t){0x46, 0xad, 0xb6};

    app->lines[0].max_height = LINE1_MAX_HEIGHT;
    app->lines[1].max_height = LINE2_MAX_HEIGHT;
    app->lines[2].max_height = LINE3_MAX_HEIGHT;
    app->lines[3].max_height = LINE4_MAX_HEIGHT;

    if (!load_state(app)) {
        int greet_index = rand() % (greetings_size - 1);
        int intro_index = rand() % (intros_size - 1);
        int name_index  = rand() % (names_size - 1);
        strcpy(app->lines[0].text, greetings[greet_index]);
        strcpy(app->lines[1].text, intros[intro_index]);
        strcpy(app->lines[2].text, names[intro_index]);
        strcpy(app->lines[3].text, "They/Them");
        app->image_index = 0;
    }

    for (int i = 0; i < NUM_LINES; i++) {
        app->lines[i].dirty           = true;
        app->lines[i].draft_mode      = false;
        app->lines[i].last_edit_time  = 0;
        app->lines[i].needs_hq_render = false;
        strcpy(app->lines[i].prev_text, "");
    }

    app->cursor_color = (color_t){255, 255, 0};

    app->current_line        = 0;
    app->prev_line           = 0;
    app->cursor_pos          = strlen(app->lines[0].text);
    app->cursor_visible      = 1;
    app->prev_cursor_visible = 1;
    app->last_cursor_blink   = get_ticks_ms();
    app->needs_full_redraw   = true;
    app->has_been_edited     = false;

    return app;
}

static void render_frame(state_t *app) {
    bool needs_update = false;

    update_draft_modes(app);

    if (app->needs_full_redraw) {
        memset(app->framebuffer, 0, WINDOW_WIDTH * WINDOW_HEIGHT * 2);
        render_png_with_alpha_scaled(app->framebuffer, WINDOW_WIDTH, WINDOW_HEIGHT, images[app->image_index], 0, 0, 1);

        for (int i = 0; i < NUM_LINES; i++) {
            render_line(app, i);
            app->lines[i].dirty = false;
        }

        app->needs_full_redraw = false;
        needs_update           = true;
    } else {
        for (int i = 0; i < NUM_LINES; i++) {
            if (app->lines[i].dirty || strcmp(app->lines[i].text, app->lines[i].prev_text) != 0 ||
                app->lines[i].needs_hq_render) {
                clear_line_area(app, i);
                render_line(app, i);
                app->lines[i].dirty           = false;
                app->lines[i].needs_hq_render = false;
                needs_update                  = true;
            }
        }

        if (app->current_line != app->prev_line) {
            if (!app->lines[app->prev_line].draft_mode) {
                app->lines[app->prev_line].draft_mode      = false;
                app->lines[app->prev_line].needs_hq_render = true;
            }

            clear_line_area(app, app->prev_line);
            render_line(app, app->prev_line);

            clear_line_area(app, app->current_line);
            render_line(app, app->current_line);

            app->prev_line = app->current_line;
            needs_update   = true;
        }

        if (app->cursor_visible != app->prev_cursor_visible) {
            clear_line_area(app, app->current_line);
            render_line(app, app->current_line);
            app->prev_cursor_visible = app->cursor_visible;
            needs_update             = true;
        }
    }

    if (app->cursor_visible) {
        render_cursor(app);
        needs_update = true;
    }

    if (needs_update) {
        memcpy(app->window_framebuffer->pixels, app->framebuffer, WINDOW_WIDTH * WINDOW_HEIGHT * 2);
        window_present(app->window, true, NULL, 0);
        maybe_save(app);
    }
}

static void handle_text_input(state_t *app, char const text) {
    textline_t *line = &app->lines[app->current_line];
    int         len  = strlen(line->text);

    if (len < MAX_LINE_LENGTH - 1) {
        memmove(&line->text[app->cursor_pos + 1], &line->text[app->cursor_pos], len - app->cursor_pos + 1);
        line->text[app->cursor_pos] = text;
        app->cursor_pos++;

        line->draft_mode     = true;
        line->last_edit_time = get_ticks_ms();
        line->dirty          = true;

        app->has_been_edited = true;
    }
}

static void handle_key(state_t *app, keyboard_scancode_t scancode) {
    textline_t *line = &app->lines[app->current_line];

    switch (scancode) {
        case KEY_SCANCODE_BACKSPACE:
            if (app->cursor_pos > 0) {
                memmove(
                    &line->text[app->cursor_pos - 1],
                    &line->text[app->cursor_pos],
                    strlen(&line->text[app->cursor_pos]) + 1
                );
                app->cursor_pos--;

                line->draft_mode     = true;
                line->last_edit_time = get_ticks_ms();
                line->dirty          = true;

                app->has_been_edited = true;
            } else {
                if (app->current_line > 0) {
                    app->lines[app->current_line].draft_mode      = false;
                    app->lines[app->current_line].needs_hq_render = true;

                    app->current_line--;
                    app->cursor_pos = strlen(app->lines[app->current_line].text);
                }
            }
            break;

        case KEY_SCANCODE_DELETE:
            if (app->cursor_pos < strlen(line->text)) {
                memmove(
                    &line->text[app->cursor_pos],
                    &line->text[app->cursor_pos + 1],
                    strlen(&line->text[app->cursor_pos + 1]) + 1
                );

                line->draft_mode     = true;
                line->last_edit_time = get_ticks_ms();
                line->dirty          = true;

                app->has_been_edited = true;
            }
            break;

        case KEY_SCANCODE_LEFT:
            if (app->cursor_pos > 0) {
                app->cursor_pos--;
                line->dirty = true;
            }
            break;

        case KEY_SCANCODE_RIGHT:
            if (app->cursor_pos < strlen(line->text)) {
                app->cursor_pos++;
                line->dirty = true;
            }
            break;

        case KEY_SCANCODE_UP:
            app->lines[app->current_line].draft_mode      = false;
            app->lines[app->current_line].needs_hq_render = true;

            if (app->current_line) {
                app->current_line--;
            } else {
                app->current_line = NUM_LINES - 1;
            }
            app->cursor_pos = strlen(app->lines[app->current_line].text);
            break;

        case KEY_SCANCODE_DOWN:
        case KEY_SCANCODE_RETURN:
        case KEY_SCANCODE_TAB:
            app->lines[app->current_line].draft_mode      = false;
            app->lines[app->current_line].needs_hq_render = true;

            app->current_line++;
            app->current_line %= NUM_LINES;
            app->cursor_pos    = strlen(app->lines[app->current_line].text);
            break;

        case KEY_SCANCODE_CLOUD:
            app->image_index++;
            app->image_index       %= images_size;
            app->needs_full_redraw  = true;

            app->has_been_edited = true;
            break;

        case KEY_SCANCODE_ESCAPE: running = 0; break;
    }
}

int main(int argc, char *argv[]) {
    srand(time(0));
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    char *font_path = "APPS:[why2025_namebadge]font.ttf";
    if (argc > 1) {
        font_path = argv[1];
    }

    state_t *app = init_app(font_path);
    if (!app) {
        printf("Failed to initialize application\n");
        printf("Usage: %s [path_to_ttf_font]\n", argv[0]);
        printf("Make sure you have a TrueType font file available\n");
        return 1;
    }

    event_t event;
    while (running) {
        event = window_event_poll(app->window, false, 0);
        switch (event.type) {
            case EVENT_QUIT: running = 0; break;
            case EVENT_KEY_DOWN:
                if (isprint(event.keyboard.text)) {
                    handle_text_input(app, event.keyboard.text);
                } else {
                    handle_key(app, event.keyboard.scancode);
                }
                break;
        }

        now = get_ticks_ms();
        if (now - app->last_cursor_blink > CURSOR_BLINK_MS) {
            app->cursor_visible                 = !app->cursor_visible;
            app->last_cursor_blink              = now;
            app->lines[app->current_line].dirty = true;
        }

        render_frame(app);
    }

    if (app->has_been_edited) {
        save_state(app);
    }
    return 0;
}
