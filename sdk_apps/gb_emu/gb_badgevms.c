/* peanut-gb single-header — define implementation here, no audio */
#define ENABLE_SOUND 0
#define ENABLE_LCD   1
#include "peanut_gb.h"
#include "emu_overlay.h"

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

#define GB_W LCD_WIDTH   /* 160 */
#define GB_H LCD_HEIGHT  /* 144 */

/* DMG green palette (lightest → darkest) */
static const uint16_t g_dmg_pal[4] = {
    0xE7FF,  /* #E8FFD8 */
    0x7328,  /* #70C070 */
    0x3185,  /* #307830 */
    0x0820,  /* #083808 */
};

static struct gb_s    g_gb;
static uint8_t       *g_rom         = NULL;
static uint8_t       *g_cart_ram    = NULL;
static size_t         g_cart_ram_sz = 0;
static window_handle_t g_win        = NULL;
static framebuffer_t  *g_fb         = NULL;
static bool           g_running     = true;
static char           g_save_path[256];

/* ------------------------------------------------------------------ */
/* peanut-gb callbacks                                                 */
/* ------------------------------------------------------------------ */

static uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr) {
    (void)gb;
    return g_rom[addr];
}

static uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
    (void)gb;
    return g_cart_ram ? g_cart_ram[addr] : 0xFF;
}

static void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr,
                              const uint8_t val) {
    (void)gb;
    if (g_cart_ram) g_cart_ram[addr] = val;
}

static void gb_error_cb(struct gb_s *gb, const enum gb_error_e err,
                        const uint16_t addr) {
    (void)gb; (void)addr;
    printf("gb_emu: error %d at 0x%04X\n", (int)err, addr);
    g_running = false;
}

/* LCD scanline callback — pixels[x] bits 1-0 = shade index (0-3) */
static void lcd_draw_line(struct gb_s *gb, const uint8_t *pixels,
                          const uint_fast8_t line) {
    (void)gb;
    uint16_t *row = &g_fb->pixels[(uint_fast8_t)line * GB_W];
    for (int x = 0; x < GB_W; x++)
        row[x] = g_dmg_pal[pixels[x] & 0x03];
}

/* ------------------------------------------------------------------ */
/* Battery-backed save RAM                                             */
/* ------------------------------------------------------------------ */

static void load_save(void) {
    if (!g_cart_ram || !g_cart_ram_sz) return;
    FILE *f = fopen(g_save_path, "rb");
    if (!f) return;
    fread(g_cart_ram, 1, g_cart_ram_sz, f);
    fclose(f);
}

static void write_save(void) {
    if (!g_cart_ram || !g_cart_ram_sz) return;
    FILE *f = fopen(g_save_path, "wb");
    if (!f) return;
    fwrite(g_cart_ram, 1, g_cart_ram_sz, f);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    g_win = window_create("Game Boy Emulator",
                          (window_size_t){720, 720},
                          WINDOW_FLAG_DOUBLE_BUFFERED | WINDOW_FLAG_MAXIMIZED);
    g_fb  = window_framebuffer_create(g_win, (window_size_t){GB_W, GB_H},
                                      BADGEVMS_PIXELFORMAT_RGB565);

    if (argc < 2) {
        memset(g_fb->pixels, 0, GB_W * GB_H * sizeof(uint16_t));
        window_present(g_win, true, NULL, 0);
        sleep(3);
        window_destroy(g_win);
        return 1;
    }

    /* Load ROM */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("gb_emu: cannot open %s\n", argv[1]); window_destroy(g_win); return 1; }
    fseek(f, 0, SEEK_END);
    long rom_sz = ftell(f);
    rewind(f);
    g_rom = malloc((size_t)rom_sz);
    if (!g_rom) { fclose(f); window_destroy(g_win); return 1; }
    fread(g_rom, 1, (size_t)rom_sz, f);
    fclose(f);

    snprintf(g_save_path, sizeof(g_save_path), "%s.sav", argv[1]);

    enum gb_init_error_e err = gb_init(
        &g_gb, gb_rom_read, gb_cart_ram_read, gb_cart_ram_write, gb_error_cb, NULL
    );
    if (err != GB_INIT_NO_ERROR) {
        printf("gb_emu: gb_init error %d\n", (int)err);
        free(g_rom);
        window_destroy(g_win);
        return 1;
    }

    gb_get_save_size_s(&g_gb, &g_cart_ram_sz);
    if (g_cart_ram_sz) {
        g_cart_ram = calloc(1, g_cart_ram_sz);
        load_save();
    }

    gb_init_lcd(&g_gb, lcd_draw_line);
    g_gb.direct.frame_skip = true;

    char label[64];
    emu_overlay_label(label, sizeof(label), argv[1], "[GB]");
    window_title_set(g_win, label);

    /* Joypad: 0xFF = all released (active low) */
    g_gb.direct.joypad = 0xFF;

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
            case KEY_SCANCODE_W:     if (down) g_gb.direct.joypad &= ~JOYPAD_UP;     else g_gb.direct.joypad |= JOYPAD_UP;     break;
            case KEY_SCANCODE_DOWN:
            case KEY_SCANCODE_S:     if (down) g_gb.direct.joypad &= ~JOYPAD_DOWN;   else g_gb.direct.joypad |= JOYPAD_DOWN;   break;
            case KEY_SCANCODE_LEFT:
            case KEY_SCANCODE_A:     if (down) g_gb.direct.joypad &= ~JOYPAD_LEFT;   else g_gb.direct.joypad |= JOYPAD_LEFT;   break;
            case KEY_SCANCODE_RIGHT:
            case KEY_SCANCODE_D:     if (down) g_gb.direct.joypad &= ~JOYPAD_RIGHT;  else g_gb.direct.joypad |= JOYPAD_RIGHT;  break;
            case KEY_SCANCODE_Z:     if (down) g_gb.direct.joypad &= ~JOYPAD_A;      else g_gb.direct.joypad |= JOYPAD_A;      break;
            case KEY_SCANCODE_X:     if (down) g_gb.direct.joypad &= ~JOYPAD_B;      else g_gb.direct.joypad |= JOYPAD_B;      break;
            case KEY_SCANCODE_RETURN:if (down) g_gb.direct.joypad &= ~JOYPAD_START;  else g_gb.direct.joypad |= JOYPAD_START;  break;
            case KEY_SCANCODE_RSHIFT:if (down) g_gb.direct.joypad &= ~JOYPAD_SELECT; else g_gb.direct.joypad |= JOYPAD_SELECT; break;
            case KEY_SCANCODE_ESCAPE: g_running = false; break;
            default: break;
            }
        } while (e.type != EVENT_NONE);

        if (!g_running) break;

        gb_run_frame(&g_gb);
        /* Only present on rendered frames (frame_skip_count=false after draw) */
        if (!g_gb.display.frame_skip_count)
            window_present(g_win, false, NULL, 0);
    }

    write_save();
    window_destroy(g_win);
    free(g_rom);
    free(g_cart_ram);
    return 0;
}
