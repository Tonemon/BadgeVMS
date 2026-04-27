#include "../shared/rom_browser.h"
#include "../shared/font.h"

#include <badgevms/compositor.h>
#include <badgevms/framebuffer.h>
#include <badgevms/pixel_formats.h>
#include <badgevms/process.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* System table                                                        */
/* ------------------------------------------------------------------ */

static const char *nes_exts[] = {".nes", NULL};
static const char *gb_exts[]  = {".gb", ".gbc", NULL};
static const char *sms_exts[] = {".sms", ".bin", NULL};

#define NUM_SYSTEMS 3

static const struct {
    const char  *rom_dir;
    const char **extensions;
    const char  *emulator;
    const char  *elf_name;
    const char  *label;
} systems[NUM_SYSTEMS] = {
    { "SD0:[ROMS.NES]", nes_exts, "APPS:[nes_emu]nes_emu.elf",  "nes_emu.elf",  "NES"           },
    { "SD0:[ROMS.GB]",  gb_exts,  "APPS:[gb_emu]gb_emu.elf",   "gb_emu.elf",   "Game Boy"      },
    { "SD0:[ROMS.SMS]", sms_exts, "APPS:[sms_emu]sms_emu.elf", "sms_emu.elf",  "Master System" },
};

/* ------------------------------------------------------------------ */
/* Minimal drawing helpers (CDE palette, same as rom_browser)         */
/* ------------------------------------------------------------------ */

#define CDE_BG_COLOR      0x9CA0A0u
#define CDE_PANEL_COLOR   0xAEB2B2u
#define CDE_BORDER_LIGHT  0xFFFFFFu
#define CDE_BORDER_DARK   0x636363u
#define CDE_TEXT_COLOR    0x000000u
#define CDE_TITLE_BG      0x808080u
#define CDE_WHITE_TEXT    0xFFFFFFu

typedef struct { uint16_t *pixels; int w, h; } ctx_t;

static inline uint16_t rgb_to_565(uint32_t rgb) {
    return (uint16_t)((((rgb >> 16) & 0xFF) >> 3) << 11 |
                      (((rgb >>  8) & 0xFF) >> 2) << 5  |
                      (( rgb        & 0xFF) >> 3));
}

static void fill_rect(ctx_t *c, int x, int y, int w, int h, uint32_t col) {
    uint16_t v = rgb_to_565(col);
    int x2 = x + w > c->w ? c->w : x + w;
    int y2 = y + h > c->h ? c->h : y + h;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    for (int py = y0; py < y2; py++)
        for (int px = x0; px < x2; px++)
            c->pixels[py * c->w + px] = v;
}

static void draw_char(ctx_t *c, int x, int y, char ch, uint32_t col) {
    if (ch < FONT_FIRST_CHAR || ch > FONT_LAST_CHAR) return;
    uint16_t v = rgb_to_565(col);
    const uint16_t *glyph = pixel_font[ch - FONT_FIRST_CHAR];
    for (int row = 0; row < FONT_HEIGHT; row++) {
        int py = y + row;
        if (py < 0 || py >= c->h) continue;
        for (int bit = 0; bit < FONT_WIDTH; bit++) {
            if (glyph[row] & (0x800 >> bit)) {
                int px = x + bit;
                if (px >= 0 && px < c->w)
                    c->pixels[py * c->w + px] = v;
            }
        }
    }
}

static void draw_text(ctx_t *c, int x, int y, const char *s, uint32_t col) {
    for (; *s; s++, x += FONT_WIDTH)
        draw_char(c, x, y, *s, col);
}

static void draw_text_centered(ctx_t *c, int rx, int y, int rw,
                               const char *s, uint32_t col) {
    int tw = (int)(strlen(s) * (size_t)FONT_WIDTH);
    draw_text(c, rx + (rw - tw) / 2, y, s, col);
}

