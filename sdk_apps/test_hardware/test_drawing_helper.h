#pragma once

#include "badgevms/framebuffer.h"
#include "thirdparty/microui.h"

static uint8_t const font_8x8[96][8];
void                 draw_text(framebuffer_t *fb, char const *text, int x, int y, mu_Color color);
void                 draw_char(framebuffer_t *fb, char c, int x, int y, uint16_t color);
void                 draw_rect(framebuffer_t *fb, mu_Rect rect, mu_Color color);
void                 draw_special_symbol(framebuffer_t *fb, int symbol_index, int x, int y, uint16_t color);
int                  get_special_symbol_index(char const *symbol);
int                  mu_text_height(mu_Font font);
int                  mu_text_width(mu_Font font, char const *text, int len);
