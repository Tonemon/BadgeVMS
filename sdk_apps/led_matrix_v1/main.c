/* led_matrix_v1/main.c
 *
 * SDL3 app (SDL_MAIN_USE_CALLBACKS=1) that drives a PCA9698-based 5x8 LED
 * matrix (device name "LEDMATRIX0") with three modes:
 *   0 = static  – show a single character from the text string
 *   1 = scroll  – scroll the entire text string left pixel-by-pixel
 *   2 = diag    – I2C bus scan + hardware test patterns
 *
 * Layout (720x720):
 *   y=0..149    LED matrix pixel-art preview (5 cols x 8 rows, 40px LEDs)
 *   y=160..     Mode-dependent content
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <badgevms/device.h>

/* -------------------------------------------------------------------------
 * 5x7 ASCII bitmap font (columns indexed by char-0x20).
 * Each entry is 5 bytes — one byte per column, bit 0 = top row, bit 6 = row 6.
 * Source: classic 5x7 dot-matrix font, public domain.
 * ------------------------------------------------------------------------- */
static const uint8_t font5x7[][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 }, /* 0x20 ' ' */
    { 0x00, 0x00, 0x5F, 0x00, 0x00 }, /* 0x21 '!' */
    { 0x00, 0x07, 0x00, 0x07, 0x00 }, /* 0x22 '"' */
    { 0x14, 0x7F, 0x14, 0x7F, 0x14 }, /* 0x23 '#' */
    { 0x24, 0x2A, 0x7F, 0x2A, 0x12 }, /* 0x24 '$' */
    { 0x23, 0x13, 0x08, 0x64, 0x62 }, /* 0x25 '%' */
    { 0x36, 0x49, 0x55, 0x22, 0x50 }, /* 0x26 '&' */
    { 0x00, 0x05, 0x03, 0x00, 0x00 }, /* 0x27 '\'' */
    { 0x00, 0x1C, 0x22, 0x41, 0x00 }, /* 0x28 '(' */
    { 0x00, 0x41, 0x22, 0x1C, 0x00 }, /* 0x29 ')' */
    { 0x14, 0x08, 0x3E, 0x08, 0x14 }, /* 0x2A '*' */
    { 0x08, 0x08, 0x3E, 0x08, 0x08 }, /* 0x2B '+' */
    { 0x00, 0x50, 0x30, 0x00, 0x00 }, /* 0x2C ',' */
    { 0x08, 0x08, 0x08, 0x08, 0x08 }, /* 0x2D '-' */
    { 0x00, 0x60, 0x60, 0x00, 0x00 }, /* 0x2E '.' */
    { 0x20, 0x10, 0x08, 0x04, 0x02 }, /* 0x2F '/' */
    { 0x3E, 0x51, 0x49, 0x45, 0x3E }, /* 0x30 '0' */
    { 0x00, 0x42, 0x7F, 0x40, 0x00 }, /* 0x31 '1' */
    { 0x42, 0x61, 0x51, 0x49, 0x46 }, /* 0x32 '2' */
    { 0x21, 0x41, 0x45, 0x4B, 0x31 }, /* 0x33 '3' */
    { 0x18, 0x14, 0x12, 0x7F, 0x10 }, /* 0x34 '4' */
    { 0x27, 0x45, 0x45, 0x45, 0x39 }, /* 0x35 '5' */
    { 0x3C, 0x4A, 0x49, 0x49, 0x30 }, /* 0x36 '6' */
    { 0x01, 0x71, 0x09, 0x05, 0x03 }, /* 0x37 '7' */
    { 0x36, 0x49, 0x49, 0x49, 0x36 }, /* 0x38 '8' */
    { 0x06, 0x49, 0x49, 0x29, 0x1E }, /* 0x39 '9' */
    { 0x00, 0x36, 0x36, 0x00, 0x00 }, /* 0x3A ':' */
    { 0x00, 0x56, 0x36, 0x00, 0x00 }, /* 0x3B ';' */
    { 0x08, 0x14, 0x22, 0x41, 0x00 }, /* 0x3C '<' */
    { 0x14, 0x14, 0x14, 0x14, 0x14 }, /* 0x3D '=' */
    { 0x00, 0x41, 0x22, 0x14, 0x08 }, /* 0x3E '>' */
    { 0x02, 0x01, 0x51, 0x09, 0x06 }, /* 0x3F '?' */
    { 0x32, 0x49, 0x79, 0x41, 0x3E }, /* 0x40 '@' */
    { 0x7E, 0x11, 0x11, 0x11, 0x7E }, /* 0x41 'A' */
    { 0x7F, 0x49, 0x49, 0x49, 0x36 }, /* 0x42 'B' */
    { 0x3E, 0x41, 0x41, 0x41, 0x22 }, /* 0x43 'C' */
    { 0x7F, 0x41, 0x41, 0x22, 0x1C }, /* 0x44 'D' */
    { 0x7F, 0x49, 0x49, 0x49, 0x41 }, /* 0x45 'E' */
    { 0x7F, 0x09, 0x09, 0x09, 0x01 }, /* 0x46 'F' */
    { 0x3E, 0x41, 0x49, 0x49, 0x7A }, /* 0x47 'G' */
    { 0x7F, 0x08, 0x08, 0x08, 0x7F }, /* 0x48 'H' */
    { 0x00, 0x41, 0x7F, 0x41, 0x00 }, /* 0x49 'I' */
    { 0x20, 0x40, 0x41, 0x3F, 0x01 }, /* 0x4A 'J' */
    { 0x7F, 0x08, 0x14, 0x22, 0x41 }, /* 0x4B 'K' */
    { 0x7F, 0x40, 0x40, 0x40, 0x40 }, /* 0x4C 'L' */
    { 0x7F, 0x02, 0x0C, 0x02, 0x7F }, /* 0x4D 'M' */
    { 0x7F, 0x04, 0x08, 0x10, 0x7F }, /* 0x4E 'N' */
    { 0x3E, 0x41, 0x41, 0x41, 0x3E }, /* 0x4F 'O' */
    { 0x7F, 0x09, 0x09, 0x09, 0x06 }, /* 0x50 'P' */
    { 0x3E, 0x41, 0x51, 0x21, 0x5E }, /* 0x51 'Q' */
    { 0x7F, 0x09, 0x19, 0x29, 0x46 }, /* 0x52 'R' */
    { 0x46, 0x49, 0x49, 0x49, 0x31 }, /* 0x53 'S' */
    { 0x01, 0x01, 0x7F, 0x01, 0x01 }, /* 0x54 'T' */
    { 0x3F, 0x40, 0x40, 0x40, 0x3F }, /* 0x55 'U' */
    { 0x1F, 0x20, 0x40, 0x20, 0x1F }, /* 0x56 'V' */
    { 0x3F, 0x40, 0x38, 0x40, 0x3F }, /* 0x57 'W' */
    { 0x63, 0x14, 0x08, 0x14, 0x63 }, /* 0x58 'X' */
    { 0x07, 0x08, 0x70, 0x08, 0x07 }, /* 0x59 'Y' */
    { 0x61, 0x51, 0x49, 0x45, 0x43 }, /* 0x5A 'Z' */
    { 0x00, 0x7F, 0x41, 0x41, 0x00 }, /* 0x5B '[' */
    { 0x02, 0x04, 0x08, 0x10, 0x20 }, /* 0x5C '\\' */
    { 0x00, 0x41, 0x41, 0x7F, 0x00 }, /* 0x5D ']' */
    { 0x04, 0x02, 0x01, 0x02, 0x04 }, /* 0x5E '^' */
    { 0x40, 0x40, 0x40, 0x40, 0x40 }, /* 0x5F '_' */
    { 0x00, 0x01, 0x02, 0x04, 0x00 }, /* 0x60 '`' */
    { 0x20, 0x54, 0x54, 0x54, 0x78 }, /* 0x61 'a' */
    { 0x7F, 0x48, 0x44, 0x44, 0x38 }, /* 0x62 'b' */
    { 0x38, 0x44, 0x44, 0x44, 0x20 }, /* 0x63 'c' */
    { 0x38, 0x44, 0x44, 0x48, 0x7F }, /* 0x64 'd' */
    { 0x38, 0x54, 0x54, 0x54, 0x18 }, /* 0x65 'e' */
    { 0x08, 0x7E, 0x09, 0x01, 0x02 }, /* 0x66 'f' */
    { 0x0C, 0x52, 0x52, 0x52, 0x3E }, /* 0x67 'g' */
    { 0x7F, 0x08, 0x04, 0x04, 0x78 }, /* 0x68 'h' */
    { 0x00, 0x44, 0x7D, 0x40, 0x00 }, /* 0x69 'i' */
    { 0x20, 0x40, 0x44, 0x3D, 0x00 }, /* 0x6A 'j' */
    { 0x7F, 0x10, 0x28, 0x44, 0x00 }, /* 0x6B 'k' */
    { 0x00, 0x41, 0x7F, 0x40, 0x00 }, /* 0x6C 'l' */
    { 0x7C, 0x04, 0x18, 0x04, 0x78 }, /* 0x6D 'm' */
    { 0x7C, 0x08, 0x04, 0x04, 0x78 }, /* 0x6E 'n' */
    { 0x38, 0x44, 0x44, 0x44, 0x38 }, /* 0x6F 'o' */
    { 0x7C, 0x14, 0x14, 0x14, 0x08 }, /* 0x70 'p' */
    { 0x08, 0x14, 0x14, 0x18, 0x7C }, /* 0x71 'q' */
    { 0x7C, 0x08, 0x04, 0x04, 0x08 }, /* 0x72 'r' */
    { 0x48, 0x54, 0x54, 0x54, 0x20 }, /* 0x73 's' */
    { 0x04, 0x3F, 0x44, 0x40, 0x20 }, /* 0x74 't' */
    { 0x3C, 0x40, 0x40, 0x20, 0x7C }, /* 0x75 'u' */
    { 0x1C, 0x20, 0x40, 0x20, 0x1C }, /* 0x76 'v' */
    { 0x3C, 0x40, 0x30, 0x40, 0x3C }, /* 0x77 'w' */
    { 0x44, 0x28, 0x10, 0x28, 0x44 }, /* 0x78 'x' */
    { 0x0C, 0x50, 0x50, 0x50, 0x3C }, /* 0x79 'y' */
    { 0x44, 0x64, 0x54, 0x4C, 0x44 }, /* 0x7A 'z' */
    { 0x00, 0x08, 0x36, 0x41, 0x00 }, /* 0x7B '{' */
    { 0x00, 0x00, 0x7F, 0x00, 0x00 }, /* 0x7C '|' */
    { 0x00, 0x41, 0x36, 0x08, 0x00 }, /* 0x7D '}' */
    { 0x10, 0x08, 0x08, 0x10, 0x08 }, /* 0x7E '~' */
};

