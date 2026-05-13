/* led_matrix_v1/main.c
 *
 * SDL3 app (SDL_MAIN_USE_CALLBACKS=1) that drives a PCA9698-based 12×20 LED
 * matrix (device name "LEDMATRIX0") with three modes:
 *   0 = static  – show up to 4 characters from the text string
 *   1 = scroll  – scroll text left pixel-by-pixel
 *   2 = diag    – I2C bus scan + hardware test patterns
 *
 * Hardware: PCA9698 at 0x20 (7-bit) / 0x40 (8-bit).  Bank layout:
 *   Bank 0 (IO0): PA0-PA7   = columns 0-7   (cathode, active LOW)
 *   Bank 1 (IO1): PA8-PA15  = columns 8-15  (cathode, active LOW)
 *   Bank 2 (IO2): PA16-PA19 = cols 16-19 (bits 0-3), bits 4-7 unused
 *   Bank 3 (IO3): PB0-PB7   = rows 0-7    (anode, active HIGH)
 *   Bank 4 (IO4): PB8-PB11  = rows 8-11 (bits 0-3), bits 4-7 unused
 * Drive: assert one PB row HIGH, write 20-bit column mask (LOW=ON), ≥1 kHz.
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
 * 5×7 ASCII bitmap font (ASCII 0x20-0x7E).
 * Each entry: 5 bytes, one per column.  Bit 0 = top row, bit 6 = row 6.
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

/* -------------------------------------------------------------------------
 * Screen / matrix geometry
 * ------------------------------------------------------------------------- */
#define SCREEN_WIDTH   720
#define SCREEN_HEIGHT  720
#define MATRIX_ROWS     12
#define MATRIX_COLS     20
#define LED_W           32   /* cell width  px */
#define LED_H           42   /* cell height px */
#define LED_GAP          4
#define LED_CW          (LED_W + LED_GAP)              /* 36  */
#define LED_CH          (LED_H + LED_GAP)              /* 46  */
#define MATRIX_PX_W     (MATRIX_COLS * LED_CW - LED_GAP)   /* 716 */
#define MATRIX_PX_H     (MATRIX_ROWS * LED_CH - LED_GAP)   /* 548 */
#define MATRIX_X0       ((SCREEN_WIDTH  - MATRIX_PX_W) / 2) /* 2 */
#define MATRIX_Y0       10
#define CTRL_Y          (MATRIX_Y0 + MATRIX_PX_H + 10) /* 568 */

/* -------------------------------------------------------------------------
 * Font / display constants
 * 7-row font centered in 12 LED rows: top padding=2, bottom padding=3.
 * 4 chars × 5 px = 20 px = full matrix width (no inter-char gap).
 * ------------------------------------------------------------------------- */
#define FONT_W           5
#define FONT_H           7
#define FONT_ROW_OFF     2
#define CHARS_VISIBLE    4
#define SCROLL_INTERVAL_MS 50
#define MAX_TEXT_LEN     127

/* -------------------------------------------------------------------------
 * Diagnostic patterns:
 *   0         = All ON
 *   1         = All OFF
 *   2..21     = Column 0-19
 *   22..33    = Row 0-11
 *   34        = Checkerboard A
 *   35        = Checkerboard B
 * ------------------------------------------------------------------------- */
#define DIAG_NUM_PATTERNS (2 + MATRIX_COLS + MATRIX_ROWS + 2)  /* 36 */

/* -------------------------------------------------------------------------
 * Shared framebuffer + multiplexing thread
 *
 * The background thread continuously cycles all 12 rows at ~1.5 kHz
 * (12 rows × ~56 µs/row at 1 MHz I2C).  The main SDL thread updates the
 * framebuffer; the thread reads it under a mutex.
 * ------------------------------------------------------------------------- */
static SDL_Mutex    *g_fb_mutex          = NULL;
static uint8_t       g_framebuf[MATRIX_ROWS][MATRIX_COLS]; /* 1 = LED on */
static volatile bool g_multiplex_running = false;
static volatile bool g_multiplex_paused  = false; /* set while I2C scanning */
static volatile bool g_hw_ok             = false;  /* last _write returned > 0 */

