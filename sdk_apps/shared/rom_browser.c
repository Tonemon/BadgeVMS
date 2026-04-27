#include "rom_browser.h"
#include "font.h"  /* shared copy — see Task 2 */

#include <badgevms/event.h>
#include <badgevms/keyboard.h>

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* CDE colour constants (matching badgevms_launcher)                  */
/* ------------------------------------------------------------------ */
#define CDE_BG_COLOR      0x9CA0A0u
#define CDE_PANEL_COLOR   0xAEB2B2u
#define CDE_BORDER_LIGHT  0xFFFFFFu
#define CDE_BORDER_DARK   0x636363u
#define CDE_TEXT_COLOR    0x000000u
#define CDE_SELECTED_BG   0x0078D4u
#define CDE_SELECTED_TEXT 0xFFFFFFu
#define CDE_TITLE_BG      0x808080u
#define CDE_INACTIVE_TEXT 0x808080u

/* ------------------------------------------------------------------ */
/* Internal drawing context                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    uint16_t *pixels;
    int       w;
    int       h;
} rb_ctx_t;

static inline uint16_t rb_rgb888_to_rgb565(uint32_t rgb888) {
    uint8_t r = (rgb888 >> 16) & 0xFF;
    uint8_t g = (rgb888 >> 8)  & 0xFF;
    uint8_t b =  rgb888        & 0xFF;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void rb_fill_rect(rb_ctx_t *ctx, int x, int y, int w, int h,
                         uint32_t color888) {
    uint16_t c  = rb_rgb888_to_rgb565(color888);
    int      x2 = x + w, y2 = y + h;
    if (x  < 0)      x  = 0;
    if (y  < 0)      y  = 0;
    if (x2 > ctx->w) x2 = ctx->w;
    if (y2 > ctx->h) y2 = ctx->h;
    for (int py = y; py < y2; py++) {
        uint16_t *row = &ctx->pixels[py * ctx->w + x];
        for (int i = 0; i < x2 - x; i++) row[i] = c;
    }
}

static void rb_draw_char(rb_ctx_t *ctx, int x, int y, char ch,
                         uint32_t color888) {
    if (ch < FONT_FIRST_CHAR || ch > FONT_LAST_CHAR) return;
    uint16_t        c         = rb_rgb888_to_rgb565(color888);
    uint16_t const *char_data = pixel_font[ch - FONT_FIRST_CHAR];
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint16_t bits = char_data[row];
        int      py   = y + row;
        if (py < 0 || py >= ctx->h) continue;
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x800 >> col)) {
                int px = x + col;
                if (px >= 0 && px < ctx->w)
                    ctx->pixels[py * ctx->w + px] = c;
            }
        }
    }
}

static void rb_draw_text(rb_ctx_t *ctx, int x, int y, const char *text,
                         uint32_t color888) {
    for (; *text; text++, x += FONT_WIDTH)
        rb_draw_char(ctx, x, y, *text, color888);
}

static void rb_draw_text_centered(rb_ctx_t *ctx, int rx, int y, int rw,
                                  const char *text, uint32_t color888) {
    int tw = (int)strlen(text) * FONT_WIDTH;
    rb_draw_text(ctx, rx + (rw - tw) / 2, y, text, color888);
}

static void rb_draw_3d_border(rb_ctx_t *ctx, int x, int y, int w, int h,
                              int inset) {
    uint32_t light = inset ? CDE_BORDER_DARK  : CDE_BORDER_LIGHT;
    uint32_t dark  = inset ? CDE_BORDER_LIGHT : CDE_BORDER_DARK;
    rb_fill_rect(ctx, x,         y,         w, 2, light);
    rb_fill_rect(ctx, x,         y,         2, h, light);
    rb_fill_rect(ctx, x,         y + h - 2, w, 2, dark);
    rb_fill_rect(ctx, x + w - 2, y,         2, h, dark);
}

/* ------------------------------------------------------------------ */
/* Extension matching (case-insensitive)                               */
/* ------------------------------------------------------------------ */
static bool ext_matches(const char *name, const char **exts) {
    size_t nlen = strlen(name);
    for (int i = 0; exts[i]; i++) {
        size_t elen = strlen(exts[i]);
        if (nlen < elen) continue;
        const char *tail = name + nlen - elen;
        bool ok = true;
        for (size_t j = 0; j < elen; j++) {
            if (tolower((unsigned char)tail[j]) !=
                tolower((unsigned char)exts[i][j])) {
                ok = false; break;
            }
        }
        if (ok) return true;
    }
    return false;
}

/* VMS path chars: A-Z a-z 0-9 - _ $ .  (no spaces, parens, brackets, !) */
static bool filename_is_vms_safe(const char *name) {
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '$' || c == '.'))
            return false;
    }
    return true;
}

