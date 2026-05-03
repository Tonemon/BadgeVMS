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

#pragma once

#include "event.h"
#include "framebuffer.h"
#include "pixel_formats.h"

#include <stdbool.h>

typedef enum {
    WINDOW_FLAG_NONE            = 0,
    WINDOW_FLAG_FULLSCREEN      = (1 << 0), // Only one fullscreen application can run at a tim
    WINDOW_FLAG_ALWAYS_ON_TOP   = (1 << 1), // Does not apply to fullscreen apps
    WINDOW_FLAG_UNDECORATED     = (1 << 2), // Create a floating window
    WINDOW_FLAG_MAXIMIZED       = (1 << 3), // Create an application window of the maximum size
    WINDOW_FLAG_MAXIMIZED_LEFT  = (1 << 4), // Create a window and have it cover the whole left of the screen
    WINDOW_FLAG_MAXIMIZED_RIGHT = (1 << 5), // Create a window and have it cover the whole right of the screen
    WINDOW_FLAG_DOUBLE_BUFFERED = (1 << 6), // Create a double buffered window
    WINDOW_FLAG_LOW_PRIORITY    = (1 << 7), // Don't elevate my priority, even if I'm fullscreen
    WINDOW_FLAG_FLIP_HORIZONTAL = (1 << 8), // Flip my window horizontally
    WINDOW_FLAG_FLIP_VERTICAL   = (1 << 9), // Flip my window vertically
} window_flag_t;

typedef struct {
    int x;
    int y;
} window_coords_t;

typedef struct {
    int w;
    int h;
} window_size_t;

typedef struct {
    int x, y;
    int w, h;
} window_rect_t;

typedef struct window *window_handle_t;

window_handle_t window_create(char const *title, window_size_t size, window_flag_t flags);
framebuffer_t  *window_framebuffer_create(window_handle_t window, window_size_t size, pixel_format_t pixel_format);

void window_destroy(window_handle_t window);

char const *window_title_get(window_handle_t window);
void        window_title_set(window_handle_t window, char const *title);

window_coords_t window_position_get(window_handle_t window);
window_coords_t window_position_set(window_handle_t window, window_coords_t coords);

window_size_t window_size_get(window_handle_t window);
window_size_t window_size_set(window_handle_t window, window_size_t size);

window_flag_t window_flags_get(window_handle_t window);
window_flag_t window_flags_set(window_handle_t window, window_flag_t flags);

window_size_t  window_framebuffer_size_get(window_handle_t window);
window_size_t  window_framebuffer_size_set(window_handle_t window, window_size_t size);
pixel_format_t window_framebuffer_format_get(window_handle_t window);

framebuffer_t *window_framebuffer_get(window_handle_t window);
void           window_present(window_handle_t window, bool block, window_rect_t *rects, int num_rects);

event_t window_event_poll(window_handle_t window, bool block, uint32_t timeout_msec);

void get_screen_info(int *width, int *height, pixel_format_t *format, float *refresh_rate);

void compositor_set_autorotate(bool enabled);
bool compositor_get_autorotate(void);
void compositor_inject_keyboard_event(event_t e);
