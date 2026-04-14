#include "badgevms/compositor.h"
#include "badgevms/event.h"
#include "badgevms/framebuffer.h"

#include <stddef.h>
#include <stdio.h>

#include <math.h>
#include <string.h> // for memcpy
#include <time.h>   // for clock_gettime
#include <unistd.h> // for usleep

// Configurable framebuffer dimensions
#define FB_WIDTH  (360)
#define FB_HEIGHT (360)
#define W_WIDTH   (360)
#define W_HEIGHT  (360)

int main(int argc, char *argv[]) {
    window_size_t size;
    size.w = W_WIDTH;
    size.h = W_HEIGHT;

    window_handle_t window = window_create("FB test", size, WINDOW_FLAG_DOUBLE_BUFFERED);

    size.w                              = FB_WIDTH;
    size.h                              = FB_HEIGHT;
    framebuffer_t *framebuffer          = window_framebuffer_create(window, size, BADGEVMS_PIXELFORMAT_RGB565);
    int            frame                = 0;
    long const     target_frame_time_us = 16667; // 60 FPS in microseconds

    // FPS tracking variables
    int             frames_rendered = 0;
    struct timespec fps_start_time;
    clock_gettime(CLOCK_MONOTONIC, &fps_start_time);

    memset(framebuffer->pixels, getpid() * 0xc, FB_WIDTH * FB_HEIGHT * 2);
    memset(framebuffer->pixels, 0xff, FB_WIDTH * 10);

    bool vis = false;

    while (1) {
        struct timespec start_time, end_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        // Calculate horizontal offset based on frame
        // The pattern repeats every FB_WIDTH pixels, so we use modulo to wrap
        int offset = frame % FB_WIDTH;
        // int offset = (frame * 3) % FB_WIDTH;  // Move 3 pixels per frame

        event_t e = window_event_poll(window, false, 0);
        if (e.type == EVENT_KEY_DOWN) {
            if (e.keyboard.scancode == KEY_SCANCODE_TAB) {
                vis = !vis;
            }
            if (e.keyboard.scancode == KEY_SCANCODE_ESCAPE) {
                break;
            }
        }

        static uint16_t row_template[FB_WIDTH];
        if (vis) {
            for (int x = 0; x < FB_WIDTH; x++) {
                int shifted_x = (x + offset) % FB_WIDTH;

                // Create a pattern that repeats every FB_WIDTH pixels
                // Use a smaller pattern unit that divides evenly
                int pattern_unit = FB_WIDTH / 6; // 60 pixels per unit
                int unit_index   = (shifted_x / pattern_unit) % 6;

                switch (unit_index) {
                    case 0:
                        row_template[x] = 0x1F; // Blue
                        break;
                    case 1:
                        row_template[x] = 0x7FF; // Cyan (Blue+Green)
                        break;
                    case 2:
                        row_template[x] = 0x7E0; // Green
                        break;
                    case 3:
                        row_template[x] = 0xFFE0; // Yellow (Green+Red)
                        break;
                    case 4:
                        row_template[x] = 0xF800; // Red
                        break;
                    case 5:
                        row_template[x] = 0xF81F; // Magenta (Red+Blue)
                        break;
                }
            }
        } else {
            for (int x = 0; x < FB_WIDTH; x++) {
                int shifted_x = (x + offset) % FB_WIDTH;

                // Create smooth color transitions using sine waves
                // Three sine waves 120 degrees apart for RGB
                double angle = 2.0 * M_PI * shifted_x / FB_WIDTH;

                int red   = (int)(31 * (0.5 + 0.5 * sin(angle)));
                int green = (int)(63 * (0.5 + 0.5 * sin(angle + 2.0 * M_PI / 3.0)));
                int blue  = (int)(31 * (0.5 + 0.5 * sin(angle + 4.0 * M_PI / 3.0)));

                // Pack into RGB565 format
                row_template[x] = (red << 11) | (green << 5) | blue;
            }
        }

        // #if 0
        // Copy this row FB_HEIGHT times to fill the entire framebuffer
        uint16_t *pixels = (uint16_t *)framebuffer->pixels;
        for (int y = 0; y < FB_HEIGHT; y++) {
            memcpy(&pixels[y * FB_WIDTH], row_template, FB_WIDTH * sizeof(uint16_t));
        }
        // #endif

        struct timespec post_start_time;
        clock_gettime(CLOCK_MONOTONIC, &post_start_time);

        window_present(window, true, NULL, 0);

        // Calculate how long the frame took
        clock_gettime(CLOCK_MONOTONIC, &end_time);

        long render_time_us = (post_start_time.tv_sec - start_time.tv_sec) * 1000000L +
                              (post_start_time.tv_nsec - start_time.tv_nsec) / 1000L;
        long post_time_us = (end_time.tv_sec - post_start_time.tv_sec) * 1000000L +
                            (end_time.tv_nsec - post_start_time.tv_nsec) / 1000L;
        long elapsed_us = render_time_us + post_time_us;

        // Sleep for the remaining time to maintain 60 FPS
        long sleep_time = target_frame_time_us - elapsed_us;
        if (sleep_time > 0) {
            usleep(sleep_time);
        }

        frame++;
        frames_rendered++;

        // Check if a second has passed and print FPS
        struct timespec current_time;
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        long fps_elapsed_ms = (current_time.tv_sec - fps_start_time.tv_sec) * 1000L +
                              (current_time.tv_nsec - fps_start_time.tv_nsec) / 1000000L;

        if (fps_elapsed_ms >= 10000) { // One second has passed
            double actual_fps = (double)frames_rendered / (fps_elapsed_ms / 1000.0);
            printf(
                "%u: FPS: %.1f (last frame: render=%ldus, post=%ldus, total=%ldus)\n",
                getpid(),
                actual_fps,
                render_time_us,
                post_time_us,
                elapsed_us
            );

            // Reset for next second
            frames_rendered = 0;
            fps_start_time  = current_time;
        }
    }
    printf("test_framebuffer_a exiting\n");
}
