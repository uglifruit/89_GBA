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

// Forward declaration: text() must keep the link serviced between glyphs. A single line of
// scale-2 text is hundreds of rect() calls, and with the host streaming words back-to-back
// that was long enough to lose thousands of them — which the host then read as a dead link.
static void service(void);

static void text(int x, int y, const char *s, uint16_t color, int scale)
{
    for (int i = 0; s[i]; i++) { glyph(x + i * FONT_ADV * scale, y, s[i], color, scale); service(); }
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

// Right-aligned decimal into a fixed field. Counts and kHz are much easier to read as
// decimal than hex, and this screen exists to be read at a glance from the bench.
static void dec32(char *out, uint32_t v, int width)
{
    for (int i = 0; i < width; i++) out[i] = ' ';
    out[width] = 0;
    int i = width - 1;
    if (v == 0) { out[i] = '0'; return; }
    while (v && i >= 0) { out[i--] = (char)('0' + (v % 10)); v /= 10; }
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
#define COL_FAIL   rgb15(31,  6,  6)

#define BAR_X 12
#define BAR_Y 56
#define BAR_W 216
#define BAR_H 22

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
// AUTO-ANALYSIS of the incoming stream. Reading a flickering hex line and guessing what it
// said has now misled this investigation twice, so the payload classifies every word itself
// and shows running totals. Totals cannot flicker: they only ever count up.
// ── LINK SPEED TEST (diagnostics/linkrate.uf2) ───────────────────────────────────────────
// The host streams sequence-numbered words at a rate you pick by hand; the GBA checks the
// sequence for gaps and reports the result ON ITS OWN SCREEN. Putting the readout here rather
// than on six LEDs is the whole point: this console has a display, and every number that has
// had to be decoded from blinking LEDs in this project has cost a bench cycle.
//   0xA5______  low 24 bits = sequence counter, incrementing by 1
//   0xA6____xx  low 16 bits = the current SCK rate in kHz, for display
// 16-BIT magics, not 8. The originals were 0xA5 and 0xA6 — two bits apart — and the GBA can
// latch a BIT-SHIFTED word when it re-arms in the middle of a transfer. So an ordinary ramp
// value could arrive looking like a rate message, and the displayed SCK jumped around between
// 100 and 40000. Sixteen matching bits makes that effectively impossible, and anything
// matching neither magic is now counted as corruption instead of being acted on.
#define LT_SEQ_MAGIC  0xA5A5u
#define LT_RATE_MAGIC 0xA6A6u
// The host ramps a 16-bit counter 0 -> 0xFFFF over and over. The GBA plots how far each pass
// gets: a clean pass fills the bar, a broken one stops where it broke. Whether a full sweep
// completes, repeatably, at a given rate is the actual question — and this answers it at a
// glance instead of by comparing two numbers.
static volatile int      g_linkTest = 0;
static volatile uint32_t g_ltRate   = 0;   // WORD rate in Hz (SCK is fixed at 100 kHz)
static volatile uint32_t g_ltVal    = 0;   // current value in this pass
static volatile uint32_t g_ltFailAt = 0;   // where this pass broke
static volatile int      g_ltFailed = 0;   // this pass has broken
static volatile uint32_t g_ltPassOk = 0;   // passes that reached 0xFFFF cleanly
static volatile uint32_t g_ltPassBad= 0;   // passes that broke
static volatile uint32_t g_ltBadMagic = 0;   // words matching neither magic
// MEASURING WINDOW. The payload is deaf whenever it draws, so a ramp that runs while the
// screen is being updated can never complete — it breaks once per frame, for ever, no matter
// how good the link is. That is our own redraw behaviour, not a property of the rate, and
// mixing the two makes the measurement meaningless.
//
// So the pass is measured with the screen FROZEN: draw the previous result, then service
// tightly for a whole 0->FFFF ramp without drawing at all, then draw the outcome.
static volatile int      g_ltMeasuring = 0;
static volatile int      g_ltPassDone  = 0;
// A pass must begin at a KNOWN point in the ramp, not wherever the counter happened to be
// when the measuring window opened. Andy measured a rock-steady "2 dropped, 4 corrupt" on
// every pass from 100 to 2000 words/s — identical across a 20x rate range, which cannot be a
// rate-dependent link error. It is the boundary: the payload comes out of a long deaf redraw
// part-way through a word, latches a partial one or two, and re-syncs. Those artefacts belong
// to the instrument, not the link, so the window now DISCARDS everything until the ramp wraps
// and only then starts judging. Every pass is then a true 000->FFF sweep.
static volatile int      g_ltArmed     = 0;
static volatile uint32_t g_ltSkips     = 0;   // small gaps: dropped words
static volatile uint32_t g_ltWild      = 0;   // large jumps: bit-slip / corruption
static uint16_t g_ltPrev = 0;
static int      g_ltHave = 0;

static volatile int      g_enterHits = 0;   // words exactly == BENCH_ENTER
static volatile int      g_nearHits  = 0;   // words within 4 bits of BENCH_ENTER (corruption)
static volatile int      g_be7cHits  = 0;   // words whose TOP half is 0xBE7C (partial match)
static volatile uint32_t g_lastBe7c  = 0;   // the most recent such word, whatever it was

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

        // Classify near-misses. If the burst is arriving but mangled, an exact compare can
        // never show it — the word would just look like noise. Counting how many bits differ
        // separates "arriving corrupted" from "never arriving", which need opposite fixes.
        {
            uint32_t diff = got ^ BENCH_ENTER;
            int bits = 0;
            for (int b = 0; b < 32; b++) if (diff & (1u << b)) bits++;
            if (bits > 0 && bits <= 4) g_nearHits++;
            if ((got >> 16) == 0xBE7Cu) { g_be7cHits++; g_lastBe7c = got; }
        }

        // Link speed test: a gap in the sequence is a dropped or corrupted word, which is
        // exactly the reliability figure the rate sweep needs.
        {
            uint32_t mag = got >> 16;
            if (mag == LT_SEQ_MAGIC) {
                g_linkTest = 1;
                uint16_t v = (uint16_t)(got & 0x0FFFu);   // 12-bit ramp: 4096 words
                if (!g_ltMeasuring) {
                    g_ltPrev = v; g_ltHave = 0;     // drawing: track position, judge nothing
                } else if (!g_ltArmed) {
                    // Warm-up: follow the ramp without judging until it wraps, so the pass
                    // starts at a known point and boundary artefacts are excluded entirely.
                    if (g_ltHave && v < g_ltPrev) {
                        g_ltArmed = 1;
                        g_ltSkips = 0; g_ltWild = 0; g_ltBadMagic = 0;
                        g_ltFailed = 0; g_ltFailAt = 0;
                    }
                    g_ltPrev = v; g_ltHave = 1; g_ltVal = v;
                } else {
                    uint16_t expect = (uint16_t)((g_ltPrev + 1) & 0x0FFFu);
                    if (v < g_ltPrev) {
                        // Value went backwards = the ramp wrapped. Detecting the wrap this way
                        // rather than by waiting for exactly 0 matters: if that one word is
                        // dropped, waiting for it would hang the pass for ever.
                        g_ltPassDone = 1;
                    } else if (v != expect) {
                        // Classify the discontinuity, which is the question worth asking: a
                        // small gap is dropped words, a wild jump is a bit-slip.
                        uint16_t gap = (uint16_t)(v - expect);
                        if (gap <= 8) g_ltSkips++; else g_ltWild++;
                        if (!g_ltFailed) { g_ltFailed = 1; g_ltFailAt = g_ltPrev; }
                    }
                    g_ltPrev = v; g_ltVal = v;
                }
            } else if (mag == LT_RATE_MAGIC) {
                g_linkTest = 1;
                // A rate change restarts everything, so what is on screen always describes the
                // rate shown beside it and never a blend of two.
                if ((got & 0xFFFFu) != g_ltRate) {
                    g_ltRate = got & 0xFFFFu;
                    g_ltPassOk = g_ltPassBad = 0; g_ltBadMagic = 0;
                    g_ltFailed = 0; g_ltFailAt = 0; g_ltHave = 0; g_ltVal = 0;
                    g_benchDirty = 1;
                }
            } else if (g_linkTest && g_ltMeasuring && g_ltArmed) {
                // Neither magic: corrupt, most likely bit-shifted by the slave re-arming mid
                // transfer. Only counted while MEASURING — outside the window we are drawing,
                // and words missed then are our own doing rather than the link's.
                g_ltBadMagic++;
                if (!g_ltFailed) { g_ltFailed = 1; g_ltFailAt = g_ltPrev; }
            }
        }

        REG_SIODATA32 = g_bench      ? ((0x600Du << 16) | (got & 0xFFFFu))          // echo
                      : g_linkTest   ? ((0x600Du << 16) | (uint16_t)g_ltVal)          // where we are
                                     : ((0x600Du << 16) | g_buttons);               // normal
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

        // ── LINK SPEED TEST: one full 0 -> FFFF ramp per pass, screen frozen ─────────────
        // Draw the LAST pass, then freeze the display and service tightly for a whole ramp.
        // Drawing and measuring cannot overlap: the payload is deaf while it draws, so a ramp
        // running across a redraw breaks once per frame regardless of how good the link is.
        // Separating them is what makes "did a full 0-FFFF sweep complete cleanly" answerable.
        if (g_linkTest && !g_bench) {
            char buf[12];
            g_ltMeasuring = 0;                       // drawing: judge nothing that arrives now

            rect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
            text_centre(6, "LINK 0-0FFF RAMP", COL_TITLE, 1);

            dec32(buf, g_ltRate, 5);
            text(24, 22, "RATE", COL_DIM, 1);
            text(64, 20, buf, COL_HEX, 2);
            text(174, 28, "wds/s", COL_DIM, 1);

            // Result of the pass just finished.
            rect(BAR_X - 2, BAR_Y - 2, BAR_W + 4, BAR_H + 4, COL_DIM);
            rect(BAR_X, BAR_Y, BAR_W, BAR_H, COL_BG);
            if (g_ltHave) {
                int w = g_ltFailed ? (int)(((uint64_t)g_ltFailAt * BAR_W) / 0x0FFFu) : BAR_W;
                if (w > 0) rect(BAR_X, BAR_Y, w, BAR_H, g_ltFailed ? COL_WAIT : COL_OK);
                if (g_ltFailed) rect(BAR_X + w, BAR_Y - 4, 2, BAR_H + 8, COL_FAIL);
            }

            if (g_ltHave && g_ltFailed) {
                hex32(buf, g_ltFailAt);
                text(BAR_X, BAR_Y + BAR_H + 6, "BROKE AT", COL_DIM, 1);
                text(BAR_X + 70, BAR_Y + BAR_H + 6, buf + 4, COL_FAIL, 1);
            } else if (g_ltHave) {
                text(BAR_X, BAR_Y + BAR_H + 6, "FULL RAMP 000-FFF OK", COL_OK, 1);
            }

            // What kind of discontinuity, which is the question that distinguishes causes.
            dec32(buf, g_ltSkips, 4);
            text(14, 104, "DROPPED", COL_DIM, 1); text(76, 104, buf, g_ltSkips ? COL_WAIT : COL_OK, 1);
            dec32(buf, g_ltWild, 4);
            text(120, 104, "SLIP", COL_DIM, 1);   text(158, 104, buf, g_ltWild ? COL_FAIL : COL_OK, 1);
            dec32(buf, g_ltBadMagic, 5);
            text(14, 116, "CORRUPT", COL_DIM, 1); text(76, 116, buf, g_ltBadMagic ? COL_FAIL : COL_OK, 1);

            dec32(buf, g_ltPassOk, 3);
            text(14, 130, "CLEAN PASSES", COL_DIM, 1); text(110, 130, buf, COL_OK, 1);
            dec32(buf, g_ltPassBad, 3);
            text(140, 130, "BAD", COL_DIM, 1);         text(172, 130, buf, COL_FAIL, 1);

            if (g_ltPassOk && !g_ltPassBad)  text_centre(144, "RELIABLE AT THIS RATE", COL_OK, 1);
            else if (g_ltPassBad)            text_centre(144, "NOT RELIABLE", COL_FAIL, 1);
            else                             text_centre(144, "MEASURING...", COL_DIM, 1);

            // ---- now freeze and measure one full ramp ----
            g_ltFailed = 0; g_ltFailAt = 0; g_ltHave = 0;
            g_ltSkips = 0; g_ltWild = 0; g_ltBadMagic = 0;
            g_ltPassDone = 0;
            g_ltArmed = 0;              // discard everything until the ramp wraps
            g_ltMeasuring = 1;

            // Nothing is drawn until the ramp wraps. At 100 kHz a full pass is ~21 s; at
            // 200 kHz ~10 s. The screen holding still IS the measurement running.
            uint32_t guard = 0;
            while (!g_ltPassDone && ++guard < 40000000u) service();

            g_ltMeasuring = 0;
            if (g_ltArmed) { if (g_ltFailed) g_ltPassBad++; else g_ltPassOk++; }
            continue;
        }

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
        // (The raw-word / EXACT / NEAR / TOP analysis panel that lived here has been removed:
        // the 0-FFFF ramp screen answers the same question far better, and this is the normal
        // operating UI rather than a diagnostic.)

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
