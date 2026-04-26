#include "core/sms.h"

#include <badgevms/compositor.h>
#include <badgevms/event.h>
#include <badgevms/framebuffer.h>
#include <badgevms/keyboard.h>
#include <badgevms/pixel_formats.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SMS_W 256
#define SMS_H 192

static struct SMS_Core  g_sms;
static window_handle_t  g_win    = NULL;
static framebuffer_t   *g_fb     = NULL;
static bool             g_running = true;

/* ------------------------------------------------------------------ */
/* Colour callback: r,g,b are 2-bit SMS channels (0-3); return RGB565 */
/* ------------------------------------------------------------------ */
static uint32_t colour_callback(void *user, uint8_t r, uint8_t g2, uint8_t b) {
    (void)user;
    uint8_t r8 = (uint8_t)(r  * 85u);
    uint8_t g8 = (uint8_t)(g2 * 85u);
    uint8_t b8 = (uint8_t)(b  * 85u);
    return (uint32_t)(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    g_win = window_create("SMS Emulator",
                          (window_size_t){720, 720},
                          WINDOW_FLAG_DOUBLE_BUFFERED | WINDOW_FLAG_FULLSCREEN);
    g_fb  = window_framebuffer_create(g_win, (window_size_t){SMS_W, SMS_H},
                                      BADGEVMS_PIXELFORMAT_RGB565);

    if (argc < 2) {
        memset(g_fb->pixels, 0, SMS_W * SMS_H * sizeof(uint16_t));
        window_present(g_win, true, NULL, 0);
        sleep(3);
        window_destroy(g_win);
        return 1;
    }

    /* Load ROM */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("sms_emu: cannot open %s\n", argv[1]); window_destroy(g_win); return 1; }
    fseek(f, 0, SEEK_END);
    long rom_sz = ftell(f);
    rewind(f);
    uint8_t *rom = malloc((size_t)rom_sz);
    if (!rom) { fclose(f); window_destroy(g_win); return 1; }
    fread(rom, 1, (size_t)rom_sz, f);
    fclose(f);

    SMS_init(&g_sms);
    SMS_set_system_type(&g_sms, SMS_System_SMS);
    SMS_set_colour_callback(&g_sms, colour_callback);
    SMS_set_userdata(&g_sms, NULL);

    /* stride = pixels per row (units of uint16_t when bpp=16) */
    SMS_set_pixels(&g_sms, g_fb->pixels, SMS_W, 16);

    SMS_loadrom(&g_sms, rom, (size_t)rom_sz);
    free(rom);

    const size_t cpf = SMS_cycles_per_frame(&g_sms);

    while (g_running) {
        /* Poll input */
        event_t e;
        do {
            e = window_event_poll(g_win, false, 0);
            if (e.type == EVENT_QUIT) { g_running = false; break; }
            if (e.type != EVENT_KEY_DOWN && e.type != EVENT_KEY_UP) continue;

            bool down = (e.type == EVENT_KEY_DOWN);
            switch (e.keyboard.scancode) {
            case KEY_SCANCODE_UP:
            case KEY_SCANCODE_W:     SMS_set_buttons(&g_sms, SMS_Button_JOY1_UP,    down); break;
            case KEY_SCANCODE_DOWN:
            case KEY_SCANCODE_S:     SMS_set_buttons(&g_sms, SMS_Button_JOY1_DOWN,  down); break;
            case KEY_SCANCODE_LEFT:
            case KEY_SCANCODE_A:     SMS_set_buttons(&g_sms, SMS_Button_JOY1_LEFT,  down); break;
            case KEY_SCANCODE_RIGHT:
            case KEY_SCANCODE_D:     SMS_set_buttons(&g_sms, SMS_Button_JOY1_RIGHT, down); break;
            case KEY_SCANCODE_Z:     SMS_set_buttons(&g_sms, SMS_Button_JOY1_A,     down); break;
            case KEY_SCANCODE_X:     SMS_set_buttons(&g_sms, SMS_Button_JOY1_B,     down); break;
            case KEY_SCANCODE_RETURN:SMS_set_buttons(&g_sms, SMS_Button_PAUSE,      down); break;
            case KEY_SCANCODE_ESCAPE: g_running = false; break;
            default: break;
            }
        } while (e.type != EVENT_NONE);

        if (!g_running) break;

        SMS_run(&g_sms, (int)cpf);
        window_present(g_win, false, NULL, 0);
    }

    window_destroy(g_win);
    return 0;
}
