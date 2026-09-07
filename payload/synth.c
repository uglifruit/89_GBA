// synth.c — the instrument.

#include "synth.h"
#include "psg.h"
#include "link.h"
#include "notes.h"

#define REG_BASE     0x04000000
#define REG_KEYINPUT (*(volatile uint16_t *)(REG_BASE + 0x0130))
#define REG_TM0CNT_L (*(volatile uint16_t *)(REG_BASE + 0x0100))
#define REG_TM0CNT_H (*(volatile uint16_t *)(REG_BASE + 0x0102))

// GBA key bits, active-low in the register. Same layout the host uses (gba_link.h GbaKey).
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

const char *dest_name[DEST_COUNT] = {
    "---", "PITCH", "DUTY", "DETUNE", "NOISE", "WAVE", "GLIDE", "DECAY", "SWEEP"
};
const char *src_name[SRC_COUNT] = { "CV 1", "CV 2", "AUD 1", "AUD 2" };

Patch g_patch;

int32_t  g_pitchQ8 = 60 << 8;
uint8_t  g_note    = 60;
uint16_t g_env     = 0;
uint8_t  g_noteOn  = 0;
uint8_t  g_latch   = 0;
uint8_t  g_chLevel[4] = { 0, 0, 0, 0 };

uint16_t g_btn = 0, g_btnEdge = 0, g_btnRep = 0, g_btnStep = 0;
volatile uint16_t g_btnEdgeLatch = 0;
volatile uint16_t g_btnStepLatch = 0;
uint8_t g_editMode = 0;

uint16_t synth_take_edges(void) { uint16_t v = g_btnEdgeLatch; g_btnEdgeLatch = 0; return v; }
uint16_t synth_take_steps(void) { uint16_t v = g_btnStepLatch; g_btnStepLatch = 0; return v; }

// ---- envelope ----
#define ENV_IDLE 0
#define ENV_ATK  1
#define ENV_DEC  2
#define ENV_SUS  3
#define ENV_REL  4
static uint8_t g_envState = ENV_IDLE;

// Increment per control tick for a full 0..65535 traverse. Index 0 is instantaneous; index 15
// is about a minute at the 1 kHz control rate, which is long enough to be useful as a drone
// swell rather than merely being the end of the table.
static const uint16_t ENV_INC[16] = {
    65535, 16384, 8192, 4096, 2048, 1024, 512, 256,
      128,    64,   32,   16,    8,    4,   2,   1
};

// ---- integer divide -----------------------------------------------------------------------
// We link -nostdlib, so libgcc is absent: a divide by a VARIABLE would be an undefined
// reference to __aeabi_uidiv rather than merely slow code. (Divides by a compile-time constant
// are fine — GCC turns those into a multiply and shift, which is why dec32's /10 works.)
// Called only when the CAL page changes cvScale, so the shift-subtract cost is irrelevant.
static uint32_t udiv32(uint32_t n, uint32_t d)
{
    if (d == 0) return 0;
    uint32_t q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= (1u << i); }
    }
    return q;
}

// Reciprocal of cvScale in Q12, so semitones = (counts * recip) >> 12 with no runtime divide.
// Q12 rather than Q16 on purpose: at full scale that is 2048 * 36900, which stays inside
// int32 with room to spare.
static int32_t g_cvRecip = 0;
static int16_t g_cvRecipFor = 0;

static void refresh_cv_recip(void)
{
    if (g_patch.cvScale == g_cvRecipFor) return;
    g_cvRecipFor = g_patch.cvScale;
    int32_t s = g_patch.cvScale;
    // Floor of 64 (4 ADC counts per semitone) is an OVERFLOW guard, not a taste one: recip is
    // (4096 << 12) / cvScale, and semiQ8 multiplies it by up to 4095 counts. At 64 that peaks
    // near 1.07e9, comfortably inside int32; at 32 it would reach 2.147e9 and sit one step
    // from wrapping. ui.c clamps the CAL field to the same floor.
    if (s < 64) s = 64;
    g_cvRecip = (int32_t)udiv32(4096u << 12, (uint32_t)s);
}

