// payload/main.c — GBA-side multiboot payload for the Workshop Computer link.
//
// Runs from EWRAM after the RP2040 uploads it via BIOS Multiboot. It:
//   1. Puts the LCD in mode 3 (240x160 16bpp bitmap) and IMMEDIATELY draws the title
//      "MTM - Workshop Computer Link". This is deliberately the very first thing that
//      happens, before any serial setup, so the screen alone proves the payload booted and
//      is executing. A blank screen therefore means the image never ran — a completely
//      different fault from "ran but the link is quiet", and previously the two were
//      indistinguishable.
//   2. Acts as a serial NORMAL-mode 32-bit SLAVE (external clock from the RP2040):
//        - preloads SIODATA32 with (0x600D << 16) | buttonBits before each transfer,
//        - after each host-clocked word, reads the 4 param bytes the host sent.
//   3. Redraws only the dynamic strip below the title, so the title stays rock-steady and
//      each frame is cheap.
//
// Dependency-free (no libgba) so it builds with a bare arm-none-eabi toolchain targeting
// armv4t. See build.sh.

#include <stdint.h>
#include "font5x7.h"

// ---- GBA memory-mapped I/O ----
#define REG_BASE        0x04000000
#define REG_DISPCNT     (*(volatile uint16_t*)(REG_BASE + 0x0000))
#define REG_VCOUNT      (*(volatile uint16_t*)(REG_BASE + 0x0006))
#define REG_KEYINPUT    (*(volatile uint16_t*)(REG_BASE + 0x0130))
#define REG_SIODATA32   (*(volatile uint32_t*)(REG_BASE + 0x0120))
#define REG_SIOCNT      (*(volatile uint16_t*)(REG_BASE + 0x0128))
#define REG_RCNT        (*(volatile uint16_t*)(REG_BASE + 0x0134))

#define VRAM            ((volatile uint16_t*)0x06000000)
#define SCREEN_W        240
#define SCREEN_H        160

#define MODE3           0x0003
#define BG2_ON          0x0400

// SIOCNT, normal mode. Mode select is SIOCNT bits 13:12 while RCNT[15:14] == 00:
//   00 = Normal 8-bit, 01 = Normal 32-bit, 10 = Multiplay, 11 = UART.
// So 32-bit normal mode is bit12 set, bit13 clear.
#define SIO_32BIT       (1 << 12)
#define SIO_START       (1 << 7)      // slave sets this to arm; hardware clears on completion
#define SIO_SLAVE       (0 << 0)      // external clock
#define RCNT_SERIAL     0x0000

static inline uint16_t rgb15(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)((r & 31) | ((g & 31) << 5) | ((b & 31) << 10));
}

static void rect(int x, int y, int w, int h, uint16_t color)
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

// Draw one glyph at `scale`. Column-major font: bit r of column c is the pixel at (c, r).
static void glyph(int x, int y, char ch, uint16_t color, int scale)
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

static int text_width(const char *s, int scale)
{
    int n = 0;
    while (s[n]) n++;
    return n * FONT_ADV * scale;
}

static void text(int x, int y, const char *s, uint16_t color, int scale)
{
    for (int i = 0; s[i]; i++) glyph(x + i * FONT_ADV * scale, y, s[i], color, scale);
}

static void text_centre(int y, const char *s, uint16_t color, int scale)
{
    text((SCREEN_W - text_width(s, scale)) / 2, y, s, color, scale);
}

// Render a 32-bit value as 8 hex digits. Freestanding build: no libc, so do it by hand.
static void hex32(char *out, uint32_t v)
{
    static const char H[] = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) out[i] = H[(v >> (28 - 4 * i)) & 0xF];
    out[8] = 0;
}

static uint16_t read_buttons(void)
{
    // REG_KEYINPUT is active-LOW (0 = pressed); invert and mask to the 10 valid key bits so
    // it matches the host's GbaKey layout.
    return (uint16_t)(~REG_KEYINPUT) & 0x03FF;
}

#define COL_BG     rgb15( 2,  3,  8)
#define COL_TITLE  rgb15(31, 28, 10)
#define COL_DIM    rgb15( 8,  8, 12)
#define COL_OK     rgb15( 6, 31, 10)
#define COL_WAIT   rgb15(31, 14,  4)
#define COL_BAR    rgb15(10, 22, 31)
#define COL_HEX    rgb15(31, 31, 31)   // white: the diagnostic line must be easy to read

