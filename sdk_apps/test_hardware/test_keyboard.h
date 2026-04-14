#pragma once

#include "test_badge.h"

void     init_keyboard_layout(app_state_t *app);
void     handle_keyboard_event(app_state_t *app, keyboard_event_t *kbd_event);
void     check_stuck_keys(app_state_t *app, uint64_t current_time);
mu_Color get_key_color(key_state_t state);
void     set_button_color(mu_Context *ctx, mu_Color color);
void     reset_button_colors(mu_Context *ctx);
void     count_key_stats(app_state_t *app, int *never, int *current, int *previous, int *stuck);
void     render_keyboard(app_state_t *app);