void synth_init(void)
{
    Patch *p = &g_patch;

    p->chEnable   = 0x1;                      // channel 1 only: one clean voice to start
    p->duty[0]    = PSG_DUTY_50;
    p->duty[1]    = PSG_DUTY_25;
    p->detune     = 4;                        // a quarter semitone of thickness when ch2 is on
    p->waveSel    = 0;
    p->waveVol    = PSG_WAVE_100;
    p->noiseDiv   = 3;
    p->noiseShift = 4;
    p->noiseWidth = 0;
    p->noiseLevel = 8;
    p->atk        = 0;
    p->dec        = 6;
    p->sus        = 12;
    p->rel        = 5;
    p->retrig     = 1;
    p->sweepTime  = 0;
    p->sweepDir   = 0;
    p->sweepShift = 0;
    p->glide      = 0;
    p->octave     = 0;
    p->masterL    = 7;
    p->masterR    = 7;
    p->ratio      = 2;                        // 100%
    p->baseNote   = 36;                       // 0 V = C2, the classic 1V/oct convention

    // Workshop CV inputs span about +-6 V over 4096 counts: 341 counts/V, 28.44 per semitone.
    // Q4 of 28.44 is 455. There is no factory calibration for the CV INPUTS (ComputerCard
    // calibrates the outputs only), so this is a starting estimate to be trimmed on the CAL
    // page — which is exactly why that page exists.
    p->cvScale    = 455;
    p->cvOffset   = 0;

    p->mod[SRC_CV1].dest   = DEST_DUTY;   p->mod[SRC_CV1].depth  = 40;
    p->mod[SRC_CV2].dest   = DEST_PITCH;  p->mod[SRC_CV2].depth  = 32;   // unity 1V/oct
    p->mod[SRC_AUD1].dest  = DEST_DETUNE; p->mod[SRC_AUD1].depth = 32;
    p->mod[SRC_AUD2].dest  = DEST_NOISE;  p->mod[SRC_AUD2].depth = 40;

    refresh_cv_recip();
    psg_init();

    // Free-running control clock: 16.78 MHz / 1024 = 16384 Hz, so 16 ticks is 1.024 kHz.
    // Paced off a timer rather than VBlank because 60 Hz modulation on a CV-driven voice is
    // audibly steppy.
    REG_TM0CNT_L = 0;
    REG_TM0CNT_H = 0x0083;                    // prescaler 1024, enable
}

// ---- buttons ----
static uint16_t g_btnPrev = 0;
static uint16_t g_repTimer[16];

static void scan_buttons(void)
{
    uint16_t now = (uint16_t)((~REG_KEYINPUT) & 0x03FFu);
    g_btnEdge = (uint16_t)(now & ~g_btnPrev);
    g_btnRep  = 0;

    for (int b = 0; b < 10; b++) {
        uint16_t m = (uint16_t)(1u << b);
        if (!(now & m)) { g_repTimer[b] = 0; continue; }
        if (g_btnEdge & m) { g_repTimer[b] = 300; continue; }   // 300 ms before repeating
        if (g_repTimer[b]) {
            if (--g_repTimer[b] == 0) { g_btnRep |= m; g_repTimer[b] = 40; }   // 25 Hz
        }
    }

    g_btn     = now;
    g_btnPrev = now;
    g_btnStep = (uint16_t)(g_btnEdge | g_btnRep);
    g_btnEdgeLatch |= g_btnEdge;
    g_btnStepLatch |= g_btnStep;
    link_set_buttons(now);
}

// ---- helpers ----
static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Period register for a Q8 MIDI-note pitch. Linear interpolation BETWEEN table entries is
// exact rather than approximate here: the register is an affine function of 1/f, so
// interpolating the register is interpolating the period.
static uint16_t period_for(int32_t pitchQ8, int semitoneShift)
{
    int32_t p = pitchQ8 + ((int32_t)semitoneShift << 8);
    p = clampi(p, (int32_t)NOTE_MIN << 8, ((int32_t)NOTE_MAX - 1) << 8);

    int32_t n    = p >> 8;
    int32_t frac = p & 0xFF;
    int32_t a = NOTE_PERIOD[n];
    int32_t b = NOTE_PERIOD[n + 1];
    return (uint16_t)(a + (((b - a) * frac) >> 8));
}

