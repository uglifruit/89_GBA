// gfx.c — mode-3 bitmap drawing. Bodies moved unchanged from the original main.c.

#include "gfx.h"
#include "link.h"
#include "font5x7.h"

#define REG_BASE    0x04000000
#define REG_DISPCNT (*(volatile uint16_t *)(REG_BASE + 0x0000))
#define MODE3       0x0003
#define BG2_ON      0x0400

void gfx_init(void)
{
    REG_DISPCNT = MODE3 | BG2_ON;
}

void rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        volatile uint16_t *row = VRAM + (y + j) * SCREEN_W + x;
        for (int i = 0; i < w; i++) row[i] = color;
    }
}

void srect(int x, int y, int w, int h, uint16_t color)
{
    rect(x, y, w, h, color);
    link_service();
}

// Draw one glyph at `scale`. Column-major font: bit r of column c is the pixel at (c, r).
void glyph(int x, int y, char ch, uint16_t color, int scale)
{
    if ((unsigned char)ch < FONT_FIRST || (unsigned char)ch > FONT_LAST) ch = '?';
    const uint8_t *g = font5x7[(unsigned char)ch - FONT_FIRST];
    for (int c = 0; c < FONT_W; c++) {
        uint8_t col = g[c];
        for (int r = 0; r < FONT_H; r++) {
            if (col & (1u << r)) rect(x + c * scale, y + r * scale, scale, scale, color);
        }
    }
}

int text_width(const char *s, int scale)
{
    int n = 0;
    while (s[n]) n++;
    return n * FONT_ADV * scale;
}

void text(int x, int y, const char *s, uint16_t color, int scale)
{
    for (int i = 0; s[i]; i++) {
        glyph(x + i * FONT_ADV * scale, y, s[i], color, scale);
        link_service();
    }
}

void text_centre(int y, const char *s, uint16_t color, int scale)
{
    text((SCREEN_W - text_width(s, scale)) / 2, y, s, color, scale);
}

// Freestanding build: no libc, so render numbers by hand.
void hex32(char *out, uint32_t v)
{
    static const char H[] = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) out[i] = H[(v >> (28 - 4 * i)) & 0xF];
    out[8] = 0;
}

// Right-aligned decimal into a fixed field. Counts are far easier to read as decimal than
// hex, and these screens exist to be read at a glance from the bench.
//
// The divisions here are by the CONSTANT 10, which GCC turns into a multiply-and-shift. That
// matters: we link -nostdlib, so libgcc is absent and a divide by a VARIABLE would be an
// undefined reference to __aeabi_uidiv rather than merely slow code.
void dec32(char *out, uint32_t v, int width)
{
    for (int i = 0; i < width; i++) out[i] = ' ';
    out[width] = 0;
    int i = width - 1;
    if (v == 0) { out[i] = '0'; return; }
    while (v && i >= 0) { out[i--] = (char)('0' + (v % 10)); v /= 10; }
}

void sdec32(char *out, int32_t v, int width)
{
    int neg = v < 0;
    uint32_t m = (uint32_t)(neg ? -v : v);
    dec32(out, m, width);
    // Place the sign immediately left of the first digit.
    for (int i = width - 1; i >= 0; i--) {
        if (out[i] == ' ') { out[i] = neg ? '-' : '+'; break; }
        if (i == 0) out[0] = neg ? '-' : '+';
    }
}
