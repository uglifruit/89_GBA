// ui.c — the screen.
//
// Two views. PLAY is the performance readout; EDIT is a five-page parameter editor. Both
// redraw only what has changed, because every pixel drawn used to be a pixel of deafness on
// the link. With the serial IRQ that is no longer strictly true, but the discipline is cheap
// and it is what keeps the fallback path viable.

#include "ui.h"
#include "gfx.h"
#include "link.h"
#include "synth.h"
#include "psg.h"

#define KEY_A      (1u << 0)
#define KEY_B      (1u << 1)
#define KEY_SELECT (1u << 2)
#define KEY_START  (1u << 3)
#define KEY_RIGHT  (1u << 4)
#define KEY_LEFT   (1u << 5)
#define KEY_UP     (1u << 6)
#define KEY_DOWN   (1u << 7)
#define KEY_R      (1u << 8)
#define KEY_L      (1u << 9)

uint8_t g_page = PAGE_VOICE;
static uint8_t g_row      = 0;
static uint8_t g_repaint  = 1;
static uint8_t g_lastMode = 0xFF;

static const char *page_name[PAGE_COUNT] = { "VOICE", "ENVELOPE", "SWEEP", "MAP", "CAL" };
static const uint8_t page_rows[PAGE_COUNT] = { 10, 6, 5, 8, 7 };

static const char *NOTE_NAME[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};
static const char *DUTY_NAME[4] = { "12.5%", "25%", "50%", "75%" };
static const char *RATIO_NAME[4] = { "25%", "50%", "100%", "100%" };
static const char *ONOFF[2] = { "OFF", "ON" };

// ---- tiny string helpers (freestanding: no libc) ----
static int scopy(char *dst, int at, const char *src)
{
    while (*src) dst[at++] = *src++;
    dst[at] = 0;
    return at;
}

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// ---- editor fields -------------------------------------------------------------------------
// One switch for the labels, one for the values, one for the adjustment. Verbose but flat:
// every field is visible in one place, which beats a table of function pointers for something
// this size.
static const char *field_label(int page, int row)
{
    switch (page) {
    case PAGE_VOICE:
        switch (row) {
        case 0: return "CH1 SQUARE";
        case 1: return "CH2 SQUARE";
        case 2: return "CH3 WAVE";
        case 3: return "CH4 NOISE";
        case 4: return "DUTY 1";
        case 5: return "DUTY 2";
        case 6: return "DETUNE";
        case 7: return "WAVEFORM";
        case 8: return "NOISE PITCH";
        case 9: return "NOISE TYPE";
        }
        break;
    case PAGE_ENV:
        switch (row) {
        case 0: return "ATTACK";
        case 1: return "DECAY";
        case 2: return "SUSTAIN";
        case 3: return "RELEASE";
        case 4: return "RETRIGGER";
        case 5: return "NOISE LEVEL";
        }
        break;
    case PAGE_SWEEP:
        switch (row) {
        case 0: return "SWEEP TIME";
        case 1: return "SWEEP DIR";
        case 2: return "SWEEP DEPTH";
        case 3: return "GLIDE";
        case 4: return "OCTAVE";
        }
        break;
    case PAGE_MAP:
        switch (row) {
        case 0: return "CV 1   ->";
        case 1: return "CV 1  DEPTH";
        case 2: return "CV 2   ->";
        case 3: return "CV 2  DEPTH";
        case 4: return "AUD 1  ->";
        case 5: return "AUD 1 DEPTH";
        case 6: return "AUD 2  ->";
        case 7: return "AUD 2 DEPTH";
        }
        break;
    case PAGE_CAL:
        switch (row) {
        case 0: return "CV SCALE";
        case 1: return "CV OFFSET";
        case 2: return "BASE NOTE";
        case 3: return "MASTER L";
        case 4: return "MASTER R";
        case 5: return "PSG LEVEL";
        case 6: return "LINK";
        }
        break;
    }
    return "?";
}