/* qsort comparator — sorts by filename only (after last ']') */
static int cmp_rom_paths(const void *a, const void *b) {
    const char *pa = *(const char **)a;
    const char *pb = *(const char **)b;
    const char *na = strrchr(pa, ']'); na = na ? na + 1 : pa;
    const char *nb = strrchr(pb, ']'); nb = nb ? nb + 1 : pb;
    return strcasecmp(na, nb);
}

/* ------------------------------------------------------------------ */
/* Public: rom_browser_scan                                            */
/* ------------------------------------------------------------------ */
char **rom_browser_scan(const char *rom_dir, const char **extensions,
                        size_t *count) {
    *count = 0;
    DIR *d = opendir(rom_dir);
    if (!d) return NULL;

    char   **result = NULL;
    size_t   cap    = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!ext_matches(ent->d_name, extensions)) continue;
        if (!filename_is_vms_safe(ent->d_name)) continue;

        size_t dir_len  = strlen(rom_dir);
        size_t name_len = strlen(ent->d_name);
        char  *path     = malloc(dir_len + name_len + 1);
        if (!path) continue;
        memcpy(path, rom_dir, dir_len);
        memcpy(path + dir_len, ent->d_name, name_len + 1);

        if (*count >= cap) {
            size_t  ncap = cap ? cap * 2 : 16;
            char  **tmp  = realloc(result, ncap * sizeof(char *));
            if (!tmp) { free(path); break; }
            result = tmp;
            cap    = ncap;
        }
        result[(*count)++] = path;
    }
    closedir(d);

    if (*count > 1)
        qsort(result, *count, sizeof(char *), cmp_rom_paths);

    return result;
}

/* Build title string with ROM count: "NES Launcher - 42 games" */
static void rb_make_display_title(char *buf, size_t sz, const char *base,
                                  size_t count) {
    snprintf(buf, sz, "%s - %zu game%s", base, count, count == 1 ? "" : "s");
}

