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

#include "test_badge.h"

#include "badgevms/compositor.h"
#include "badgevms/event.h"
#include "badgevms/framebuffer.h"
#include "badgevms/keyboard.h"
#include "run_tests.h"
#include "test_drawing_helper.h"
#include "test_keyboard.h"
#include "thirdparty/microui.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <time.h>

typedef enum {
    RENDER_MODE_TESTS,
    RENDER_MODE_FULL_WHITE,
    RENDER_MODE_FULL_RED,
    RENDER_MODE_FULL_GREEN,
    RENDER_MODE_FULL_BLUE,

    RENDER_MODE_COUNT,
} render_mode_t;
static render_mode_t render_mode = RENDER_MODE_TESTS;

void init_tests(app_state_t *app) {
    app->num_tests = 10;

    strcpy(app->tests[0].name, "I2C Bus");
    strcpy(app->tests[0].status, "Scan Success 4/4");
    app->tests[0].passed = true;

    strcpy(app->tests[1].name, "SPI Flash");
    strcpy(app->tests[1].status, "Test Passed");
    app->tests[1].passed = true;

    strcpy(app->tests[2].name, "Display");
    strcpy(app->tests[2].status, "Test Passed");
    app->tests[2].passed = true;

    strcpy(app->tests[3].name, "Audio");
    strcpy(app->tests[3].status, "Test Failed");
    app->tests[3].passed = false;

    strcpy(app->tests[4].name, "WiFi");
    strcpy(app->tests[4].status, "Connection OK");
    app->tests[4].passed = true;

    strcpy(app->tests[5].name, "USB");
    strcpy(app->tests[5].status, "Device Ready");
    app->tests[5].passed = true;

    strcpy(app->tests[6].name, "SD Card");
    strcpy(app->tests[6].status, "Mount Failed");
    app->tests[6].passed = false;

    strcpy(app->tests[7].name, "Sensors");
    strcpy(app->tests[7].status, "All Online");
    app->tests[7].passed = true;

    // Initialize Device ID test with "Loading..."
    strcpy(app->tests[8].name, "Device ID");
    strcpy(app->tests[8].status, "Loading...");
    app->tests[8].passed = false;

    // Initialize Badgehub connection test with "Loading..."
    strcpy(app->tests[9].name, "Badgehub connection");
    strcpy(app->tests[9].status, "Loading...");
    app->tests[9].passed = false;
}