#define SCREEN_WIDTH  720
#define SCREEN_HEIGHT 720

/* LED matrix display constants.
 * Rectangular cells (wider than tall) to fill the square screen — the 5:8
 * matrix aspect ratio means square cells leave too much horizontal space. */
#define MATRIX_COLS     5
#define MATRIX_ROWS     8
#define LED_W           106   /* cell width  (pixels) */
#define LED_H            62   /* cell height (pixels) */
#define LED_GAP          10   /* gap between cells    */
#define LED_CELL_W      (LED_W + LED_GAP)   /* 116 */
#define LED_CELL_H      (LED_H + LED_GAP)   /* 72  */

#define MATRIX_PX_W     (MATRIX_COLS * LED_CELL_W - LED_GAP)  /* 570 */
#define MATRIX_PX_H     (MATRIX_ROWS * LED_CELL_H - LED_GAP)  /* 566 */
#define MATRIX_ORIGIN_X ((SCREEN_WIDTH  - MATRIX_PX_W) / 2)   /* 75  */
#define MATRIX_ORIGIN_Y 16

#define SCROLL_INTERVAL_MS 50
#define MAX_TEXT_LEN       127

/* -------------------------------------------------------------------------
 * Diagnostic test patterns
 * ------------------------------------------------------------------------- */