#define DYN_Y      52    // dynamic widgets live below this

// ── Link servicing ───────────────────────────────────────────────────────────────────────
// The slave can only have ONE transfer pending: the hardware clears SIO_START when the host
// finishes clocking a word, and until we re-arm it we are deaf. The first version blocked in
// a spin-wait and then spent a whole frame redrawing while UNARMED — the host polls every
// 1 ms and gives up after a run of bad words, so it disconnected almost immediately.
//
// Fix: never block, and re-arm the instant a word lands. service() is cheap and is called
// between every drawing step, so the slave is armed essentially all of the time.
static volatile uint32_t g_params = 0;
static volatile uint32_t g_rx     = 0;
static uint16_t g_buttons = 0;

// ── Bench mode (for diagnostics/bandwidth.uf2) ───────────────────────────────────────────
// Normally we reply with the button state. In BENCH mode we instead ECHO the low 16 bits of
// the word just received. That is what lets the host measure the DOWNSTREAM direction: a tag
// check alone only proves the GBA->host path, and the two directions can have different
// ceilings. The echo necessarily lags by one transfer, because the reply for transfer N+1 is
// preloaded while transfer N is being harvested.
//
// Entered on two CONSECUTIVE 0xBE7CBE7C words and left on two consecutive 0xBE7C0000, so the
// normal applet cannot fall into it by accident: it would need all four knob/CV bytes to hit
// exact values twice in a row.
#define BENCH_ENTER 0xBE7CBE7Cu
#define BENCH_LEAVE 0xBE7C0000u

// VOLATILE deliberately. These are written inside service(), which is called from many
// places including tight loops, and read by the main loop. Without volatile the compiler is
// entitled to cache them in a register at -O2 and never observe the change — which is
// indistinguishable from "the magic word was never recognised".
static volatile int g_bench = 0;
static volatile int g_benchDirty = 1;   // screen needs repainting for the current mode
static volatile int g_enterHits = 0;    // shown on screen: proof the compare actually fires

static void service(void)
{
    if (!(REG_SIOCNT & SIO_START)) {          // a word completed (or we have never armed)
        uint32_t got = REG_SIODATA32;
        g_params = got;
        g_rx++;

        // ONE word is enough, in each direction. Requiring several (consecutive, then merely
        // counted) has failed twice on hardware while the host's word was demonstrably
        // arriving intact — visible as BE7CBE7C on screen. The extra conditions were only
        // ever guarding against accidental entry, and a full 32-bit exact match is already
        // vanishingly unlikely: the applet would need all four knob/CV bytes to land on
        // 0xBE7CBE7C simultaneously. Trade that theoretical safety for something that works.
        if (got == BENCH_ENTER) { g_enterHits++; if (!g_bench) { g_bench = 1; g_benchDirty = 1; } }
        if (got == BENCH_LEAVE) { if (g_bench) { g_bench = 0; g_benchDirty = 1; } }

        REG_SIODATA32 = g_bench ? ((0x600Du << 16) | (got & 0xFFFFu))   // echo
                                : ((0x600Du << 16) | g_buttons);        // normal reply
        REG_SIOCNT |= SIO_START;                       // re-arm; also pulls SO low = ready
    }
}

// Draw helpers that keep the link serviced while they work.
static void srect(int x, int y, int w, int h, uint16_t color) { rect(x, y, w, h, color); service(); }