static void draw_border(ctx_t *c, int x, int y, int w, int h, int inset) {
    uint32_t tl = inset ? CDE_BORDER_DARK  : CDE_BORDER_LIGHT;
    uint32_t br = inset ? CDE_BORDER_LIGHT : CDE_BORDER_DARK;
    fill_rect(c, x,         y,         w, 2, tl);
    fill_rect(c, x,         y,         2, h, tl);
    fill_rect(c, x,         y + h - 2, w, 2, br);
    fill_rect(c, x + w - 2, y,         2, h, br);
}

/* ------------------------------------------------------------------ */
/* Draw the info panel                                                 */
/* ------------------------------------------------------------------ */

static void draw_panel(ctx_t *c, const char *system_label,
                       const char *rom_name, const char *status) {
    fill_rect(c, 0, 0, c->w, c->h, CDE_BG_COLOR);
    fill_rect(c, 30, 30, 660, 420, CDE_PANEL_COLOR);
    draw_border(c, 30, 30, 660, 420, 0);

    /* Title bar */
    fill_rect(c, 33, 33, 654, 44, CDE_TITLE_BG);
    draw_text(c, 45, 44, "Start Random Game", CDE_WHITE_TEXT);

    /* Content */
    if (status) {
        draw_text_centered(c, 30, 230, 660, status, CDE_TEXT_COLOR);
    } else {
        /* System label */
        draw_text_centered(c, 30, 190, 660, system_label, CDE_TEXT_COLOR);

        /* ROM name — truncate to fit panel width (54 chars at 12px) */
        char truncated[55];
        snprintf(truncated, sizeof(truncated), "%.54s", rom_name);
        draw_text_centered(c, 30, 230, 660, truncated, CDE_TEXT_COLOR);
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    window_handle_t win = window_create("Start Random Game",
        (window_size_t){720, 720},
        WINDOW_FLAG_DOUBLE_BUFFERED | WINDOW_FLAG_MAXIMIZED);
    framebuffer_t *fb = window_framebuffer_create(
        win, (window_size_t){720, 720}, BADGEVMS_PIXELFORMAT_RGB565);
    ctx_t ctx = { fb->pixels, 720, 720 };

    srand((unsigned int)time(NULL));

    /* Scan all systems */
    char   **roms[NUM_SYSTEMS];
    size_t   counts[NUM_SYSTEMS];
    int      available[NUM_SYSTEMS];
    int      num_available = 0;

    draw_panel(&ctx, NULL, NULL, "Scanning for ROMs...");
    window_present(win, true, NULL, 0);

    for (int i = 0; i < NUM_SYSTEMS; i++) {
        roms[i] = rom_browser_scan(systems[i].rom_dir, systems[i].extensions,
                                   &counts[i]);
        if (counts[i] > 0)
            available[num_available++] = i;
    }

    if (num_available == 0) {
        draw_panel(&ctx, NULL, NULL, "No ROMs found on SD card.");
        window_present(win, true, NULL, 0);
        sleep(3);
        window_destroy(win);
        return 0;
    }

    /* Pick a random system, then a random ROM from that system */
    int   sys  = available[rand() % num_available];
    char *rom  = roms[sys][rand() % counts[sys]];

    /* Display filename only (strip directory prefix before last ']') */
    const char *name = strrchr(rom, ']');
    name = name ? name + 1 : rom;

    draw_panel(&ctx, systems[sys].label, name, NULL);
    window_present(win, true, NULL, 0);

    sleep(1);

    /* Spawn the emulator and wait for it to finish */
    const char *args[] = { systems[sys].elf_name, rom };
    process_create(systems[sys].emulator, 65536, 2, (char **)args);
    wait(true, 0);

    /* Free all scanned ROM lists */
    for (int i = 0; i < NUM_SYSTEMS; i++) {
        for (size_t j = 0; j < counts[i]; j++) free(roms[i][j]);
        free(roms[i]);
    }

    window_destroy(win);
    return 0;
}