// ---- the control tick ---------------------------------------------------------------------
static void synth_tick(void)
{
    Patch *p = &g_patch;
    scan_buttons();
    refresh_cv_recip();

    // ---- PLAY-mode performance controls -------------------------------------------------
    // While the editor is open the D-pad belongs to it, so these stand down entirely rather
    // than both acting on the same press.
    if (!g_editMode) {
        if (g_btnStep & KEY_UP)    p->octave = (int8_t)clampi(p->octave + 1, -3, 3);
        if (g_btnStep & KEY_DOWN)  p->octave = (int8_t)clampi(p->octave - 1, -3, 3);
        if (g_btnStep & KEY_RIGHT) p->detune = (int8_t)clampi(p->detune + 1, -64, 63);
        if (g_btnStep & KEY_LEFT)  p->detune = (int8_t)clampi(p->detune - 1, -64, 63);
        if (g_btnEdge & KEY_R)     p->duty[0] = (uint8_t)((p->duty[0] + 1) & 3);
        if (g_btnEdge & KEY_L)     p->duty[1] = (uint8_t)((p->duty[1] + 1) & 3);
        if (g_btnEdge & KEY_B)     g_latch = (uint8_t)!g_latch;
    }

    // ---- modulation matrix ----------------------------------------------------------------
    int32_t modPitchQ8 = 0, modDuty = 0, modDetune = 0, modNoise = 0;
    int32_t modWave = 0, modGlide = 0, modDecay = 0, modSweep = 0;

    for (int s = 0; s < SRC_COUNT; s++) {
        int dest = p->mod[s].dest;
        if (dest == DEST_NONE) continue;
        int32_t depth = p->mod[s].depth;
        int32_t raw   = (int32_t)g_in[s] - 2048;      // -2048..2047, 0 = 0 V

        if (dest == DEST_PITCH) {
            // Depth 32 is unity: one volt in gives exactly one octave. The CV -> semitone
            // conversion uses the CAL page's scale, so tuning is trimmed in one place.
            int32_t semiQ8 = ((raw - p->cvOffset) * g_cvRecip) >> 12;
            modPitchQ8 += (semiQ8 * depth) >> 5;
            continue;
        }

        // Everything else: normalise to roughly -128..127 before scaling by depth.
        int32_t v = (raw * depth) >> 11;
        switch (dest) {
        case DEST_DUTY:   modDuty   += v; break;
        case DEST_DETUNE: modDetune += v; break;
        case DEST_NOISE:  modNoise  += v; break;
        case DEST_WAVE:   modWave   += v; break;
        case DEST_GLIDE:  modGlide  += v; break;
        case DEST_DECAY:  modDecay  += v; break;
        case DEST_SWEEP:  modSweep  += v; break;
        default: break;
        }
    }

    // ---- pitch ----------------------------------------------------------------------------
    int32_t target = ((int32_t)p->baseNote << 8)
                   + ((int32_t)p->octave * 12 << 8)
                   + modPitchQ8;
    target = clampi(target, (int32_t)NOTE_MIN << 8, (int32_t)NOTE_MAX << 8);

    // Glide doubles as the anti-zipper filter. Even at the shortest setting the one-pole
    // smooths the 12-bit CV's 3.5-cent steps into continuous motion; longer settings are
    // portamento. This is why a 12-bit pitch word sounds like a voltage and not like a
    // staircase.
    int32_t glide = clampi((int32_t)p->glide + (modGlide >> 4), 0, 15);
    if (glide == 0) {
        g_pitchQ8 = target;
    } else {
        g_pitchQ8 += (target - g_pitchQ8) >> glide;
        if (target != g_pitchQ8 && ((target - g_pitchQ8) >> glide) == 0) g_pitchQ8 = target;
    }

    uint8_t note = (uint8_t)clampi((g_pitchQ8 + 128) >> 8, 0, 127);

    // ---- gate and envelope ----------------------------------------------------------------
    // Edge-driven, not level-driven: the sticky edge bit means a trigger far shorter than one
    // poll still fires a note.
    int gateOn = g_gate || (g_btn & KEY_A) || g_latch;
    int edge   = 0;
    static uint32_t seenEdges = 0;
    if (g_gateEdges != seenEdges) { seenEdges++; edge = 1; }
    if (g_btnEdge & KEY_A)   { edge = 1; }
    if ((g_btnEdge & KEY_B) && g_latch) { edge = 1; }

    if (edge) {
        g_envState = (p->atk == 0) ? ENV_DEC : ENV_ATK;
        if (p->atk == 0) g_env = 65535;
        else if (p->retrig) g_env = 0;
        g_noteOn = 1;
        // The channels are NOT triggered here. See the trigger block at the end of this
        // function: the envelope volume has to be written first.
    } else if (!gateOn && g_noteOn && g_envState != ENV_REL) {
        g_envState = ENV_REL;
    }

    int32_t susLevel = (int32_t)p->sus * 4369;         // 0..15 -> 0..65535
    int32_t decIx    = clampi((int32_t)p->dec + (modDecay >> 5), 0, 15);

    switch (g_envState) {
    case ENV_ATK: {
        int32_t e = (int32_t)g_env + ENV_INC[p->atk];
        if (e >= 65535) { e = 65535; g_envState = ENV_DEC; }
        g_env = (uint16_t)e;
        break;
    }
    case ENV_DEC: {
        int32_t e = (int32_t)g_env - ENV_INC[decIx];
        if (e <= susLevel) { e = susLevel; g_envState = ENV_SUS; }
        g_env = (uint16_t)e;
        break;
    }
    case ENV_SUS:
        g_env = (uint16_t)susLevel;
        break;
    case ENV_REL: {
        int32_t e = (int32_t)g_env - ENV_INC[p->rel];
        if (e <= 0) { e = 0; g_envState = ENV_IDLE; g_noteOn = 0; }
        g_env = (uint16_t)e;
        break;
    }
    default:
        g_env = 0;
        break;
    }

    uint8_t vol = (uint8_t)(g_env >> 12);              // 0..15

    // Floor an ACTIVE note at 1 rather than 0. Volume 0 with direction 0 switches the DAC off,
    // which would make a slow attack silent for ever: the trigger below is skipped while the
    // volume is 0, and once the attack tick has passed nothing ever fires it again. Volume 1 is
    // the quietest audible step anyway, so nothing is lost. Idle still reaches a true 0, which
    // is what keeps a released note properly silent.
    if (vol == 0 && g_envState != ENV_IDLE) vol = 1;

    // ---- write the voice ------------------------------------------------------------------
    int duty0 = (int)clampi((int32_t)p->duty[0] + (modDuty >> 6), 0, 3);
    int duty1 = (int)clampi((int32_t)p->duty[1] + (modDuty >> 6), 0, 3);
    int detune = (int)clampi((int32_t)p->detune + (modDetune >> 2), -128, 127);

    g_chLevel[0] = (p->chEnable & 0x1) ? vol : 0;
    g_chLevel[1] = (p->chEnable & 0x2) ? vol : 0;
    g_chLevel[2] = (p->chEnable & 0x4) ? vol : 0;
    g_chLevel[3] = (p->chEnable & 0x8) ? (uint8_t)clampi((p->noiseLevel * vol) >> 4, 0, 15) : 0;

    if (p->chEnable & 0x1) {
        psg_sq_voice(PSG_CH1, (uint8_t)duty0, g_chLevel[0]);
        psg_sq_period(PSG_CH1, period_for(g_pitchQ8, 0));
    } else {
        psg_sq_voice(PSG_CH1, (uint8_t)duty0, 0);
    }

    if (p->chEnable & 0x2) {
        psg_sq_voice(PSG_CH2, (uint8_t)duty1, g_chLevel[1]);
        // Detune is in 1/16 semitone; the shift keeps it in the Q8 pitch domain.
        psg_sq_period(PSG_CH2, period_for(g_pitchQ8 + (detune << 4), 0));
    } else {
        psg_sq_voice(PSG_CH2, (uint8_t)duty1, 0);
    }

    if (p->chEnable & 0x4) {
        int wv = (int)clampi((int32_t)p->waveVol + (modWave >> 6), 0, 3);
        psg_wave_voice((uint8_t)(vol ? wv : PSG_WAVE_MUTE));
        psg_wave_period(period_for(g_pitchQ8, 12));
    } else {
        psg_wave_voice(PSG_WAVE_MUTE);
    }

    if (p->chEnable & 0x8) {
        int shift = (int)clampi((int32_t)p->noiseShift + (modNoise >> 5), 0, 13);
        psg_noise_set(p->noiseDiv, (uint8_t)shift, p->noiseWidth);
        psg_noise_voice(g_chLevel[3]);
    } else {
        psg_noise_voice(0);
    }

    // Channel 1 sweep, the one hardware modulator worth keeping — it runs faster than our
    // control rate can.
    {
        int sw = (int)clampi((int32_t)p->sweepShift + (modSweep >> 6), 0, 7);
        psg_sq_sweep((uint8_t)sw, p->sweepDir, p->sweepTime);
    }

    psg_master(p->masterL, p->masterR, p->ratio);
    psg_enable(p->chEnable, p->chEnable);

    // ---- trigger, AND IT HAS TO HAPPEN HERE, AFTER THE VOLUME WRITES ------------------------
    // On this DMG-derived PSG, an envelope register holding volume 0 with direction 0 switches
    // the channel's DAC off, and switching the DAC back on does NOT re-enable the channel: only
    // a trigger does. Releasing a note leaves the envelope at 0, so triggering before writing
    // the new volume would arm a channel whose DAC is still off. The first note would sound and
    // every note after it would be silent. Volume first, trigger last, and it costs nothing.
    //
    // Newly enabled channels are triggered too. Without that, switching CH2 ON in the editor
    // while a note is held does nothing audible until the next gate, which reads as a broken
    // control rather than as correct behaviour.
    {
        static uint8_t lastEnable = 0;
        uint8_t fresh = (uint8_t)(p->chEnable & ~lastEnable);
        lastEnable = p->chEnable;

        uint8_t fire = (uint8_t)(edge ? p->chEnable : fresh);
        if (fire && vol) {
            if (fire & 0x1) psg_sq_trigger(PSG_CH1, period_for(g_pitchQ8, 0));
            if (fire & 0x2) psg_sq_trigger(PSG_CH2, period_for(g_pitchQ8 + (detune << 4), 0));
            if (fire & 0x4) psg_wave_trigger(period_for(g_pitchQ8, 12));
            if (fire & 0x8) psg_noise_trigger();
        }
    }

    // ---- publish upstream ------------------------------------------------------------------
    // Only when something actually changed, so the priority slot stays free for the next real
    // event rather than being spent restating the same note a thousand times a second.
    {
        static uint8_t lastNote = 0xFF, lastGate = 0xFF;
        uint8_t g = (uint8_t)(g_noteOn ? (vol ? vol : 1) : 0);
        if (note != lastNote || g != lastGate) {
            lastNote = note; lastGate = g;
            g_note = note;
            link_post_note(note, g);
        }
    }
}

void synth_update(void)
{
    static uint16_t last = 0;
    static int primed = 0;
    if (!primed) { last = REG_TM0CNT_L; primed = 1; }

    uint16_t now = REG_TM0CNT_L;
    // Bounded catch-up. If a long redraw ate several ticks we do NOT run them all: the
    // envelope would lurch and the glide would jump. Running at most a few keeps time roughly
    // honest without turning a slow frame into an audible artefact.
    int budget = 8;
    while ((uint16_t)(now - last) >= 16 && budget--) {
        last = (uint16_t)(last + 16);
        synth_tick();
    }
    if ((uint16_t)(now - last) >= 16) last = now;   // gave up catching up; resync
}