#define DIAG_NUM_PATTERNS 17

static const char *diag_pattern_names[DIAG_NUM_PATTERNS] = {
    "All ON",
    "All OFF",
    "Column 0 only",
    "Column 1 only",
    "Column 2 only",
    "Column 3 only",
    "Column 4 only",
    "Row 0 only",
    "Row 1 only",
    "Row 2 only",
    "Row 3 only",
    "Row 4 only",
    "Row 5 only",
    "Row 6 only",
    "Row 7 only",
    "Checkerboard A",
    "Checkerboard B",
};

static void get_diag_columns(int pattern, uint8_t cols[5]) {
    for (int i = 0; i < 5; i++) cols[i] = 0;

    if (pattern == 0) {
        for (int i = 0; i < 5; i++) cols[i] = 0xFF;
    } else if (pattern == 1) {
        /* all off — already zeroed */
    } else if (pattern >= 2 && pattern <= 6) {
        cols[pattern - 2] = 0xFF;
    } else if (pattern >= 7 && pattern <= 14) {
        uint8_t row_mask = (uint8_t)(1u << (pattern - 7));
        for (int i = 0; i < 5; i++) cols[i] = row_mask;
    } else if (pattern == 15) {
        for (int i = 0; i < 5; i++) cols[i] = (i & 1) ? 0xAA : 0x55;
    } else if (pattern == 16) {
        for (int i = 0; i < 5; i++) cols[i] = (i & 1) ? 0x55 : 0xAA;
    }
}

