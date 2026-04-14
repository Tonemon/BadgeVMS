#include "test_keyboard.h"

#include "badgevms/event.h"
#include "badgevms/keyboard.h"
#include "test_badge.h"
#include "thirdparty/microui.h"

#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <time.h>


// Initialize keyboard layout based on actual WHY2025 badge (from image)
void init_keyboard_layout(app_state_t *app) {
    // Row 1: Function keys
    keyboard_scancode_t const r1_scancodes[] = {
        KEY_SCANCODE_ESCAPE,
        KEY_SCANCODE_SQUARE,
        KEY_SCANCODE_TRIANGLE,
        KEY_SCANCODE_CROSS,
        KEY_SCANCODE_CIRCLE,
        KEY_SCANCODE_CLOUD,
        KEY_SCANCODE_DIAMOND,
        KEY_SCANCODE_BACKSPACE
    };
    char const *r1_labels[] = {"ESC", "□", "△", "✕", "○", "☁", "◆", "⌫"};

    for (int i = 0; i < 8; i++) {
        app->row1[i].scancode = r1_scancodes[i];
        strcpy(app->row1[i].label, r1_labels[i]);
        app->row1[i].state           = KEY_STATE_NEVER_PRESSED;
        app->row1[i].last_press_time = 0;
    }

    // Row 2: Number row
    keyboard_scancode_t const r2_scancodes[] = {
        KEY_SCANCODE_GRAVE,
        KEY_SCANCODE_1,
        KEY_SCANCODE_2,
        KEY_SCANCODE_3,
        KEY_SCANCODE_4,
        KEY_SCANCODE_5,
        KEY_SCANCODE_6,
        KEY_SCANCODE_7,
        KEY_SCANCODE_8,
        KEY_SCANCODE_9,
        KEY_SCANCODE_0,
        KEY_SCANCODE_MINUS,
        KEY_SCANCODE_EQUALS
    };
    char const *r2_labels[] = {"~", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "="};

    for (int i = 0; i < 13; i++) {
        app->row2[i].scancode = r2_scancodes[i];
        strcpy(app->row2[i].label, r2_labels[i]);
        app->row2[i].state           = KEY_STATE_NEVER_PRESSED;
        app->row2[i].last_press_time = 0;
    }

    // Row 3: QWERTY row
    keyboard_scancode_t const r3_scancodes[] = {
        KEY_SCANCODE_TAB,
        KEY_SCANCODE_Q,
        KEY_SCANCODE_W,
        KEY_SCANCODE_E,
        KEY_SCANCODE_R,
        KEY_SCANCODE_T,
        KEY_SCANCODE_Y,
        KEY_SCANCODE_U,
        KEY_SCANCODE_I,
        KEY_SCANCODE_O,
        KEY_SCANCODE_P,
        KEY_SCANCODE_LEFTBRACKET,
        KEY_SCANCODE_RIGHTBRACKET
    };
    char const *r3_labels[] = {"TAB", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]"};

    for (int i = 0; i < 13; i++) {
        app->row3[i].scancode = r3_scancodes[i];
        strcpy(app->row3[i].label, r3_labels[i]);
        app->row3[i].state           = KEY_STATE_NEVER_PRESSED;
        app->row3[i].last_press_time = 0;
    }

    // Row 4: ASDF row
    keyboard_scancode_t const r4_scancodes[] = {
        KEY_SCANCODE_FN,
        KEY_SCANCODE_A,
        KEY_SCANCODE_S,
        KEY_SCANCODE_D,
        KEY_SCANCODE_F,
        KEY_SCANCODE_G,
        KEY_SCANCODE_H,
        KEY_SCANCODE_J,
        KEY_SCANCODE_K,
        KEY_SCANCODE_L,
        KEY_SCANCODE_SEMICOLON,
        KEY_SCANCODE_APOSTROPHE,
        KEY_SCANCODE_RETURN
    };
    char const *r4_labels[] = {"Fn", "A", "S", "D", "F", "G", "H", "J", "K", "L", ";", "'", "↵"};

    for (int i = 0; i < 13; i++) {
        app->row4[i].scancode = r4_scancodes[i];
        strcpy(app->row4[i].label, r4_labels[i]);
        app->row4[i].state           = KEY_STATE_NEVER_PRESSED;
        app->row4[i].last_press_time = 0;
    }

    // Row 5: ZXCV row
    keyboard_scancode_t const r5_scancodes[] = {
        KEY_SCANCODE_LSHIFT,
        KEY_SCANCODE_Z,
        KEY_SCANCODE_X,
        KEY_SCANCODE_C,
        KEY_SCANCODE_V,
        KEY_SCANCODE_B,
        KEY_SCANCODE_N,
        KEY_SCANCODE_M,
        KEY_SCANCODE_COMMA,
        KEY_SCANCODE_PERIOD,
        KEY_SCANCODE_SLASH,
        KEY_SCANCODE_UP,
        KEY_SCANCODE_RSHIFT
    };
    char const *r5_labels[] = {"⇧", "Z", "X", "C", "V", "B", "N", "M", ",", ".", "/", "↑", "⇧"};

    for (int i = 0; i < 13; i++) {
        app->row5[i].scancode = r5_scancodes[i];
        strcpy(app->row5[i].label, r5_labels[i]);
        app->row5[i].state           = KEY_STATE_NEVER_PRESSED;
        app->row5[i].last_press_time = 0;
    }

    // Row 6: Bottom row
    keyboard_scancode_t const r6_scancodes[] = {
        KEY_SCANCODE_LCTRL,
        KEY_SCANCODE_LGUI,
        KEY_SCANCODE_LALT,
        KEY_SCANCODE_BACKSLASH,
        KEY_SCANCODE_SPACE,
        KEY_SCANCODE_RALT,
        KEY_SCANCODE_LEFT,
        KEY_SCANCODE_DOWN,
        KEY_SCANCODE_RIGHT
    };
    char const *r6_labels[] = {"CTRL", "⟲", "ALT", "|", "SPACE", "ALT", "←", "↓", "→"};

    for (int i = 0; i < 9; i++) {
        app->row6[i].scancode = r6_scancodes[i];
        strcpy(app->row6[i].label, r6_labels[i]);
        app->row6[i].state           = KEY_STATE_NEVER_PRESSED;
        app->row6[i].last_press_time = 0;
    }
}

