// gfx.h — mode-3 bitmap drawing for the GBA payload.
//
// Split out of main.c unchanged. Every routine keeps the link serviced while it works: a
// single line of scale-2 text is hundreds of rect() calls, and with the host streaming words
// that was long enough to lose thousands of them, which the host then read as a dead link.
#pragma once

#include <stdint.h>

#define SCREEN_W 240
#define SCREEN_H 160

#define VRAM ((volatile uint16_t *)0x06000000)

static inline uint16_t rgb15(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)((r & 31) | ((g & 31) << 5) | ((b & 31) << 10));
}

// ---- palette ----
#define COL_BG     rgb15( 2,  3,  8)
#define COL_TITLE  rgb15(31, 28, 10)
#define COL_DIM    rgb15( 8,  8, 12)
#define COL_MID    rgb15(16, 16, 20)
#define COL_OK     rgb15( 6, 31, 10)
#define COL_WAIT   rgb15(31, 14,  4)
#define COL_BAR    rgb15(10, 22, 31)
#define COL_HEX    rgb15(31, 31, 31)
#define COL_FAIL   rgb15(31,  6,  6)
#define COL_SEL    rgb15(31, 31, 31)
#define COL_PANEL  rgb15( 5,  6, 14)

void gfx_init(void);

void rect(int x, int y, int w, int h, uint16_t color);
void srect(int x, int y, int w, int h, uint16_t color);   // rect() + link_service()
void glyph(int x, int y, char ch, uint16_t color, int scale);
void text(int x, int y, const char *s, uint16_t color, int scale);
void text_centre(int y, const char *s, uint16_t color, int scale);
int  text_width(const char *s, int scale);

void hex32(char *out, uint32_t v);
void dec32(char *out, uint32_t v, int width);
void sdec32(char *out, int32_t v, int width);   // signed, with a leading + or -