/* -------------------------------------------------------------------------
 * App state
 * ------------------------------------------------------------------------- */
typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    device_t     *led_matrix;   /* LEDMATRIX0 or NULL */

    char          text[MAX_TEXT_LEN + 1];
    int           text_len;
    int           mode;           /* 0=static, 1=scroll, 2=diag */

    /* static mode */
    int           static_char_idx;

    /* scroll mode */
    int           scroll_x;
    Uint64        last_scroll_tick;

    /* text editing */
    bool          editing;

    /* diag mode */
    int           diag_pattern;
    i2c_scanresult_t diag_scan_results[128];
    int           diag_scan_count;
    bool          diag_hw_responding;  /* last _write returned > 0 */
    uint8_t       diag_pca_addrs[8];   /* which 0x20-0x27 ACKed on scan */
    int           diag_pca_addr_count;
} AppState;

/* -------------------------------------------------------------------------
 * Normal mode column helpers
 * ------------------------------------------------------------------------- */
static void get_columns_static(AppState *s, uint8_t cols[5]) {
    for (int c = 0; c < 5; c++) cols[c] = 0;
    if (s->text_len == 0) return;
    int idx = s->static_char_idx;
    if (idx < 0 || idx >= s->text_len) return;
    unsigned char ch = (unsigned char)s->text[idx];
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const uint8_t *glyph = font5x7[ch - 0x20];
    for (int c = 0; c < 5; c++) cols[c] = glyph[c] & 0x7F;
}

static uint8_t scroll_col_at(AppState *s, int col_pixel) {
    if (s->text_len == 0) return 0;
    int total = s->text_len * 6;
    col_pixel = col_pixel % total;
    if (col_pixel < 0) col_pixel += total;
    int char_idx = col_pixel / 6;
    int col_in_char = col_pixel % 6;
    if (col_in_char == 5) return 0;
    unsigned char ch = (unsigned char)s->text[char_idx];
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    return font5x7[ch - 0x20][col_in_char] & 0x7F;
}

static void get_columns_scroll(AppState *s, uint8_t cols[5]) {
    for (int c = 0; c < 5; c++) cols[c] = scroll_col_at(s, s->scroll_x + c);
}

static void update_matrix(AppState *s) {
    uint8_t cols[5];
    if (s->mode == 0) {
        get_columns_static(s, cols);
    } else if (s->mode == 1) {
        get_columns_scroll(s, cols);
    } else {
        get_diag_columns(s->diag_pattern, cols);
    }
    if (s->led_matrix) {
        ssize_t n = s->led_matrix->_write(s->led_matrix, 0, cols, 5);
        s->diag_hw_responding = (n > 0);
    }
}

/* -------------------------------------------------------------------------
 * I2C bus scan (diag mode)
 * ------------------------------------------------------------------------- */
static void run_diag_scan(AppState *s) {
    s->diag_scan_count = 0;
    s->diag_pca_addr_count = 0;

    device_t *bus = device_get("I2CBUS0");
    if (!bus) return;
    i2c_bus_device_t *i2c_bus = (i2c_bus_device_t *)bus;
    s->diag_scan_count = i2c_bus->_scan(i2c_bus, s->diag_scan_results, 128);

    /* Check which PCA9698 addresses (0x20-0x27) responded */
    for (int i = 0; i < s->diag_scan_count; i++) {
        uint8_t addr = s->diag_scan_results[i].address;
        if (addr >= 0x20 && addr <= 0x27 && s->diag_pca_addr_count < 8) {
            s->diag_pca_addrs[s->diag_pca_addr_count++] = addr;
        }
    }
}

/* -------------------------------------------------------------------------
 * Rendering helpers
 * ------------------------------------------------------------------------- */