// MIDI 60 renders as C4, the convention every synth on the bench beside this one uses. Both
// divisors are compile-time constants, so GCC turns them into multiply-and-shift rather than
// the __aeabi_idivmod that -nostdlib cannot resolve.
static void note_text(char *buf, int note)
{
    int n   = (int)clampi(note, 0, 127);
    int oct = (n / 12) - 1;
    int at  = scopy(buf, 0, NOTE_NAME[n % 12]);
    if (oct < 0) { buf[at++] = '-'; oct = -oct; }
    char d[6];
    dec32(d, (uint32_t)oct, 1);
    scopy(buf, at, d);
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void field_value(int page, int row, char *buf)
{
    Patch *p = &g_patch;
    buf[0] = 0;

    switch (page) {
    case PAGE_VOICE:
        switch (row) {
        case 0: case 1: case 2: case 3:
            scopy(buf, 0, ONOFF[(p->chEnable >> row) & 1]); return;
        case 4: scopy(buf, 0, DUTY_NAME[p->duty[0] & 3]); return;
        case 5: scopy(buf, 0, DUTY_NAME[p->duty[1] & 3]); return;
        case 6: sdec32(buf, p->detune, 4); return;
        case 7: scopy(buf, 0, psg_wave_name[p->waveSel % PSG_WAVE_PRESETS]); return;
        case 8: dec32(buf, p->noiseShift, 3); return;
        case 9: scopy(buf, 0, p->noiseWidth ? "7 BIT" : "15 BIT"); return;
        }
        break;
    case PAGE_ENV:
        switch (row) {
        case 0: dec32(buf, p->atk, 3); return;
        case 1: dec32(buf, p->dec, 3); return;
        case 2: dec32(buf, p->sus, 3); return;
        case 3: dec32(buf, p->rel, 3); return;
        case 4: scopy(buf, 0, ONOFF[p->retrig ? 1 : 0]); return;
        case 5: dec32(buf, p->noiseLevel, 3); return;
        }
        break;
    case PAGE_SWEEP:
        switch (row) {
        case 0: dec32(buf, p->sweepTime, 3); return;
        case 1: scopy(buf, 0, p->sweepDir ? "DOWN" : "UP"); return;
        case 2: dec32(buf, p->sweepShift, 3); return;
        case 3: dec32(buf, p->glide, 3); return;
        case 4: sdec32(buf, p->octave, 3); return;
        }
        break;
    case PAGE_MAP: {
        int src = row >> 1;
        if ((row & 1) == 0) scopy(buf, 0, dest_name[p->mod[src].dest % DEST_COUNT]);
        else                sdec32(buf, p->mod[src].depth, 4);
        return;
    }
    case PAGE_CAL:
        switch (row) {
        case 0: dec32(buf, (uint32_t)p->cvScale, 5); return;
        case 1: sdec32(buf, p->cvOffset, 5); return;
        case 2: note_text(buf, p->baseNote); return;
        case 3: dec32(buf, p->masterL, 3); return;
        case 4: dec32(buf, p->masterR, 3); return;
        case 5: scopy(buf, 0, RATIO_NAME[p->ratio & 3]); return;
        case 6: scopy(buf, 0, link_irq_alive() ? "IRQ" : "POLLED"); return;
        }
        break;
    }
}

static void field_adjust(int page, int row, int delta)
{
    Patch *p = &g_patch;

    switch (page) {
    case PAGE_VOICE:
        switch (row) {
        case 0: case 1: case 2: case 3:
            p->chEnable ^= (uint8_t)(1u << row); return;
        case 4: p->duty[0] = (uint8_t)clampi(p->duty[0] + delta, 0, 3); return;
        case 5: p->duty[1] = (uint8_t)clampi(p->duty[1] + delta, 0, 3); return;
        case 6: p->detune  = (int8_t)clampi(p->detune + delta, -64, 63); return;
        case 7: p->waveSel = (uint8_t)clampi(p->waveSel + delta, 0, PSG_WAVE_PRESETS - 1);
                psg_wave_load(psg_wave_preset[p->waveSel]); return;
        case 8: p->noiseShift = (uint8_t)clampi(p->noiseShift + delta, 0, 13); return;
        case 9: p->noiseWidth = (uint8_t)!p->noiseWidth; return;
        }
        break;
    case PAGE_ENV:
        switch (row) {
        case 0: p->atk = (uint8_t)clampi(p->atk + delta, 0, 15); return;
        case 1: p->dec = (uint8_t)clampi(p->dec + delta, 0, 15); return;
        case 2: p->sus = (uint8_t)clampi(p->sus + delta, 0, 15); return;
        case 3: p->rel = (uint8_t)clampi(p->rel + delta, 0, 15); return;
        case 4: p->retrig = (uint8_t)!p->retrig; return;
        case 5: p->noiseLevel = (uint8_t)clampi(p->noiseLevel + delta, 0, 15); return;
        }
        break;
    case PAGE_SWEEP:
        switch (row) {
        case 0: p->sweepTime  = (uint8_t)clampi(p->sweepTime + delta, 0, 7); return;
        case 1: p->sweepDir   = (uint8_t)!p->sweepDir; return;
        case 2: p->sweepShift = (uint8_t)clampi(p->sweepShift + delta, 0, 7); return;
        case 3: p->glide      = (uint8_t)clampi(p->glide + delta, 0, 15); return;
        case 4: p->octave     = (int8_t)clampi(p->octave + delta, -3, 3); return;
        }
        break;
    case PAGE_MAP: {
        int src = row >> 1;
        if ((row & 1) == 0)
            p->mod[src].dest = (uint8_t)clampi(p->mod[src].dest + delta, 0, DEST_COUNT - 1);
        else
            p->mod[src].depth = (int8_t)clampi(p->mod[src].depth + delta, -64, 63);
        return;
    }
    case PAGE_CAL:
        switch (row) {
        case 0: p->cvScale  = (int16_t)clampi(p->cvScale + delta, 64, 4000); return;
        case 1: p->cvOffset = (int16_t)clampi(p->cvOffset + delta, -2048, 2047); return;
        case 2: p->baseNote = (uint8_t)clampi(p->baseNote + delta, 0, 120); return;
        case 3: p->masterL  = (uint8_t)clampi(p->masterL + delta, 0, 7); return;
        case 4: p->masterR  = (uint8_t)clampi(p->masterR + delta, 0, 7); return;
        case 5: p->ratio    = (uint8_t)clampi(p->ratio + delta, 0, 2); return;
        default: return;
        }
    }
}

// ---- PLAY view -----------------------------------------------------------------------------
#define PLAY_NOTE_Y 26
#define PLAY_CH_Y   72
#define PLAY_IN_Y   116

static void play_static(void)
{
    rect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
    text_centre(4, "MTM WORKSHOP COMPUTER  //  GBA PSG VOICE", COL_TITLE, 1);
    rect(14, 16, SCREEN_W - 28, 1, COL_DIM);

    static const char *chn[4] = { "SQ1", "SQ2", "WAV", "NSE" };
    for (int i = 0; i < 4; i++) text(14, PLAY_CH_Y + i * 10, chn[i], COL_DIM, 1);

    static const char *inn[5] = { "CV1", "CV2", "AU1", "AU2", "GTE" };
    for (int i = 0; i < 5; i++) text(14 + i * 44, PLAY_IN_Y, inn[i], COL_DIM, 1);

    text(14, 148, "START EDIT", COL_DIM, 1);
    text(150, 148, "A TRIG  B HOLD", COL_DIM, 1);
}

static void play_frame(void)
{
    char buf[16];

    // Note name, large. Only redrawn when it changes — a scale-3 glyph is expensive and this
    // is the one thing on screen that must never flicker.
    static uint8_t lastNote = 0xFF;
    static uint8_t lastOn   = 0xFF;
    if (g_note != lastNote || g_noteOn != lastOn) {
        lastNote = g_note; lastOn = g_noteOn;
        note_text(buf, g_note);
        srect(12, PLAY_NOTE_Y, 120, 24, COL_BG);
        text(14, PLAY_NOTE_Y, buf, g_noteOn ? COL_TITLE : COL_MID, 3);
    }

    // Envelope bar.
    {
        int w = (int)((uint32_t)g_env * 84u / 65535u);
        srect(140, PLAY_NOTE_Y + 4, 86, 14, COL_PANEL);
        if (w > 0) srect(141, PLAY_NOTE_Y + 5, w, 12, g_noteOn ? COL_OK : COL_MID);
    }

    // Gate / latch flags.
    srect(140, PLAY_NOTE_Y + 22, 90, 8, COL_BG);
    if (g_gate)  text(140, PLAY_NOTE_Y + 22, "GATE", COL_OK, 1);
    if (g_latch) text(180, PLAY_NOTE_Y + 22, "HOLD", COL_WAIT, 1);

    // Per-channel level bars.
    for (int i = 0; i < 4; i++) {
        int y = PLAY_CH_Y + i * 10;
        int w = (g_chLevel[i] * 160) / 15;
        srect(44, y, 162, 7, COL_PANEL);
        if (w > 0) srect(45, y + 1, w, 5, (g_patch.chEnable & (1u << i)) ? COL_BAR : COL_MID);
    }

    // Input meters: the five things the Workshop is sending.
    for (int i = 0; i < 4; i++) {
        int v = (int)g_in[i] >> 5;                      // 0..127
        srect(14 + i * 44, PLAY_IN_Y + 10, 36, 8, COL_PANEL);
        srect(14 + i * 44, PLAY_IN_Y + 10, (v * 36) / 127, 8, COL_BAR);
    }
    srect(14 + 4 * 44, PLAY_IN_Y + 10, 36, 8, g_gate ? COL_OK : COL_PANEL);

    // Link health. g_streamBad counts words the tag or check bits rejected: on a healthy link
    // it simply does not move, so any climb at all is worth seeing.
    static uint32_t lastBad = 0xFFFFFFFFu;
    if (g_streamBad != lastBad) {
        lastBad = g_streamBad;
        srect(150, 136, 84, 8, COL_BG);
        dec32(buf, g_streamBad, 5);
        text(150, 136, "REJ", COL_DIM, 1);
        text(176, 136, buf, g_streamBad ? COL_WAIT : COL_DIM, 1);
    }
}

// ---- EDIT view ------------------------------------------------------------------------------
#define EDIT_TOP 24
#define EDIT_ROW 12

// Cleared whenever the whole page is repainted, so the incremental redraw below knows the
// screen no longer matches what it last drew.
static int g_editCacheValid = 0;

static void edit_static(void)
{
    char buf[8];
    g_editCacheValid = 0;
    rect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
    text(14, 4, "EDIT", COL_DIM, 1);
    text(46, 4, page_name[g_page], COL_TITLE, 1);

    dec32(buf, (uint32_t)g_page + 1, 1);
    text(196, 4, buf, COL_DIM, 1);
    text(202, 4, "/", COL_DIM, 1);
    dec32(buf, PAGE_COUNT, 1);
    text(208, 4, buf, COL_DIM, 1);

    rect(14, 16, SCREEN_W - 28, 1, COL_DIM);
    text(14, 148, "SELECT PAGE", COL_DIM, 1);
    text(120, 148, "L/R FAST", COL_DIM, 1);
    text(180, 148, "START PLAY", COL_DIM, 1);
}

// Redraw only the rows that actually changed.
//
// Drawing every row every frame is about 5600 rect() calls, which will not fit in a frame:
// the editor would run at single-figure frames per second and the 25 Hz key repeat would be
// throttled by the redraw rather than by the repeat rate. Comparing the rendered value string
// against what is already on screen means a still page costs nothing and a held D-pad repaints
// one row.
static void edit_frame(void)
{
    static char cacheVal[10][20];
    static int  cacheSel = -1;
    char buf[20];
    int rows = page_rows[g_page];

    if (!g_editCacheValid) { g_editCacheValid = 1; cacheSel = -1; for (int r = 0; r < 10; r++) cacheVal[r][0] = 1; }

    for (int r = 0; r < rows; r++) {
        int y   = EDIT_TOP + r * EDIT_ROW;
        int sel = (r == g_row);

        field_value(g_page, r, buf);
        if (sel == (r == cacheSel) && streq(buf, cacheVal[r])) continue;

        srect(10, y - 1, SCREEN_W - 20, EDIT_ROW - 1, sel ? COL_PANEL : COL_BG);
        text(16, y, field_label(g_page, r), sel ? COL_SEL : COL_DIM, 1);
        text(148, y, buf, sel ? COL_TITLE : COL_MID, 1);

        scopy(cacheVal[r], 0, buf);
    }
    cacheSel = g_row;

    // Live pitch readout under the CAL page: trimming the scale by ear is guesswork without
    // seeing what the incoming voltage is actually being read as.
    if (g_page == PAGE_CAL) {
        // Always redrawn: it tracks the incoming voltage, which is the whole point of trimming
        // the scale while watching it.
        int y = EDIT_TOP + rows * EDIT_ROW + 4;
        srect(10, y, SCREEN_W - 20, 10, COL_BG);
        note_text(buf, g_note);
        text(16, y, "READS", COL_DIM, 1);
        text(60, y, buf, COL_OK, 1);
        dec32(buf, g_in[LINK_IN_CV2], 5);
        text(110, y, "RAW", COL_DIM, 1);
        text(138, y, buf, COL_MID, 1);
    }
}

// ---- dispatch --------------------------------------------------------------------------------
void ui_init(void)
{
    g_page = PAGE_VOICE;
    g_row  = 0;
    g_repaint = 1;
}

void ui_frame(void)
{
    uint16_t edges = synth_take_edges();
    uint16_t steps = synth_take_steps();

    if (edges & KEY_START) {
        g_editMode = (uint8_t)!g_editMode;
        g_repaint = 1;
    }

    if (g_editMode) {
        if (edges & KEY_SELECT) {
            g_page = (uint8_t)((g_page + 1) % PAGE_COUNT);
            g_row = 0;
            g_repaint = 1;
        }
        // Wrap by comparison, not by %. The divisor is a variable, and we link -nostdlib:
        // a modulo here is an undefined reference to __aeabi_idivmod at link time. That
        // tripwire caught this on the first build.
        int rows = page_rows[g_page];
        if (steps & KEY_DOWN)  g_row = (uint8_t)((g_row + 1 >= rows) ? 0 : g_row + 1);
        if (steps & KEY_UP)    g_row = (uint8_t)((g_row == 0) ? rows - 1 : g_row - 1);
        if (steps & KEY_RIGHT) field_adjust(g_page, g_row, +1);
        if (steps & KEY_LEFT)  field_adjust(g_page, g_row, -1);
        if (steps & KEY_R)     field_adjust(g_page, g_row, +8);
        if (steps & KEY_L)     field_adjust(g_page, g_row, -8);
        if (edges & KEY_A)     field_adjust(g_page, g_row, +1);
    }

    if (g_editMode != g_lastMode) { g_lastMode = g_editMode; g_repaint = 1; }

    if (g_repaint) {
        g_repaint = 0;
        if (g_editMode) edit_static(); else play_static();
    }

    if (g_editMode) edit_frame(); else play_frame();

    link_post_status(g_page, g_editMode ? GBA_MODE_EDIT : GBA_MODE_PLAY,
                     (uint8_t)((g_noteOn ? GBA_FLAG_NOTE_ON : 0) |
                               (g_gate   ? GBA_FLAG_GATE    : 0) |
                               (g_latch  ? GBA_FLAG_LATCH   : 0)));
}