void handle_keyboard_event(app_state_t *app, keyboard_event_t *kbd_event) {
    key_info_t *found_key = NULL;

    for (int i = 0; i < 8 && !found_key; i++) {
        if (app->row1[i].scancode == kbd_event->scancode)
            found_key = &app->row1[i];
    }
    for (int i = 0; i < 13 && !found_key; i++) {
        if (app->row2[i].scancode == kbd_event->scancode)
            found_key = &app->row2[i];
    }
    for (int i = 0; i < 13 && !found_key; i++) {
        if (app->row3[i].scancode == kbd_event->scancode)
            found_key = &app->row3[i];
    }
    for (int i = 0; i < 13 && !found_key; i++) {
        if (app->row4[i].scancode == kbd_event->scancode)
            found_key = &app->row4[i];
    }
    for (int i = 0; i < 13 && !found_key; i++) {
        if (app->row5[i].scancode == kbd_event->scancode)
            found_key = &app->row5[i];
    }
    for (int i = 0; i < 9 && !found_key; i++) {
        if (app->row6[i].scancode == kbd_event->scancode)
            found_key = &app->row6[i];
    }

    if (found_key) {
        if (kbd_event->down) {
            if (found_key->state == KEY_STATE_NEVER_PRESSED || found_key->state == KEY_STATE_PREVIOUSLY_PRESSED) {
                found_key->state           = KEY_STATE_CURRENTLY_PRESSED;
                found_key->last_press_time = kbd_event->timestamp;
            }
        } else {
            if (found_key->state == KEY_STATE_CURRENTLY_PRESSED || found_key->state == KEY_STATE_STUCK_DOWN) {
                found_key->state = KEY_STATE_PREVIOUSLY_PRESSED;
            }
        }
    }

    int input_buffer_len = strlen(app->input_buffer);
    if (kbd_event->down && kbd_event->text && kbd_event->text != 0) {
        if (input_buffer_len < sizeof(app->input_buffer) - 1) {
            app->input_buffer[input_buffer_len]     = kbd_event->text;
            app->input_buffer[input_buffer_len + 1] = '\0';
            ++input_buffer_len;
        }
    }

    if (kbd_event->down && kbd_event->scancode == KEY_SCANCODE_BACKSPACE) {
        if (input_buffer_len > 2) {
            // We just wrote a backspace to the buffer.
            app->input_buffer[input_buffer_len - 2] = '\0';
            --input_buffer_len;
        }
    }

    if (input_buffer_len == sizeof(app->input_buffer) - 1) {
        app->input_buffer[0] = '\0';
    }
}

// TODO this is kinda broken, maybe even as a concept
void check_stuck_keys(app_state_t *app, uint64_t current_time) {
    uint64_t const STUCK_THRESHOLD = 5000000000ULL; // 5 seconds

    key_info_t *all_keys[]  = {app->row1, app->row2, app->row3, app->row4, app->row5, app->row6};
    int         row_sizes[] = {8, 13, 13, 13, 13, 9};

    for (int row = 0; row < 6; row++) {
        for (int i = 0; i < row_sizes[row]; i++) {
            key_info_t *key = &all_keys[row][i];
            if (key->state == KEY_STATE_CURRENTLY_PRESSED) {
                if (current_time - key->last_press_time > STUCK_THRESHOLD) {
                    key->state = KEY_STATE_STUCK_DOWN;
                }
            }
        }
    }
}