int main(void)
{
    // ---- 1. LCD up and TITLE DRAWN FIRST — the boot proof ----
    REG_DISPCNT = MODE3 | BG2_ON;
    rect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
    text_centre(14, "MTM - Workshop Computer Link", COL_TITLE, 1);
    rect(20, 30, SCREEN_W - 40, 1, COL_DIM);

    // ---- 2. Serial: normal mode, 32-bit, slave (external clock from the RP2040) ----
    REG_RCNT   = RCNT_SERIAL;
    REG_SIOCNT = SIO_32BIT | SIO_SLAVE;
    REG_SIODATA32 = (0x600Du << 16);
    REG_SIOCNT |= SIO_START;                   // arm immediately

    uint32_t lastRx = 0;
    int      linkUp = 0, quiet = 0;

    for (;;) {
        g_buttons = read_buttons();
        service();

        // BENCH: service as tightly as possible and draw nothing. This measures the transport
        // ceiling itself; comparing it against the normal path shows what the UI costs.
        if (g_bench) {
            if (g_benchDirty) {                 // paint the banner ONCE, then never again
                g_benchDirty = 0;
                rect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
                service();
                text_centre(60, "BENCH MODE", COL_WAIT, 2);
                service();
                text_centre(90, "measuring link", COL_DIM, 1);
            }
            for (int i = 0; i < 2000; i++) service();
            continue;
        }
        if (g_benchDirty) {                     // returning to the normal UI: repaint the title
            g_benchDirty = 0;
            rect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
            text_centre(14, "MTM - Workshop Computer Link", COL_TITLE, 1);
            rect(20, 30, SCREEN_W - 40, 1, COL_DIM);
        }

        uint32_t rx = g_rx;
        if (rx != lastRx) { lastRx = rx; linkUp = 1; quiet = 0; }
        else if (++quiet > 90) { linkUp = 0; }

        uint32_t params = g_params;

        // ---- 3. Redraw only the small dynamic widgets, servicing between each ----
        // A full-strip clear each frame cost ~26k pixels; these targeted erases cost a
        // fraction of that, which keeps the deaf window short.
        srect(0, DYN_Y, SCREEN_W, 9, COL_BG);
        text_centre(DYN_Y, linkUp ? "LINK OK" : "WAITING FOR HOST",
                    linkUp ? COL_OK : COL_WAIT, 1);
        service();

        // THE LAST WORD RECEIVED, in hex. This is the payload's equivalent of the host's
        // nibble readout: without it, "the GBA is not recognising the magic word" and "the
        // GBA is not receiving anything" look identical from the bench. With it you can read
        // straight off the screen whether BE7CBE7C is arriving intact.
        {
            // HOLD the value for ~0.5 s. Updating every frame made it change far too fast to
            // read, so what it showed could only be guessed at — and a guess is exactly what
            // must not be fed back into a diagnosis.
            static uint32_t held = 0; static int holdN = 0;
            if (++holdN >= 30) { holdN = 0; held = params; }
            char buf[9];
            hex32(buf, held);
            srect(0, 36, SCREEN_W, 9, COL_BG);
            text(60, 36, buf, COL_HEX, 1);
            // How many times the BENCH_ENTER compare has actually matched. If the hex line
            // reads BE7CBE7C and this stays 00, the comparison itself is not firing and the
            // fault is nowhere near the link.
            char hits[9];
            hex32(hits, (uint32_t)g_enterHits);
            text(150, 36, hits + 6, COL_HEX, 1);
        }

        // Activity pip: steps across on every received word, so liveness is visible even if
        // the params themselves are wrong.
        srect(0, DYN_Y + 12, SCREEN_W, 4, COL_BG);
        srect(8 + (int)((rx >> 2) % 28) * 8, DYN_Y + 12, 6, 4, linkUp ? COL_OK : COL_DIM);

        // Button indicators.
        for (int b = 0; b < 10; b++) {
            uint16_t c = (g_buttons & (1u << b)) ? COL_TITLE : COL_DIM;
            srect(6 + b * 23, DYN_Y + 20, 19, 8, c);
        }

        // Four bars for the four host params.
        const int barTop = DYN_Y + 34;
        const int maxh   = SCREEN_H - barTop - 6;
        const uint8_t vals[4] = {
            (uint8_t)(params >> 24), (uint8_t)(params >> 16),
            (uint8_t)(params >>  8), (uint8_t)(params)
        };
        for (int k = 0; k < 4; k++) {
            int h = 1 + (vals[k] * maxh) / 255;
            srect(22 + k * 52, barTop, 34, maxh, COL_BG);          // erase the column
            srect(22 + k * 52, SCREEN_H - 6 - h, 34, h, COL_BAR);  // draw the bar
        }

        // Pace to roughly one frame without blocking the link: poll VCOUNT while servicing.
        while (REG_VCOUNT <  SCREEN_H) service();
        while (REG_VCOUNT >= SCREEN_H) service();
    }
}