/* ------------------------------------------------------------------ */
/* Public: rom_browser_run                                             */
/* ------------------------------------------------------------------ */
char *rom_browser_run(const rom_browser_config_t *cfg) {
    size_t  count = 0;
    char  **roms  = rom_browser_scan(cfg->rom_dir, cfg->extensions, &count);

    rb_ctx_t ctx = {
        .pixels = cfg->framebuffer->pixels,
        .w      = (int)cfg->framebuffer->w,
        .h      = (int)cfg->framebuffer->h,
    };

    /* Layout constants */
    const int WIN_X    = 30, WIN_Y = 30;
    const int WIN_W    = ctx.w - 60;
    const int WIN_H    = ctx.h - 60;
    const int TITLE_H  = 45;
    const int FOOTER_H = 42;
    const int LIST_Y   = WIN_Y + TITLE_H + 15;
    const int LIST_H   = WIN_H - TITLE_H - FOOTER_H - 20;
    const int ITEM_H   = FONT_HEIGHT + 10;
    const int PER_PAGE = LIST_H / ITEM_H;

    int    selected = 0;
    int    scroll   = 0;
    char  *result   = NULL;
    char   display_title[128];
    rb_make_display_title(display_title, sizeof(display_title), cfg->title, count);

    while (true) {
        /* Background + panel */
        rb_fill_rect(&ctx, 0, 0, ctx.w, ctx.h, CDE_BG_COLOR);
        rb_fill_rect(&ctx, WIN_X, WIN_Y, WIN_W, WIN_H, CDE_PANEL_COLOR);
        rb_draw_3d_border(&ctx, WIN_X, WIN_Y, WIN_W, WIN_H, 0);

        /* Title bar */
        rb_fill_rect(&ctx, WIN_X + 3, WIN_Y + 3, WIN_W - 6, TITLE_H, CDE_TITLE_BG);
        rb_draw_text(&ctx, WIN_X + 15, WIN_Y + 11, display_title, CDE_SELECTED_TEXT);

        /* List area */
        int lx = WIN_X + 15, ly = LIST_Y, lw = WIN_W - 30;
        rb_fill_rect(&ctx, lx, ly, lw, LIST_H, 0xFFFFFF);
        rb_draw_3d_border(&ctx, lx, ly, lw, LIST_H, 1);

        if (count == 0) {
            /* Convert VMS rom_dir (SD0:[ROMS.NES]) to SD-card path (ROMS/NES/) for display */
            char sd_path[64] = "";
            const char *bracket = strchr(cfg->rom_dir, '[');
            if (bracket) {
                size_t i = 0, j = 1;
                while (bracket[j] && bracket[j] != ']' && i < sizeof(sd_path) - 2) {
                    sd_path[i++] = (bracket[j] == '.') ? '/' : bracket[j];
                    j++;
                }
                sd_path[i++] = '/';
                sd_path[i]   = '\0';
            }
            char msg1[64], msg2[64];
            snprintf(msg1, sizeof(msg1), "No ROMs found.");
            snprintf(msg2, sizeof(msg2), "Copy files to SD card: %s",
                     sd_path[0] ? sd_path : cfg->rom_dir);
            int mid = ly + LIST_H / 2;
            rb_draw_text_centered(&ctx, lx, mid - FONT_HEIGHT - 2, lw, msg1, CDE_INACTIVE_TEXT);
            rb_draw_text_centered(&ctx, lx, mid + 2,               lw, msg2, CDE_INACTIVE_TEXT);
        } else {
            int vis_end = scroll + PER_PAGE;
            if (vis_end > (int)count) vis_end = (int)count;

            for (int i = scroll; i < vis_end; i++) {
                int iy = ly + 3 + (i - scroll) * ITEM_H;
                bool sel = (i == selected);

                if (sel)
                    rb_fill_rect(&ctx, lx + 2, iy, lw - 4, ITEM_H - 2,
                                 CDE_SELECTED_BG);

                /* Display filename only (after last ']') */
                const char *name = strrchr(roms[i], ']');
                name = name ? name + 1 : roms[i];
                rb_draw_text(&ctx, lx + 10,
                             iy + (ITEM_H - FONT_HEIGHT) / 2,
                             name,
                             sel ? CDE_SELECTED_TEXT : CDE_TEXT_COLOR);

                if (i < vis_end - 1)
                    rb_fill_rect(&ctx, lx, iy + ITEM_H - 1, lw, 1,
                                 CDE_BORDER_DARK);
            }

            /* Scrollbar */
            if ((int)count > PER_PAGE) {
                int sx = lx + lw - 23, sy = ly + 3, sh = LIST_H - 6;
                rb_fill_rect(&ctx, sx, sy, 20, sh, 0xD4D0C8);
                rb_draw_3d_border(&ctx, sx, sy, 20, sh, 1);
                int th = (sh * PER_PAGE) / (int)count;
                if (th < 20) th = 20;
                int ty = sy + ((sh - th) * scroll) / ((int)count - PER_PAGE);
                rb_fill_rect(&ctx, sx + 3, ty, 14, th, CDE_PANEL_COLOR);
                rb_draw_3d_border(&ctx, sx + 3, ty, 14, th, 0);
            }
        }

        /* Footer */
        rb_fill_rect(&ctx, WIN_X + 3, WIN_Y + WIN_H - FOOTER_H,
                     WIN_W - 6, FOOTER_H - 3, 0xD4D0C8);
        rb_draw_3d_border(&ctx, WIN_X + 3, WIN_Y + WIN_H - FOOTER_H,
                          WIN_W - 6, FOOTER_H - 3, 1);
        rb_draw_text(&ctx, WIN_X + 15, WIN_Y + WIN_H - FOOTER_H + 9,
                     "UP/DOWN: Navigate  ENTER: Launch  R: Reload  ESC: Back",
                     CDE_TEXT_COLOR);

        window_present(cfg->window, true, NULL, 0);

        event_t e = window_event_poll(cfg->window, true, 0);
        if (e.type == EVENT_QUIT) break;
        if (e.type != EVENT_KEY_DOWN) continue;

        keyboard_scancode_t sc = e.keyboard.scancode;

        if (sc == KEY_SCANCODE_ESCAPE) break;

        if (sc == KEY_SCANCODE_RETURN && count > 0) {
            result = roms[selected];
            roms[selected] = NULL;
            break;
        }

        if (sc == KEY_SCANCODE_R) {
            for (size_t i = 0; i < count; i++) free(roms[i]);
            free(roms);
            roms     = rom_browser_scan(cfg->rom_dir, cfg->extensions, &count);
            selected = 0;
            scroll   = 0;
            rb_make_display_title(display_title, sizeof(display_title), cfg->title, count);
            continue;
        }

        if ((sc == KEY_SCANCODE_UP || sc == KEY_SCANCODE_W) && count > 0) {
            if (selected == 0) {
                selected = (int)count - 1;
                scroll   = selected - PER_PAGE + 1;
                if (scroll < 0) scroll = 0;
            } else {
                selected--;
                if (selected < scroll) scroll = selected;
            }
        }

        if ((sc == KEY_SCANCODE_DOWN || sc == KEY_SCANCODE_S) && count > 0) {
            if (selected == (int)count - 1) {
                selected = 0;
                scroll   = 0;
            } else {
                selected++;
                if (selected >= scroll + PER_PAGE)
                    scroll = selected - PER_PAGE + 1;
            }
        }
    }

    for (size_t i = 0; i < count; i++) free(roms[i]);
    free(roms);
    return result;
}