static void render_led_matrix(AppState *s, uint8_t cols[5]) {
    SDL_Renderer *r = s->renderer;
    for (int col = 0; col < MATRIX_COLS; col++) {
        for (int row = 0; row < MATRIX_ROWS; row++) {
            bool on = (cols[col] >> row) & 1;
            SDL_FRect rect = {
                (float)(MATRIX_ORIGIN_X + col * LED_CELL_W),
                (float)(MATRIX_ORIGIN_Y + row * LED_CELL_H),
                (float)LED_W,
                (float)LED_H
            };
            SDL_SetRenderDrawColor(r,
                on ? 0x00 : 0x1A,
                on ? 0xFF : 0x1A,
                0x00,
                SDL_ALPHA_OPAQUE);
            SDL_RenderFillRect(r, &rect);
        }
    }
}

static void render_rect_outline(SDL_Renderer *r, float x, float y, float w, float h, float thick) {
    SDL_FRect rects[4] = {
        { x, y, w, thick },
        { x, y + h - thick, w, thick },
        { x, y, thick, h },
        { x + w - thick, y, thick, h }
    };
    SDL_RenderFillRects(r, rects, 4);
}

static void render_text_centered(SDL_Renderer *r, float cx, float y, const char *text) {
    float w = (float)(SDL_strlen(text) * 8);
    SDL_RenderDebugText(r, cx - w * 0.5f, y, text);
}

/* -------------------------------------------------------------------------
 * Diag screen rendering — compact strip below the matrix (y=584..720)
 * ------------------------------------------------------------------------- */
#define CTRL_Y_TOP  (MATRIX_ORIGIN_Y + MATRIX_PX_H + 6)   /* ~588 */