static void build_banks_for_row(int row, const uint8_t col[MATRIX_COLS], uint8_t banks[5]) {
    /* Default: all column cathodes HIGH (off), all row anodes LOW (off) */
    banks[0] = 0xFF; banks[1] = 0xFF; banks[2] = 0xFF;
    banks[3] = 0x00; banks[4] = 0x00;

    /* Cols 0-7  → bank 0 bits 0-7  (active LOW) */
    for (int c = 0; c < 8; c++)
        if (col[c]) banks[0] &= ~(uint8_t)(1u << c);
    /* Cols 8-15 → bank 1 bits 0-7 */
    for (int c = 0; c < 8; c++)
        if (col[c + 8]) banks[1] &= ~(uint8_t)(1u << c);
    /* Cols 16-19 → bank 2 bits 0-3 */
    for (int c = 0; c < 4; c++)
        if (col[c + 16]) banks[2] &= ~(uint8_t)(1u << c);

    /* Active row (active HIGH) */
    if (row < 8)
        banks[3] = (uint8_t)(1u << row);
    else
        banks[4] = (uint8_t)(1u << (row - 8));
}

static int SDLCALL multiplex_thread_fn(void *userdata) {
    device_t *dev = (device_t *)userdata;
    int row = 0;
    while (g_multiplex_running) {
        if (g_multiplex_paused) {
            SDL_Delay(1);
            continue;
        }
        uint8_t col[MATRIX_COLS];
        SDL_LockMutex(g_fb_mutex);
        SDL_memcpy(col, g_framebuf[row], MATRIX_COLS);
        SDL_UnlockMutex(g_fb_mutex);

        uint8_t banks[5];
        build_banks_for_row(row, col, banks);
        ssize_t n = dev->_write(dev, 0, banks, 5);
        g_hw_ok = (n > 0);

        row = (row + 1) % MATRIX_ROWS;
    }
    /* Turn off all LEDs on exit */
    uint8_t off[5] = {0xFF, 0xFF, 0xFF, 0x00, 0x00};
    dev->_write(dev, 0, off, 5);
    return 0;
}

/* -------------------------------------------------------------------------
 * App state
 * ------------------------------------------------------------------------- */
typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    device_t     *led_matrix;
    SDL_Thread   *mux_thread;

    char   text[MAX_TEXT_LEN + 1];
    int    text_len;
    int    mode;             /* 0=static, 1=scroll, 2=diag */

    int    static_char_idx;  /* first char shown in static mode (0..text_len-1) */

    int    scroll_x;
    Uint64 last_scroll_tick;

    bool   editing;

    int    diag_pattern;
    i2c_scanresult_t diag_scan_results[128];
    int    diag_scan_count;
    uint8_t diag_pca_addrs[8];
    int    diag_pca_addr_count;
} AppState;

/* -------------------------------------------------------------------------
 * Framebuffer update helpers
 * ------------------------------------------------------------------------- */
static void fb_update_static(AppState *s) {
    uint8_t fb[MATRIX_ROWS][MATRIX_COLS];
    SDL_memset(fb, 0, sizeof(fb));
    for (int ci = 0; ci < CHARS_VISIBLE; ci++) {
        int ti = s->static_char_idx + ci;
        if (ti >= s->text_len) break;
        unsigned char ch = (unsigned char)s->text[ti];
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        int col_off = ci * FONT_W;
        for (int fc = 0; fc < FONT_W; fc++)
            for (int fr = 0; fr < FONT_H; fr++)
                if ((font5x7[ch - 0x20][fc] >> fr) & 1)
                    fb[fr + FONT_ROW_OFF][col_off + fc] = 1;
    }
    SDL_LockMutex(g_fb_mutex);
    SDL_memcpy(g_framebuf, fb, sizeof(fb));
    SDL_UnlockMutex(g_fb_mutex);
}

static void fb_update_scroll(AppState *s) {
    uint8_t fb[MATRIX_ROWS][MATRIX_COLS];
    SDL_memset(fb, 0, sizeof(fb));
    if (s->text_len > 0) {
        int total = s->text_len * 6; /* 5 px char + 1 px gap */
        for (int c = 0; c < MATRIX_COLS; c++) {
            int px = ((s->scroll_x + c) % total + total) % total;
            int ci = px / 6;
            int col_in = px % 6;
            if (col_in >= FONT_W) continue; /* inter-char gap */
            unsigned char ch = (unsigned char)s->text[ci];
            if (ch < 0x20 || ch > 0x7E) ch = '?';
            for (int fr = 0; fr < FONT_H; fr++)
                if ((font5x7[ch - 0x20][col_in] >> fr) & 1)
                    fb[fr + FONT_ROW_OFF][c] = 1;
        }
    }
    SDL_LockMutex(g_fb_mutex);
    SDL_memcpy(g_framebuf, fb, sizeof(fb));
    SDL_UnlockMutex(g_fb_mutex);
}

