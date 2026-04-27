#include "nofrendo/nofrendo.h"
#include "nofrendo/nes/nes.h"
#include "nofrendo/nes/input.h"
#include "emu_overlay.h"

#include <badgevms/compositor.h>
#include <badgevms/event.h>
#include <badgevms/framebuffer.h>
#include <badgevms/keyboard.h>
#include <badgevms/pixel_formats.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NES_W NES_SCREEN_WIDTH
#define NES_H NES_SCREEN_HEIGHT

static window_handle_t g_win    = NULL;
static framebuffer_t  *g_fb     = NULL;
static uint16_t       *g_pal565 = NULL;
static uint8_t         g_vidbuf[NES_SCREEN_PITCH * NES_SCREEN_HEIGHT];
static uint8_t         g_joypad = 0;

/* ------------------------------------------------------------------ */
/* Input callback — called every NES frame (draw and skip)            */
/* ------------------------------------------------------------------ */
static void input_frame(void) {
    event_t e;
    do {
        e = window_event_poll(g_win, false, 0);
        if (e.type == EVENT_QUIT) { nofrendo_stop(); return; }
        if (e.type != EVENT_KEY_DOWN && e.type != EVENT_KEY_UP) continue;

        bool down = (e.type == EVENT_KEY_DOWN);
        switch (e.keyboard.scancode) {
        case KEY_SCANCODE_UP:
        case KEY_SCANCODE_W:     if (down) g_joypad |= NES_PAD_UP;     else g_joypad &= ~NES_PAD_UP;     break;
        case KEY_SCANCODE_DOWN:
        case KEY_SCANCODE_S:     if (down) g_joypad |= NES_PAD_DOWN;   else g_joypad &= ~NES_PAD_DOWN;   break;
        case KEY_SCANCODE_LEFT:
        case KEY_SCANCODE_A:     if (down) g_joypad |= NES_PAD_LEFT;   else g_joypad &= ~NES_PAD_LEFT;   break;
        case KEY_SCANCODE_RIGHT:
        case KEY_SCANCODE_D:     if (down) g_joypad |= NES_PAD_RIGHT;  else g_joypad &= ~NES_PAD_RIGHT;  break;
        case KEY_SCANCODE_Z:     if (down) g_joypad |= NES_PAD_A;      else g_joypad &= ~NES_PAD_A;      break;
        case KEY_SCANCODE_X:     if (down) g_joypad |= NES_PAD_B;      else g_joypad &= ~NES_PAD_B;      break;
        case KEY_SCANCODE_RETURN:if (down) g_joypad |= NES_PAD_START;  else g_joypad &= ~NES_PAD_START;  break;
        case KEY_SCANCODE_RSHIFT:if (down) g_joypad |= NES_PAD_SELECT; else g_joypad &= ~NES_PAD_SELECT; break;
        case KEY_SCANCODE_ESCAPE: nofrendo_stop(); return;
        default: break;
        }
    } while (e.type != EVENT_NONE);

    input_update(0, g_joypad);
}

/* ------------------------------------------------------------------ */
/* Blit callback — called on rendered frames only (2:1 frame skip)    */
/* ------------------------------------------------------------------ */
static void blit_frame(uint8_t *vidbuf) {
    /* Convert indexed vidbuf → RGB565 into the framebuffer, skipping overdraw */
    uint16_t *dst = g_fb->pixels;
    for (int y = 0; y < NES_H; y++) {
        const uint8_t *src = vidbuf + y * NES_SCREEN_PITCH + NES_SCREEN_OVERDRAW;
        for (int x = 0; x < NES_W; x++)
            dst[y * NES_W + x] = g_pal565[src[x]];
    }

    window_present(g_win, false, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv) {
    g_win = window_create("NES Emulator",
                          (window_size_t){720, 720},
                          WINDOW_FLAG_DOUBLE_BUFFERED | WINDOW_FLAG_MAXIMIZED);
    g_fb  = window_framebuffer_create(g_win, (window_size_t){NES_W, NES_H},
                                      BADGEVMS_PIXELFORMAT_RGB565);

    if (argc < 2) {
        memset(g_fb->pixels, 0, NES_W * NES_H * sizeof(uint16_t));
        window_present(g_win, true, NULL, 0);
        sleep(3);
        window_destroy(g_win);
        return 1;
    }

    char label[64];
    emu_overlay_label(label, sizeof(label), argv[1], "[NES]");
    window_title_set(g_win, label);

    nofrendo_init(SYS_NES_NTSC, 0, false, blit_frame, NULL, NULL);
    nofrendo_set_frame_begin(input_frame);
    nes_setvidbuf(g_vidbuf);

    /* Build RGB565 palette (256 entries) */
    g_pal565 = nofrendo_buildpalette(NES_PALETTE_NOFRENDO, 16);

    input_connect(0, NES_JOYPAD);

    nofrendo_start(argv[1], NULL);

    free(g_pal565);
    window_destroy(g_win);
    return 0;
}
