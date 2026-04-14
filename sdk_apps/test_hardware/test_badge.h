#pragma once

#include "badgevms/compositor.h"
#include "badgevms/event.h"
#include "badgevms/framebuffer.h"
#include "badgevms/keyboard.h"
#include "test_badge.h"
#include "thirdparty/microui.h"

#include <stdbool.h>

#include <string.h>
#include <time.h>

typedef struct {
    char name[32];
    char status[32];
    bool passed;
} test_result_t;

typedef enum {
    KEY_STATE_NEVER_PRESSED,
    KEY_STATE_CURRENTLY_PRESSED,
    KEY_STATE_PREVIOUSLY_PRESSED,
    KEY_STATE_STUCK_DOWN
} key_state_t;

typedef struct {
    keyboard_scancode_t scancode;
    char                label[8];
    key_state_t         state;
    uint64_t            last_press_time;
} key_info_t;


typedef struct {
    mu_Context     *ctx;
    framebuffer_t  *fb;
    window_handle_t window;

    test_result_t tests[10];
    int           num_tests;

    key_info_t row1[8];  // Function keys
    key_info_t row2[13]; // Number row
    key_info_t row3[13]; // QWERTY row
    key_info_t row4[13]; // ASDF row
    key_info_t row5[13]; // ZXCV row
    key_info_t row6[9];  // Bottom row

    char input_buffer[50];
} app_state_t;