static void fb_update_diag(int pattern) {
    uint8_t fb[MATRIX_ROWS][MATRIX_COLS];
    SDL_memset(fb, 0, sizeof(fb));
    if (pattern == 0) {
        SDL_memset(fb, 1, sizeof(fb));
    } else if (pattern >= 2 && pattern < 2 + MATRIX_COLS) {
        int col = pattern - 2;
        for (int r = 0; r < MATRIX_ROWS; r++) fb[r][col] = 1;
    } else if (pattern >= 2 + MATRIX_COLS && pattern < 2 + MATRIX_COLS + MATRIX_ROWS) {
        int row = pattern - 2 - MATRIX_COLS;
        for (int c = 0; c < MATRIX_COLS; c++) fb[row][c] = 1;
    } else if (pattern == 2 + MATRIX_COLS + MATRIX_ROWS) {
        for (int r = 0; r < MATRIX_ROWS; r++)
            for (int c = 0; c < MATRIX_COLS; c++)
                fb[r][c] = (uint8_t)((r + c) % 2);
    } else if (pattern == 2 + MATRIX_COLS + MATRIX_ROWS + 1) {
        for (int r = 0; r < MATRIX_ROWS; r++)
            for (int c = 0; c < MATRIX_COLS; c++)
                fb[r][c] = (uint8_t)((r + c + 1) % 2);
    }
    SDL_LockMutex(g_fb_mutex);
    SDL_memcpy(g_framebuf, fb, sizeof(fb));
    SDL_UnlockMutex(g_fb_mutex);
}

static void update_framebuf(AppState *s) {
    if (s->mode == 0)      fb_update_static(s);
    else if (s->mode == 1) fb_update_scroll(s);
    else                   fb_update_diag(s->diag_pattern);
}

/* -------------------------------------------------------------------------
 * I2C bus scan — pauses multiplexing thread to avoid bus contention
 * ------------------------------------------------------------------------- */
static void run_diag_scan(AppState *s) {
    g_multiplex_paused = true;
    SDL_Delay(5); /* let thread finish any in-flight write */

    s->diag_scan_count    = 0;
    s->diag_pca_addr_count = 0;

    device_t *bus = device_get("I2CBUS0");
    if (bus) {
        i2c_bus_device_t *ib = (i2c_bus_device_t *)bus;
        s->diag_scan_count = ib->_scan(ib, s->diag_scan_results, 128);
        for (int i = 0; i < s->diag_scan_count; i++) {
            uint8_t addr = s->diag_scan_results[i].address;
            if (addr >= 0x20 && addr <= 0x27 && s->diag_pca_addr_count < 8)
                s->diag_pca_addrs[s->diag_pca_addr_count++] = addr;
        }
    }

    g_multiplex_paused = false;
}

/* -------------------------------------------------------------------------
 * Diagnostic pattern name
 * ------------------------------------------------------------------------- */
static void diag_pattern_name(int p, char *buf, int sz) {
    if (p == 0)
        SDL_snprintf(buf, sz, "All ON");
    else if (p == 1)
        SDL_snprintf(buf, sz, "All OFF");
    else if (p < 2 + MATRIX_COLS)
        SDL_snprintf(buf, sz, "Column %d", p - 2);
    else if (p < 2 + MATRIX_COLS + MATRIX_ROWS)
        SDL_snprintf(buf, sz, "Row %d", p - 2 - MATRIX_COLS);
    else if (p == 2 + MATRIX_COLS + MATRIX_ROWS)
        SDL_snprintf(buf, sz, "Checkerboard A");
    else
        SDL_snprintf(buf, sz, "Checkerboard B");
}

/* -------------------------------------------------------------------------
 * Rendering
 * ------------------------------------------------------------------------- */
