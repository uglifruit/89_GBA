// synth.h — the instrument: pitch tracking, modulation matrix, envelope, voice state.
//
// Everything musical lives on this side of the link. The Workshop streams five raw inputs and
// nothing else; what they DO is decided here, which is why the whole editor costs no protocol.
#pragma once

#include <stdint.h>

// ---- modulation destinations ----
#define DEST_NONE    0
#define DEST_PITCH   1
#define DEST_DUTY    2
#define DEST_DETUNE  3
#define DEST_NOISE   4
#define DEST_WAVE    5
#define DEST_GLIDE   6
#define DEST_DECAY   7
#define DEST_SWEEP   8
#define DEST_COUNT   9

extern const char *dest_name[DEST_COUNT];

// Modulation sources, in the order they appear on the MAP page and in g_in[].
#define SRC_CV1  0
#define SRC_CV2  1
#define SRC_AUD1 2
#define SRC_AUD2 3
#define SRC_COUNT 4

extern const char *src_name[SRC_COUNT];

typedef struct {
    uint8_t dest;
    int8_t  depth;    // -64..+63. For DEST_PITCH, +32 is unity 1V/oct — see synth.c.
} ModSlot;

typedef struct {
    uint8_t chEnable;        // bit per PSG channel 0..3
    uint8_t duty[2];         // ch1, ch2
    int8_t  detune;          // ch2 offset in 1/16 semitone
    uint8_t waveSel;         // index into psg_wave_preset
    uint8_t waveVol;         // PSG_WAVE_*
    uint8_t noiseDiv, noiseShift, noiseWidth, noiseLevel;
    uint8_t atk, dec, sus, rel;   // 0..15
    uint8_t retrig;               // retrigger the envelope on every gate edge
    uint8_t sweepTime, sweepDir, sweepShift;
    uint8_t glide;                // 0..15, 0 = instant
    int8_t  octave;               // -3..+3
    uint8_t masterL, masterR;     // 0..7
    uint8_t ratio;                // 0 = 25%, 1 = 50%, 2 = 100%
    uint8_t baseNote;             // MIDI note produced by 0 V on the pitch input
    int16_t cvScale;              // ADC counts per semitone, Q4 (455 = 28.4 counts)
    int16_t cvOffset;             // ADC counts of offset trim
    ModSlot mod[SRC_COUNT];
} Patch;

extern Patch g_patch;

// ---- live voice state, for the UI to display ----
extern int32_t  g_pitchQ8;     // current (glided) pitch in Q8 MIDI-note units
extern uint8_t  g_note;        // nearest MIDI note, published upstream
extern uint16_t g_env;         // 0..65535
extern uint8_t  g_noteOn;
extern uint8_t  g_latch;
extern uint8_t  g_chLevel[4];  // 0..15, what each channel is actually sounding at

void synth_init(void);

// Runs the control-rate update as many times as the free-running timer says are due. Called
// from the main loop; cheap when nothing is owed.
void synth_update(void);

// Button edge/repeat state, recomputed each control tick. The UI reads these rather than
// polling REG_KEYINPUT itself, so PLAY and EDIT agree on exactly one debounced view.
extern uint16_t g_btn;      // level
extern uint16_t g_btnEdge;  // pressed this tick
extern uint16_t g_btnRep;   // auto-repeat fired this tick
extern uint16_t g_btnStep;  // edge OR repeat: what a "nudge this value" control wants

// Buttons are scanned at the 1 kHz control rate but the UI runs at frame rate, so edges would
// be missed if it read g_btnEdge directly. These latches accumulate until the UI drains them.
extern volatile uint16_t g_btnEdgeLatch;
extern volatile uint16_t g_btnStepLatch;
uint16_t synth_take_edges(void);   // drain g_btnEdgeLatch
uint16_t synth_take_steps(void);   // drain g_btnStepLatch

// Set by the UI. While editing, the D-pad belongs to the editor, so the PLAY-mode performance
// controls (octave, detune, duty) stand down rather than both acting on the same press.
extern uint8_t g_editMode;