void render_ui(app_state_t *app) {
    mu_Context *ctx = app->ctx;

    uint16_t bg_color = rgb888_to_rgb565(32, 32, 32);

    if (render_mode != RENDER_MODE_TESTS) {
        if (render_mode == RENDER_MODE_FULL_WHITE)
            bg_color = rgb888_to_rgb565(0xFF, 0xFF, 0xFF);
        if (render_mode == RENDER_MODE_FULL_RED)
            bg_color = rgb888_to_rgb565(0xFF, 0x00, 0x00);
        if (render_mode == RENDER_MODE_FULL_GREEN)
            bg_color = rgb888_to_rgb565(0x00, 0xFF, 0x00);
        if (render_mode == RENDER_MODE_FULL_BLUE)
            bg_color = rgb888_to_rgb565(0x00, 0x00, 0xFF);
    }

    for (int i = 0; i < app->fb->w * app->fb->h; i++) {
        app->fb->pixels[i] = bg_color;
    }

    if (render_mode != RENDER_MODE_TESTS)
        return;

    mu_begin(ctx);

    if (mu_begin_window(ctx, "WHY2025 Badge Test", mu_rect(0, 0, 720, 270))) {
        mu_layout_row(ctx, 1, (int[]){-1}, 40);
        mu_text(ctx, "WHY2025 Badge Test");

        mu_layout_row(ctx, 1, (int[]){-1}, 10);

        mu_layout_row(ctx, 2, (int[]){340, 340}, 35);

        for (int i = 0; i < app->num_tests; i++) {
            mu_Color test_color = app->tests[i].passed ? mu_color(0, 150, 0, 255) : mu_color(200, 0, 0, 255);

            set_button_color(ctx, test_color);

            char button_text[128];
            snprintf(button_text, sizeof(button_text), "%s\n%s", app->tests[i].name, app->tests[i].status);

            mu_button(ctx, button_text);

            if ((i + 1) % 2 == 0 && i < app->num_tests - 1) {
                mu_layout_row(ctx, 2, (int[]){340, 340}, 35);
            }
        }

        reset_button_colors(ctx);

        mu_layout_row(ctx, 1, (int[]){-1}, 10);

        mu_layout_row(ctx, 1, (int[]){-1}, 25);
        char input_label[300];
        snprintf(input_label, sizeof(input_label), "Keyboard Input: %s", app->input_buffer);
        mu_text(ctx, input_label);

        mu_end_window(ctx);
    }

    render_keyboard(app);

    mu_end(ctx);

    mu_Command *cmd = NULL;
    while (mu_next_command(ctx, &cmd)) {
        switch (cmd->type) {
            case MU_COMMAND_TEXT:
                draw_text(app->fb, cmd->text.str, cmd->text.pos.x, cmd->text.pos.y, cmd->text.color);
                break;
            case MU_COMMAND_RECT: draw_rect(app->fb, cmd->rect.rect, cmd->rect.color); break;
            case MU_COMMAND_ICON:
            case MU_COMMAND_CLIP: break;
        }
    }
}

int main() {
    app_state_t app = {0};

    app.window = window_create(
        "WHY2025 Badge Test",
        (window_size_t){720, 720},
        WINDOW_FLAG_FULLSCREEN | WINDOW_FLAG_DOUBLE_BUFFERED
    );
    int fb_num;
    app.fb = window_framebuffer_create(app.window, (window_size_t){720, 720}, BADGEVMS_PIXELFORMAT_RGB565);

    app.ctx = malloc(sizeof(mu_Context));
    mu_init(app.ctx);
    app.ctx->text_width  = mu_text_width;
    app.ctx->text_height = mu_text_height;

    init_tests(&app);
    init_keyboard_layout(&app);
    strcpy(app.input_buffer, "Type here...");

    bool       running              = true;
    long const target_frame_time_us = 16667;

    memset(app.fb->pixels, 0x55, 720 * 720 * 2);

    render_ui(&app);
    window_present(app.window, true, NULL, 0);

    run_tests(&app, fb_num);

    while (running) {
        struct timespec start_time, cur_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        while (1) {
            clock_gettime(CLOCK_MONOTONIC, &cur_time);
            long elapsed_us =
                (cur_time.tv_sec - start_time.tv_sec) * 1000000L + (cur_time.tv_nsec - start_time.tv_nsec) / 1000L;
            long sleep_time = target_frame_time_us - elapsed_us;

            if (sleep_time <= 0) {
                break;
            }

            event_t event = window_event_poll(app.window, false, sleep_time / 1000);

            switch (event.type) {
                case EVENT_QUIT: {
                    running = false;
                    break;
                }
                case EVENT_KEY_DOWN: {
                    if (event.keyboard.scancode == KEY_SCANCODE_SPACE) {
                        render_mode++;
                        if (render_mode >= RENDER_MODE_COUNT)
                            render_mode = 0;
                    }
                    handle_keyboard_event(&app, &event.keyboard);
                    break;
                }
                case EVENT_KEY_UP: {
                    handle_keyboard_event(&app, &event.keyboard);
                    break;
                }
                default: break;
            }

            render_ui(&app);
        }

        static uint64_t frame_counter = 0;
        frame_counter++;
        check_stuck_keys(&app, frame_counter * 16000000ULL);

        window_present(app.window, true, NULL, 0);
    }

    window_destroy(app.window);
    free(app.ctx);

    return 0;
}