static void render_matrix_preview(SDL_Renderer *r) {
    uint8_t fb[MATRIX_ROWS][MATRIX_COLS];
    SDL_LockMutex(g_fb_mutex);
    SDL_memcpy(fb, g_framebuf, sizeof(fb));
    SDL_UnlockMutex(g_fb_mutex);

    for (int row = 0; row < MATRIX_ROWS; row++) {
        for (int col = 0; col < MATRIX_COLS; col++) {
            bool on = fb[row][col] != 0;
            SDL_FRect rect = {
                (float)(MATRIX_X0 + col * LED_CW),
                (float)(MATRIX_Y0 + row * LED_CH),
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

static void render_text_centered(SDL_Renderer *r, float cx, float y, const char *text) {
    float w = (float)(SDL_strlen(text) * 8);
    SDL_RenderDebugText(r, cx - w * 0.5f, y, text);
}

static void render_diag(AppState *s) {
    SDL_Renderer *r = s->renderer;
    float y = (float)CTRL_Y;

    /* Build scan address string */
    char scan_addrs[200] = {0};
    int  apos = 0;
    for (int i = 0; i < s->diag_scan_count && apos < (int)sizeof(scan_addrs) - 8; i++) {
        int n = SDL_snprintf(scan_addrs + apos, sizeof(scan_addrs) - apos,
                             "0x%02X ", s->diag_scan_results[i].address);
        apos += n;
    }

    /* Line 1: driver status + hw response */
    SDL_SetRenderDrawColor(r, 0x66, 0x66, 0x66, SDL_ALPHA_OPAQUE);
    SDL_RenderDebugTextFormat(r, 40.0f, y, "Driver: %s",
        s->led_matrix ? "registered (0x20)" : "NOT registered");

    if (!s->led_matrix) {
        SDL_SetRenderDrawColor(r, 0xFF, 0x44, 0x44, SDL_ALPHA_OPAQUE);
        SDL_RenderDebugText(r, 340.0f, y, "Hardware: N/A");
    } else if (g_hw_ok) {
        SDL_SetRenderDrawColor(r, 0x00, 0xCC, 0x00, SDL_ALPHA_OPAQUE);
        SDL_RenderDebugText(r, 340.0f, y, "Hardware: RESPONDING");
    } else {
        SDL_SetRenderDrawColor(r, 0xFF, 0x44, 0x44, SDL_ALPHA_OPAQUE);
        SDL_RenderDebugText(r, 340.0f, y, "Hardware: NOT RESPONDING");
    }
    SDL_SetRenderDrawColor(r, 0x55, 0x55, 0x55, SDL_ALPHA_OPAQUE);
    SDL_RenderDebugText(r, 590.0f, y, "R=rescan");
    y += 14.0f;

    /* Line 2: I2C bus scan results */
    SDL_SetRenderDrawColor(r, 0x77, 0x99, 0xBB, SDL_ALPHA_OPAQUE);
    SDL_RenderDebugTextFormat(r, 40.0f, y, "I2C bus (%d): %s",
        s->diag_scan_count, apos > 0 ? scan_addrs : "(none)");
    y += 14.0f;

    /* Line 3: PCA9698 address range */
    if (s->diag_pca_addr_count > 0) {
        char pca_str[48] = {0};
        int ppos = 0;
        for (int i = 0; i < s->diag_pca_addr_count; i++) {
            int n = SDL_snprintf(pca_str + ppos, sizeof(pca_str) - ppos,
                                 "0x%02X ", s->diag_pca_addrs[i]);
            ppos += n;
        }
        SDL_SetRenderDrawColor(r, 0x00, 0xFF, 0x88, SDL_ALPHA_OPAQUE);
        SDL_RenderDebugTextFormat(r, 40.0f, y, "PCA9698 (0x20-0x27): FOUND at %s", pca_str);
        if (s->diag_pca_addrs[0] != 0x20) {
            SDL_SetRenderDrawColor(r, 0xFF, 0xCC, 0x00, SDL_ALPHA_OPAQUE);
            SDL_RenderDebugText(r, 40.0f, y + 13.0f,
                "  ^ addr mismatch — update why2025_firmware.c");
            y += 13.0f;
        }
    } else {
        SDL_SetRenderDrawColor(r, 0xFF, 0x44, 0x44, SDL_ALPHA_OPAQUE);
        SDL_RenderDebugText(r, 40.0f, y,
            "PCA9698 (0x20-0x27): NOT FOUND — check wiring/power");
    }
    y += 16.0f;

    /* Separator */
    SDL_SetRenderDrawColor(r, 0x2A, 0x2A, 0x2A, SDL_ALPHA_OPAQUE);
    SDL_FRect sep = { 40.0f, y, 640.0f, 1.0f };
    SDL_RenderFillRect(r, &sep);
    y += 4.0f;

    /* Pattern list — 5 visible rows, scroll around selection */
    int visible = 5;
    int start = s->diag_pattern - 2;
    if (start < 0) start = 0;
    if (start > DIAG_NUM_PATTERNS - visible) start = DIAG_NUM_PATTERNS - visible;

    for (int i = start; i < start + visible && i < DIAG_NUM_PATTERNS; i++) {
        bool sel = (i == s->diag_pattern);
        char name[48];
        diag_pattern_name(i, name, sizeof(name));
        if (sel) {
            SDL_SetRenderDrawColor(r, 0x00, 0x60, 0xAA, SDL_ALPHA_OPAQUE);
            SDL_FRect hl = { 36.0f, y - 1.0f, 420.0f, 13.0f };
            SDL_RenderFillRect(r, &hl);
            SDL_SetRenderDrawColor(r, 0xFF, 0xFF, 0xFF, SDL_ALPHA_OPAQUE);
        } else {
            SDL_SetRenderDrawColor(r, 0x66, 0x66, 0x66, SDL_ALPHA_OPAQUE);
        }
        SDL_RenderDebugTextFormat(r, 40.0f, y, "%s %s", sel ? ">" : " ", name);
        y += 13.0f;
    }
    y += 2.0f;

    SDL_SetRenderDrawColor(r, 0x44, 0x44, 0x44, SDL_ALPHA_OPAQUE);
    render_text_centered(r, 360.0f, y,
        "UP/DOWN pattern   R rescan   D exit diag   ESC quit");
}

static void render_normal(AppState *s) {
    SDL_Renderer *r = s->renderer;
    float y = (float)CTRL_Y;

    /* Line 1: status + mode */
    SDL_SetRenderDrawColor(r, 0x66, 0x66, 0x66, SDL_ALPHA_OPAQUE);
    SDL_RenderDebugTextFormat(r, 40.0f, y, "LEDMATRIX0: %s",
        !s->led_matrix ? "no driver" :
        g_hw_ok        ? "responding" : "driver only (D=diag)");

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
        SDL_RenderDebugText(r, 84.0f, y,
            s->editing ? "_" : "(empty — press T to type)");
    }
    y += 16.0f;

    /* Line 3: context hint */
    SDL_SetRenderDrawColor(r, 0x44, 0x44, 0x44, SDL_ALPHA_OPAQUE);
    if (s->editing) {
        SDL_RenderDebugText(r, 40.0f, y,
            "Editing — BACKSPACE delete   ENTER/ESC done");
    } else if (s->mode == 0 && s->text_len > 0) {
        int last = s->static_char_idx + CHARS_VISIBLE - 1;
        if (last >= s->text_len) last = s->text_len - 1;
        SDL_RenderDebugTextFormat(r, 40.0f, y,
            "Showing chars %d-%d/%d   LEFT/RIGHT navigate",
            s->static_char_idx + 1, last + 1, s->text_len);
    } else {
        SDL_RenderDebugText(r, 40.0f, y,
            "T edit   M mode   D diagnostics   ESC quit");
    }
}

/* -------------------------------------------------------------------------
 * SDL callbacks
 * ------------------------------------------------------------------------- */

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
    (void)argc; (void)argv;

    if (!SDL_SetAppMetadata("LED Matrix v1", "1.0", "com.badgevms.led_matrix_v1"))
        return SDL_APP_FAILURE;

    AppState *s = SDL_calloc(1, sizeof(AppState));
    if (!s) return SDL_APP_FAILURE;
    *appstate = s;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    s->window = SDL_CreateWindow("LED Matrix v1",
                                 SCREEN_WIDTH, SCREEN_HEIGHT,
                                 SDL_WINDOW_FULLSCREEN);
    if (!s->window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    s->renderer = SDL_CreateRenderer(s->window, NULL);
    if (!s->renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    g_fb_mutex = SDL_CreateMutex();
    if (!g_fb_mutex) {
        SDL_Log("SDL_CreateMutex failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    s->led_matrix = device_get("LEDMATRIX0");
    if (!s->led_matrix) {
        SDL_Log("LEDMATRIX0 not found — preview-only mode");
    } else {
        g_multiplex_running = true;
        s->mux_thread = SDL_CreateThread(multiplex_thread_fn, "mux", s->led_matrix);
        if (!s->mux_thread) {
            SDL_Log("Failed to create mux thread: %s", SDL_GetError());
            g_multiplex_running = false;
        }
    }

    SDL_strlcpy(s->text, "Hello", sizeof(s->text));
    s->text_len      = (int)SDL_strlen(s->text);
    s->mode          = 0;
    s->last_scroll_tick = SDL_GetTicks();

    run_diag_scan(s);
    update_framebuf(s);
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
                    unsigned char ch = (unsigned char)*input++;
                    if (ch >= 0x20 && ch <= 0x7E) {
                        s->text[s->text_len++] = (char)ch;
                        s->text[s->text_len]   = '\0';
                    }
                }
                if (s->static_char_idx >= s->text_len && s->text_len > 0)
                    s->static_char_idx = s->text_len - 1;
                update_framebuf(s);
            }
            break;

        case SDL_EVENT_KEY_DOWN: {
            SDL_Scancode sc = event->key.scancode;

            if (s->editing) {
                if (sc == SDL_SCANCODE_BACKSPACE && s->text_len > 0) {
                    s->text[--s->text_len] = '\0';
                    if (s->text_len == 0)
                        s->static_char_idx = 0;
                    else if (s->static_char_idx >= s->text_len)
                        s->static_char_idx = s->text_len - 1;
                    update_framebuf(s);
                } else if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_RETURN) {
                    s->editing = false;
                    SDL_StopTextInput(s->window);
                }
                break;
            }

            if (s->mode == 2) {
                switch (sc) {
                    case SDL_SCANCODE_ESCAPE: return SDL_APP_SUCCESS;
                    case SDL_SCANCODE_D:
                        s->mode = 0;
                        update_framebuf(s);
                        break;
                    case SDL_SCANCODE_R:
                        run_diag_scan(s);
                        break;
                    case SDL_SCANCODE_UP:
                        s->diag_pattern = (s->diag_pattern - 1 + DIAG_NUM_PATTERNS) % DIAG_NUM_PATTERNS;
                        update_framebuf(s);
                        break;
                    case SDL_SCANCODE_DOWN:
                        s->diag_pattern = (s->diag_pattern + 1) % DIAG_NUM_PATTERNS;
                        update_framebuf(s);
                        break;
                    default: break;
                }
                break;
            }

            switch (sc) {
                case SDL_SCANCODE_ESCAPE: return SDL_APP_SUCCESS;

                case SDL_SCANCODE_M:
                    s->mode = s->mode == 0 ? 1 : 0;
                    s->scroll_x = 0;
                    s->last_scroll_tick = SDL_GetTicks();
                    update_framebuf(s);
                    break;

                case SDL_SCANCODE_D:
                    s->mode = 2;
                    run_diag_scan(s);
                    update_framebuf(s);
                    break;

                case SDL_SCANCODE_T:
                    s->editing = true;
                    SDL_StartTextInput(s->window);
                    break;

                case SDL_SCANCODE_LEFT:
                    if (s->mode == 0 && s->static_char_idx > 0) {
                        s->static_char_idx--;
                        update_framebuf(s);
                    }
                    break;

                case SDL_SCANCODE_RIGHT:
                    if (s->mode == 0 &&
                        s->static_char_idx + CHARS_VISIBLE < s->text_len) {
                        s->static_char_idx++;
                        update_framebuf(s);
                    }
                    break;

                default: break;
            }
            break;
        }

        default: break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    AppState *s = (AppState *)appstate;
    Uint64 now = SDL_GetTicks();

    /* Advance scroll */
    if (s->mode == 1 && s->text_len > 0) {
        bool advanced = false;
        while (now - s->last_scroll_tick >= SCROLL_INTERVAL_MS) {
            int total = s->text_len * 6;
            s->scroll_x = (s->scroll_x + 1) % total;
            s->last_scroll_tick += SCROLL_INTERVAL_MS;
            advanced = true;
        }
        if (advanced) update_framebuf(s);
    }

    /* Render */
    SDL_SetRenderDrawColor(s->renderer, 0x0D, 0x0D, 0x0D, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(s->renderer);

    render_matrix_preview(s->renderer);

    if (s->mode == 2)
        render_diag(s);
    else
        render_normal(s);

    SDL_RenderPresent(s->renderer);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    (void)result;
    if (!appstate) return;
    AppState *s = (AppState *)appstate;

    if (s->editing) SDL_StopTextInput(s->window);

    if (s->mux_thread) {
        g_multiplex_running = false;
        SDL_WaitThread(s->mux_thread, NULL);
        s->mux_thread = NULL;
    }

    if (g_fb_mutex) {
        SDL_DestroyMutex(g_fb_mutex);
        g_fb_mutex = NULL;
    }

    SDL_DestroyRenderer(s->renderer);
    SDL_DestroyWindow(s->window);
    SDL_free(s);
}