static void render_diag(AppState *s) {
    SDL_Renderer *r = s->renderer;

    /* Build scan address string */
    char scan_addrs[160] = {0};
    int  scan_pos = 0;
    for (int i = 0; i < s->diag_scan_count && i < 20; i++) {
        int n = SDL_snprintf(scan_addrs + scan_pos,
            sizeof(scan_addrs) - (size_t)scan_pos,
            "0x%02X ", s->diag_scan_results[i].address);
        scan_pos += n;
    }

    float y = (float)CTRL_Y_TOP;

    /* Line 1: driver registration status (always succeeds — not a hw check) */
    SDL_SetRenderDrawColor(r, 0x66, 0x66, 0x66, SDL_ALPHA_OPAQUE);
    SDL_RenderDebugTextFormat(r, 40.0f, y, "Driver: %s",
        s->led_matrix ? "registered (addr 0x20)" : "NOT registered");

    /* Hardware response — the real indicator */
    if (!s->led_matrix) {
        SDL_SetRenderDrawColor(r, 0xFF, 0x44, 0x44, SDL_ALPHA_OPAQUE);
        SDL_RenderDebugText(r, 340.0f, y, "Hardware: N/A");
    } else if (s->diag_hw_responding) {
        SDL_SetRenderDrawColor(r, 0x00, 0xCC, 0x00, SDL_ALPHA_OPAQUE);
        SDL_RenderDebugText(r, 340.0f, y, "Hardware: RESPONDING");
    } else {
        SDL_SetRenderDrawColor(r, 0xFF, 0x44, 0x44, SDL_ALPHA_OPAQUE);
        SDL_RenderDebugText(r, 340.0f, y, "Hardware: NOT RESPONDING");
    }

    SDL_SetRenderDrawColor(r, 0x55, 0x55, 0x55, SDL_ALPHA_OPAQUE);
    SDL_RenderDebugTextFormat(r, 590.0f, y, "R=rescan");
    y += 14.0f;

    /* Line 2: full bus scan */
    SDL_SetRenderDrawColor(r, 0x77, 0x99, 0xBB, SDL_ALPHA_OPAQUE);
    SDL_RenderDebugTextFormat(r, 40.0f, y, "I2C bus (%d): %s",
        s->diag_scan_count, scan_pos > 0 ? scan_addrs : "(none)");
    y += 14.0f;

    /* Line 3: PCA9698 address range check */
    if (s->diag_pca_addr_count > 0) {
        char pca_str[48] = {0};
        int pos = 0;
        for (int i = 0; i < s->diag_pca_addr_count; i++) {
            int n = SDL_snprintf(pca_str + pos, sizeof(pca_str) - (size_t)pos,
                "0x%02X ", s->diag_pca_addrs[i]);
            pos += n;
        }
        SDL_SetRenderDrawColor(r, 0x00, 0xFF, 0x88, SDL_ALPHA_OPAQUE);
        SDL_RenderDebugTextFormat(r, 40.0f, y,
            "PCA9698 range (0x20-0x27): FOUND at %s", pca_str);
        if (s->diag_pca_addrs[0] != 0x20) {
            SDL_SetRenderDrawColor(r, 0xFF, 0xCC, 0x00, SDL_ALPHA_OPAQUE);
            SDL_RenderDebugTextFormat(r, 40.0f, y + 13.0f,
                "  ^ addr mismatch! driver uses 0x20, update why2025_firmware.c");
            y += 13.0f;
        }
    } else {
        SDL_SetRenderDrawColor(r, 0xFF, 0x44, 0x44, SDL_ALPHA_OPAQUE);
        SDL_RenderDebugText(r, 40.0f, y,
            "PCA9698 range (0x20-0x27): NOT FOUND — check wiring/power");
    }
    y += 16.0f;

    /* Separator */
    SDL_SetRenderDrawColor(r, 0x2A, 0x2A, 0x2A, SDL_ALPHA_OPAQUE);
    SDL_FRect sep = { 40.0f, y, 640.0f, 1.0f };
    SDL_RenderFillRect(r, &sep);
    y += 4.0f;

    /* Pattern list — show 5 visible rows, scroll window around selection */
    int visible = 5;
    int start = s->diag_pattern - 2;
    if (start < 0) start = 0;
    if (start > DIAG_NUM_PATTERNS - visible) start = DIAG_NUM_PATTERNS - visible;

    for (int i = start; i < start + visible && i < DIAG_NUM_PATTERNS; i++) {
        bool sel = (i == s->diag_pattern);
        if (sel) {
            SDL_SetRenderDrawColor(r, 0x00, 0x60, 0xAA, SDL_ALPHA_OPAQUE);
            SDL_FRect hl = { 36.0f, y - 1.0f, 420.0f, 13.0f };
            SDL_RenderFillRect(r, &hl);
            SDL_SetRenderDrawColor(r, 0xFF, 0xFF, 0xFF, SDL_ALPHA_OPAQUE);
        } else {
            SDL_SetRenderDrawColor(r, 0x66, 0x66, 0x66, SDL_ALPHA_OPAQUE);
        }
        SDL_RenderDebugTextFormat(r, 40.0f, y, "%s %s",
            sel ? ">" : " ", diag_pattern_names[i]);
        y += 13.0f;
    }
    y += 2.0f;

    /* Key hints */
    SDL_SetRenderDrawColor(r, 0x44, 0x44, 0x44, SDL_ALPHA_OPAQUE);
    render_text_centered(r, 360.0f, y,
        "UP/DOWN pattern   R rescan   D exit diag   ESC quit");
}

/* -------------------------------------------------------------------------
 * Normal mode rendering — compact strip below the matrix (y≈588..720)
 * ------------------------------------------------------------------------- */