mu_Color get_key_color(key_state_t state) {
    switch (state) {
        case KEY_STATE_NEVER_PRESSED: return mu_color(64, 64, 64, 255);       // Dark gray
        case KEY_STATE_CURRENTLY_PRESSED: return mu_color(0, 255, 0, 255);    // Bright green
        case KEY_STATE_PREVIOUSLY_PRESSED: return mu_color(255, 255, 0, 255); // Yellow
        case KEY_STATE_STUCK_DOWN: return mu_color(255, 0, 0, 255);           // Red
        default: return mu_color(64, 64, 64, 255);
    }
}

void set_button_color(mu_Context *ctx, mu_Color color) {
    ctx->style->colors[MU_COLOR_BUTTON]      = color;
    ctx->style->colors[MU_COLOR_BUTTONHOVER] = color;
    ctx->style->colors[MU_COLOR_BUTTONFOCUS] = color;
}

void reset_button_colors(mu_Context *ctx) {
    ctx->style->colors[MU_COLOR_BUTTON]      = mu_color(68, 68, 68, 255);
    ctx->style->colors[MU_COLOR_BUTTONHOVER] = mu_color(88, 88, 88, 255);
    ctx->style->colors[MU_COLOR_BUTTONFOCUS] = mu_color(78, 78, 78, 255);
}

void count_key_stats(app_state_t *app, int *never, int *current, int *previous, int *stuck) {
    *never = *current = *previous = *stuck = 0;

    key_info_t *all_keys[]  = {app->row1, app->row2, app->row3, app->row4, app->row5, app->row6};
    int         row_sizes[] = {8, 13, 13, 13, 13, 9};

    for (int row = 0; row < 6; row++) {
        for (int i = 0; i < row_sizes[row]; i++) {
            switch (all_keys[row][i].state) {
                case KEY_STATE_NEVER_PRESSED: (*never)++; break;
                case KEY_STATE_CURRENTLY_PRESSED: (*current)++; break;
                case KEY_STATE_PREVIOUSLY_PRESSED: (*previous)++; break;
                case KEY_STATE_STUCK_DOWN: (*stuck)++; break;
            }
        }
    }
}

void render_keyboard(app_state_t *app) {
    mu_Context *ctx = app->ctx;

    if (mu_begin_window(ctx, "Keyboard Tests", mu_rect(10, 280, 700, 430))) {

        int never, current, previous, stuck;
        count_key_stats(app, &never, &current, &previous, &stuck);

        char stats[256];
        snprintf(
            stats,
            sizeof(stats),
            "Keys: Tested=%d Untested=%d Active=%d Stuck=%d",
            previous,
            never,
            current,
            stuck
        );
        mu_text(ctx, stats);
        mu_text(ctx, "Gray=Untested  Green=Active  Yellow=Tested  Red=Stuck");

        mu_layout_row(ctx, 1, (int[]){-1}, 10); // Spacer

        // Row 1: Function keys (8 keys)
        mu_layout_row(ctx, 8, (int[]){80, 80, 80, 80, 80, 80, 80, 80}, 30);
        for (int i = 0; i < 8; i++) {
            set_button_color(ctx, get_key_color(app->row1[i].state));
            mu_button(ctx, app->row1[i].label);
        }

        // Row 2: Number row (13 keys)
        mu_layout_row(ctx, 13, (int[]){50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50}, 30);
        for (int i = 0; i < 13; i++) {
            set_button_color(ctx, get_key_color(app->row2[i].state));
            mu_button(ctx, app->row2[i].label);
        }

        // Row 3: QWERTY row (13 keys)
        mu_layout_row(ctx, 13, (int[]){50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50}, 30);
        for (int i = 0; i < 13; i++) {
            set_button_color(ctx, get_key_color(app->row3[i].state));
            mu_button(ctx, app->row3[i].label);
        }

        // Row 4: ASDF row (13 keys)
        mu_layout_row(ctx, 13, (int[]){50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50}, 30);
        for (int i = 0; i < 13; i++) {
            set_button_color(ctx, get_key_color(app->row4[i].state));
            mu_button(ctx, app->row4[i].label);
        }

        // Row 5: ZXCV row (13 keys)
        mu_layout_row(ctx, 13, (int[]){50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50}, 30);
        for (int i = 0; i < 13; i++) {
            set_button_color(ctx, get_key_color(app->row5[i].state));
            mu_button(ctx, app->row5[i].label);
        }

        // Row 6: Bottom row (9 keys)
        mu_layout_row(ctx, 9, (int[]){50, 50, 50, 50, 260, 50, 50, 50, 50}, 30);
        for (int i = 0; i < 9; i++) {
            set_button_color(ctx, get_key_color(app->row6[i].state));
            mu_button(ctx, app->row6[i].label);
        }

        reset_button_colors(ctx);
        mu_end_window(ctx);
    }
}
