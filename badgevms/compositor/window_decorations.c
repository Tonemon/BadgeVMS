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

#include "window_decorations.h"

#include "font.h"
#include "pixel_functions.h"

#define TAG "window_decorations"

#define RGB565(r, g, b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)))

// Default color scheme
IRAM_ATTR static window_colors_t window_colors = {
    .window_outer_border = RGB565(0, 0, 0), // Black

    // Foreground window colors
    .fg_titlebar_background       = RGB565(0, 0, 0),       // Black
    .fg_titlebar_text             = RGB565(255, 255, 255), // White
    .fg_titlebar_text_shadow      = RGB565(64, 64, 64),    // Dark gray
    .fg_titlebar_corner_accents   = RGB565(255, 255, 255), // White
    .fg_titlebar_horizontal_lines = RGB565(128, 128, 128), // Gray
    .fg_window_background         = RGB565(255, 255, 255), // White
    .fg_window_inner_border       = RGB565(0, 0, 0),       // Black
    .fg_window_outer_boundary     = RGB565(64, 64, 64),    // Dark gray
    .fg_window_decorative_dark    = RGB565(128, 128, 128), // Gray
    .fg_window_decorative_light   = RGB565(255, 255, 255), // White
    .fg_window_outer_border       = RGB565(255, 255, 255), // White

    // Background window colors
    .bg_titlebar_background      = RGB565(160, 160, 160), // Gray
    .bg_titlebar_dither_pattern  = RGB565(200, 200, 200), // Light gray
    .bg_titlebar_text            = RGB565(64, 64, 64),    // Dark gray
    .bg_titlebar_stippled_border = RGB565(96, 96, 96),    // Darker gray
    .bg_window_background        = RGB565(240, 240, 240), // Light gray
    .bg_window_inner_border      = RGB565(128, 128, 128), // Gray
    .bg_window_outer_border      = RGB565(200, 200, 200), // Light gray
};

IRAM_ATTR void draw_window_box(uint16_t *fb, window_t *window, bool foreground) {
    int x      = window->rect.x;
    int y      = window->rect.y;
    int width  = window->rect.w;
    int height = window->rect.h;

    int total_width  = width + 2 * BORDER_PX;
    int total_height = height + BORDER_TOP_PX + BORDER_PX;

    draw_rect_rotated(fb, x, y, total_width, total_height, window_colors.window_outer_border);

    // Draw inner border
    uint16_t inner_border_color =
        foreground ? window_colors.fg_window_outer_border : window_colors.bg_window_outer_border;
    draw_rect_rotated(fb, x + 1, y + 1, total_width - 2, total_height - 2, inner_border_color);

    // Draw title bar background
    if (foreground) {
        // Foreground window: black title bar
        draw_filled_rect_rotated(fb, x + 1, y + 1, total_width - 2, BORDER_TOP_PX, window_colors.fg_titlebar_background);

        // Top-left corner - simple L-shaped accent
        draw_filled_rect_rotated(fb, x + 3, y + 3, 3, 1, window_colors.fg_titlebar_corner_accents);
        draw_filled_rect_rotated(fb, x + 3, y + 3, 1, 3, window_colors.fg_titlebar_corner_accents);
        // Top-right corner - simple L-shaped accent
        draw_filled_rect_rotated(fb, x + total_width - 6, y + 3, 3, 1, window_colors.fg_titlebar_corner_accents);
        draw_filled_rect_rotated(fb, x + total_width - 4, y + 3, 1, 3, window_colors.fg_titlebar_corner_accents);

        // Horizontal accent lines in title bar
        draw_filled_rect_rotated(fb, x + 7, y + 6, total_width - 14, 1, window_colors.fg_titlebar_horizontal_lines);
        draw_filled_rect_rotated(
            fb,
            x + 7,
            y + BORDER_TOP_PX - 7,
            total_width - 14,
            1,
            window_colors.fg_titlebar_horizontal_lines
        );

    } else {
        // Background window: dithered/stippled title bar
        draw_filled_rect_rotated(
            fb,
            x + 2,
            y + 2,
            total_width - 4,
            BORDER_TOP_PX - 3,
            window_colors.bg_titlebar_background
        );

        // Add dither pattern to title bar to make it look faded/inactive
        for (int dither_y = y + 2; dither_y < y + BORDER_TOP_PX - 1; dither_y++) {
            for (int dither_x = x + 2; dither_x < x + total_width - 2; dither_x++) {
                if ((dither_x + dither_y) % 3 == 0) { // Sparse dither pattern
                    draw_pixel_rotated(fb, dither_x, dither_y, window_colors.bg_titlebar_dither_pattern);
                }
            }
        }

        // Stippled border accent for inactive windows
        for (int dot_x = x; dot_x < x + total_width; dot_x += 4) {
            draw_pixel_rotated(fb, dot_x, y, window_colors.bg_titlebar_stippled_border);
            draw_pixel_rotated(fb, dot_x, y + total_height - 1, window_colors.bg_titlebar_stippled_border);
        }
        for (int dot_y = y; dot_y < y + total_height; dot_y += 4) {
            draw_pixel_rotated(fb, x, dot_y, window_colors.bg_titlebar_stippled_border);
            draw_pixel_rotated(fb, x + total_width - 1, dot_y, window_colors.bg_titlebar_stippled_border);
        }
    }

    // Title text
    char title[21];
    if (window->title) {
        strncpy(title, window->title, 20);
    } else {
        strncpy(title, foreground ? "FOREGROUND" : "BACKGROUND", 20);
    }
    int max_text = strlen(title);
    int text_width;
    int title_bar_width = total_width - 4; // Account for borders

    do {
        title[max_text--] = '\0';
        text_width        = strlen(title) * (FONT_WIDTH + 1) - 1;
    } while (text_width > title_bar_width);

    int text_x = x + 2 + (title_bar_width - text_width) / 2;
    int text_y = y + (BORDER_TOP_PX - FONT_HEIGHT) / 2; // Center vertically in title bar

    if (foreground) {
        // Subtle drop shadow effect
        draw_text_rotated(fb, title, text_x + 1, text_y + 1, window_colors.fg_titlebar_text_shadow);
        draw_text_rotated(fb, title, text_x, text_y, window_colors.fg_titlebar_text);
    } else {
        draw_text_rotated(fb, title, text_x, text_y, window_colors.bg_titlebar_text);
    }

    // Inner content border
    uint16_t border_color = foreground ? window_colors.fg_window_inner_border : window_colors.bg_window_inner_border;
    int      content_x    = x + BORDER_PX;
    int      content_y    = y + BORDER_TOP_PX;
    draw_rect_rotated(fb, content_x - 1, content_y - 1, width + 2, height + 2, border_color);
}