static void render_normal(AppState *s, uint8_t cols[5]) {
    SDL_Renderer *r = s->renderer;
    float y = (float)CTRL_Y_TOP;

    /* Line 1: device status + mode */
    SDL_SetRenderDrawColor(r, 0x66, 0x66, 0x66, SDL_ALPHA_OPAQUE);
    SDL_RenderDebugTextFormat(r, 40.0f, y, "LEDMATRIX0: %s",
        !s->led_matrix         ? "no driver"      :
        s->diag_hw_responding  ? "responding"      : "driver only (D=diag)");
    if (s->mode == 0) {
        SDL_SetRenderDrawColor(r, 0x00, 0xCC, 0x00, SDL_ALPHA_OPAQUE);
        SDL_RenderDebugText(r, 400.0f, y, "[ STATIC ]  M=scroll");
    } else {
        SDL_SetRenderDrawColor(r, 0xFF, 0xAA, 0x00, SDL_ALPHA_OPAQUE);
        SDL_RenderDebugText(r, 400.0f, y, "[ SCROLL ]  M=static");
    }
    y += 16.0f;

    /* Line 2: text content */
    SDL_SetRenderDrawColor(r, 0x55, 0x55, 0x55, SDL_ALPHA_OPAQUE);
    SDL_RenderDebugText(r, 40.0f, y, "Text:");
    if (s->text_len > 0) {
        SDL_SetRenderDrawColor(r, 0xDD, 0xDD, 0xDD, SDL_ALPHA_OPAQUE);
        int max_chars = 76;
        const char *display = s->text;
        if (s->text_len > max_chars) display = s->text + (s->text_len - max_chars);
        SDL_RenderDebugText(r, 84.0f, y, display);
        if (s->editing) {
            float cx = 84.0f + (float)(SDL_strlen(display) * 8);
            SDL_RenderDebugText(r, cx, y, "_");
        }
    } else {
        SDL_SetRenderDrawColor(r, 0x44, 0x44, 0x44, SDL_ALPHA_OPAQUE);
        SDL_RenderDebugText(r, 84.0f, y, s->editing ? "_" : "(empty — press T to type)");
    }
    y += 16.0f;

    /* Line 3: context hint */
    SDL_SetRenderDrawColor(r, 0x44, 0x44, 0x44, SDL_ALPHA_OPAQUE);
    if (s->editing) {
        SDL_RenderDebugText(r, 40.0f, y,
            "Editing — BACKSPACE delete   ENTER/ESC done");
    } else if (s->mode == 0 && s->text_len > 0) {
        char buf[64];
        SDL_snprintf(buf, sizeof(buf), "Char %d/%d: '%c'   LEFT/RIGHT to navigate",
            s->static_char_idx + 1, s->text_len,
            (unsigned char)s->text[s->static_char_idx] >= 0x20
                ? s->text[s->static_char_idx] : '?');
        SDL_RenderDebugText(r, 40.0f, y, buf);
    } else {
        SDL_RenderDebugText(r, 40.0f, y, "T edit   M mode   D diagnostics   ESC quit");
    }
    y += 16.0f;

    /* Line 4: key hints (only when not editing and not already shown above) */
    if (!s->editing && !(s->mode == 0 && s->text_len > 0)) {
        /* already covered in line 3 */
    } else if (!s->editing) {
        SDL_SetRenderDrawColor(r, 0x33, 0x33, 0x33, SDL_ALPHA_OPAQUE);
        SDL_RenderDebugText(r, 40.0f, y, "T edit   M mode   D diagnostics   ESC quit");
    }

    (void)cols;
}

/* -------------------------------------------------------------------------
 * SDL callbacks
 * ------------------------------------------------------------------------- */

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
    (void)argc; (void)argv;

    if (!SDL_SetAppMetadata("LED Matrix v1", "1.0", "com.badgevms.led_matrix_v1")) {
        return SDL_APP_FAILURE;
    }

    AppState *s = (AppState *)SDL_calloc(1, sizeof(AppState));
    if (!s) return SDL_APP_FAILURE;
    *appstate = s;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    s->window = SDL_CreateWindow("LED Matrix v1", SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_FULLSCREEN);
    if (!s->window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    s->renderer = SDL_CreateRenderer(s->window, NULL);
    if (!s->renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    s->led_matrix = device_get("LEDMATRIX0");
    if (!s->led_matrix) {
        SDL_Log("LEDMATRIX0 not found — preview-only mode");
    }

    const char *default_text = "Hello";
    SDL_strlcpy(s->text, default_text, sizeof(s->text));
    s->text_len = (int)SDL_strlen(s->text);
    s->mode = 0;
    s->static_char_idx = 0;
    s->scroll_x = 0;
    s->last_scroll_tick = SDL_GetTicks();
    s->editing = false;
    s->diag_pattern = 0;

    run_diag_scan(s);   /* prime scan so status label is accurate from boot */
    update_matrix(s);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    AppState *s = (AppState *)appstate;

    switch (event->type) {
        case SDL_EVENT_QUIT:
            return SDL_APP_SUCCESS;

        case SDL_EVENT_TEXT_INPUT:
            if (s->editing && s->mode != 2) {
                const char *input = event->text.text;
                while (*input && s->text_len < MAX_TEXT_LEN) {
                    unsigned char ch = (unsigned char)*input;
                    if (ch >= 0x20 && ch <= 0x7E) {
                        s->text[s->text_len++] = (char)ch;
                        s->text[s->text_len] = '\0';
                    }
                    input++;
                }
                if (s->text_len > 0 && s->static_char_idx >= s->text_len)
                    s->static_char_idx = s->text_len - 1;
                update_matrix(s);
            }
            break;

        case SDL_EVENT_KEY_DOWN: {
            SDL_Scancode sc = event->key.scancode;

            /* Text editing eats most keys */
            if (s->editing) {
                if (sc == SDL_SCANCODE_BACKSPACE) {
                    if (s->text_len > 0) {
                        s->text[--s->text_len] = '\0';
                        if (s->static_char_idx >= s->text_len && s->text_len > 0)
                            s->static_char_idx = s->text_len - 1;
                        else if (s->text_len == 0)
                            s->static_char_idx = 0;
                        update_matrix(s);
                    }
                } else if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_RETURN) {
                    s->editing = false;
                    SDL_StopTextInput(s->window);
                }
                break;
            }

            /* Diag mode keys */
            if (s->mode == 2) {
                switch (sc) {
                    case SDL_SCANCODE_ESCAPE:
                        return SDL_APP_SUCCESS;
                    case SDL_SCANCODE_D:
                        s->mode = 0;
                        update_matrix(s);
                        break;
                    case SDL_SCANCODE_R:
                        run_diag_scan(s);
                        break;
                    case SDL_SCANCODE_UP:
                        s->diag_pattern--;
                        if (s->diag_pattern < 0) s->diag_pattern = DIAG_NUM_PATTERNS - 1;
                        update_matrix(s);
                        break;
                    case SDL_SCANCODE_DOWN:
                        s->diag_pattern++;
                        if (s->diag_pattern >= DIAG_NUM_PATTERNS) s->diag_pattern = 0;
                        update_matrix(s);
                        break;
                    default:
                        break;
                }
                break;
            }

            /* Normal mode keys */
            switch (sc) {
                case SDL_SCANCODE_ESCAPE:
                    return SDL_APP_SUCCESS;

                case SDL_SCANCODE_M:
                    s->mode = s->mode == 0 ? 1 : 0;
                    s->scroll_x = 0;
                    s->last_scroll_tick = SDL_GetTicks();
                    update_matrix(s);
                    break;

                case SDL_SCANCODE_D:
                    s->mode = 2;
                    run_diag_scan(s);
                    update_matrix(s);
                    break;

                case SDL_SCANCODE_T:
                    s->editing = true;
                    SDL_StartTextInput(s->window);
                    break;

                case SDL_SCANCODE_LEFT:
                    if (s->mode == 0 && s->text_len > 0) {
                        s->static_char_idx--;
                        if (s->static_char_idx < 0) s->static_char_idx = s->text_len - 1;
                        update_matrix(s);
                    }
                    break;

                case SDL_SCANCODE_RIGHT:
                    if (s->mode == 0 && s->text_len > 0) {
                        s->static_char_idx++;
                        if (s->static_char_idx >= s->text_len) s->static_char_idx = 0;
                        update_matrix(s);
                    }
                    break;

                default:
                    break;
            }
            break;
        }

        default:
            break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    AppState *s = (AppState *)appstate;
    SDL_Renderer *r = s->renderer;
    Uint64 now = SDL_GetTicks();

    /* Advance scroll */
    if (s->mode == 1 && s->text_len > 0) {
        while (now - s->last_scroll_tick >= SCROLL_INTERVAL_MS) {
            s->scroll_x++;
            int total = s->text_len * 6;
            if (s->scroll_x >= total) s->scroll_x = 0;
            s->last_scroll_tick += SCROLL_INTERVAL_MS;
        }
        update_matrix(s);
    }

    /* Gather current columns for preview */
    uint8_t cols[5];
    if (s->mode == 0) {
        get_columns_static(s, cols);
    } else if (s->mode == 1) {
        get_columns_scroll(s, cols);
    } else {
        get_diag_columns(s->diag_pattern, cols);
    }

    /* Background */
    SDL_SetRenderDrawColor(r, 0x0D, 0x0D, 0x0D, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(r);

    /* LED matrix preview (always shown) */
    render_led_matrix(s, cols);

    /* Mode-specific content */
    if (s->mode == 2) {
        render_diag(s);
    } else {
        render_normal(s, cols);
    }

    SDL_RenderPresent(r);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    (void)result;
    if (appstate) {
        AppState *s = (AppState *)appstate;
        if (s->editing) SDL_StopTextInput(s->window);
        SDL_DestroyRenderer(s->renderer);
        SDL_DestroyWindow(s->window);
        SDL_free(s);
    }
}
