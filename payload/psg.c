// psg.c — GBA PSG register access. See psg.h for the rules that matter.

#include "psg.h"

#define REG_BASE 0x04000000
#define R16(off) (*(volatile uint16_t *)(REG_BASE + (off)))

#define REG_SOUND1CNT_L R16(0x0060)   // sweep
#define REG_SOUND1CNT_H R16(0x0062)   // duty / length / envelope
#define REG_SOUND1CNT_X R16(0x0064)   // frequency / trigger
#define REG_SOUND2CNT_L R16(0x0068)   // duty / length / envelope
#define REG_SOUND2CNT_H R16(0x006C)   // frequency / trigger
#define REG_SOUND3CNT_L R16(0x0070)   // wave dimension / bank / enable
#define REG_SOUND3CNT_H R16(0x0072)   // length / volume
#define REG_SOUND3CNT_X R16(0x0074)   // rate / trigger
#define REG_SOUND4CNT_L R16(0x0078)   // length / envelope
#define REG_SOUND4CNT_H R16(0x007C)   // divisor / width / shift / trigger
#define REG_SOUNDCNT_L  R16(0x0080)   // master volume + per-channel pan
#define REG_SOUNDCNT_H  R16(0x0082)   // PSG:DMA mix ratio
#define REG_SOUNDCNT_X  R16(0x0084)   // master enable
#define REG_SOUNDBIAS   R16(0x0088)

#define WAVE_RAM ((volatile uint16_t *)(REG_BASE + 0x0090))

#define TRIGGER (1 << 15)

// ---- built-in channel-3 wavetables ----
// 32 four-bit samples packed two per byte, HIGH nybble played first.
const uint8_t psg_wave_preset[PSG_WAVE_PRESETS][16] = {
    // Sine-ish
    { 0x89, 0xAB, 0xCD, 0xEE, 0xFF, 0xEE, 0xDC, 0xBA,
      0x98, 0x76, 0x54, 0x32, 0x11, 0x00, 0x11, 0x23 },
    // Triangle
    { 0x89, 0xAB, 0xCD, 0xEF, 0xFE, 0xDC, 0xBA, 0x98,
      0x76, 0x54, 0x32, 0x10, 0x01, 0x23, 0x45, 0x67 },
    // Saw
    { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
      0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF },
    // Square
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    // Narrow pulse
    { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    // Organ-ish: two stacked octaves
    { 0x8B, 0xDE, 0xFE, 0xDB, 0x85, 0x21, 0x01, 0x25,
      0x8B, 0xDE, 0xFE, 0xDB, 0x85, 0x21, 0x01, 0x25 },
};

const char *psg_wave_name[PSG_WAVE_PRESETS] = {
    "SINE", "TRI", "SAW", "SQUARE", "PULSE", "ORGAN"
};

void psg_init(void)
{
    // MASTER ENABLE FIRST. Every other write below is a no-op until this is set.
    REG_SOUNDCNT_X = 0x0080;

    REG_SOUNDBIAS  = 0x0200;          // 9-bit / 32.768 kHz, the BIOS default made explicit
    REG_SOUNDCNT_H = 0x0002;          // PSG at 100%, no DMA sound
    REG_SOUNDCNT_L = 0;               // synth.c sets volume and pan

    REG_SOUND1CNT_L = 0;              // sweep off
    REG_SOUND1CNT_H = 0;
    REG_SOUND1CNT_X = 0;
    REG_SOUND2CNT_L = 0;
    REG_SOUND2CNT_H = 0;
    REG_SOUND3CNT_L = 0;
    REG_SOUND3CNT_H = 0;
    REG_SOUND3CNT_X = 0;
    REG_SOUND4CNT_L = 0;
    REG_SOUND4CNT_H = 0;

    psg_wave_load(psg_wave_preset[0]);
}

void psg_master(uint8_t volL, uint8_t volR, uint8_t ratio)
{
    uint16_t cnt = REG_SOUNDCNT_L;
    cnt = (uint16_t)((cnt & 0xFF00u) | ((volL & 7u) << 4) | (volR & 7u));
    REG_SOUNDCNT_L = cnt;
    REG_SOUNDCNT_H = (uint16_t)((REG_SOUNDCNT_H & ~0x3u) | (ratio & 0x3u));
}

void psg_enable(uint8_t maskL, uint8_t maskR)
{
    uint16_t cnt = REG_SOUNDCNT_L;
    cnt = (uint16_t)((cnt & 0x00FFu) | ((maskL & 0xFu) << 12) | ((maskR & 0xFu) << 8));
    REG_SOUNDCNT_L = cnt;
}

// ---- squares ----
// Envelope step is parked at 0 (no hardware sweep of volume) and direction at 0, so the value
// written here IS the instantaneous level. Length is left disabled so the note sustains until
// we say otherwise.
void psg_sq_voice(int ch, uint8_t duty, uint8_t vol)
{
    uint16_t v = (uint16_t)(((vol & 0xFu) << 12) | ((duty & 0x3u) << 6));
    if (ch == PSG_CH1) REG_SOUND1CNT_H = v;
    else               REG_SOUND2CNT_L = v;
}

void psg_sq_period(int ch, uint16_t period)
{
    if (ch == PSG_CH1) REG_SOUND1CNT_X = (uint16_t)(period & 0x7FFu);
    else               REG_SOUND2CNT_H = (uint16_t)(period & 0x7FFu);
}

void psg_sq_trigger(int ch, uint16_t period)
{
    if (ch == PSG_CH1) REG_SOUND1CNT_X = (uint16_t)((period & 0x7FFu) | TRIGGER);
    else               REG_SOUND2CNT_H = (uint16_t)((period & 0x7FFu) | TRIGGER);
}

void psg_sq_sweep(uint8_t shift, uint8_t dir, uint8_t time)
{
    REG_SOUND1CNT_L = (uint16_t)((shift & 7u) | ((dir & 1u) << 3) | ((time & 7u) << 4));
}

// ---- channel 3, wave ----
// SOUND3CNT_L bit 6 selects the bank that PLAYS; the CPU sees the OTHER one. So we write the
// idle bank and then flip, which also means the swap is glitch-free.
void psg_wave_load(const uint8_t *wave16)
{
    uint16_t cnt  = REG_SOUND3CNT_L;
    uint16_t bank = (uint16_t)((cnt >> 6) & 1u);

    for (int i = 0; i < 8; i++) {
        WAVE_RAM[i] = (uint16_t)(wave16[i * 2] | (wave16[i * 2 + 1] << 8));
    }

    // Play the bank we just filled, keep 32-step (one bank) mode, DAC on.
    REG_SOUND3CNT_L = (uint16_t)((cnt & ~((1u << 5) | (1u << 6))) | ((bank ^ 1u) << 6) | 0x80u);
}

void psg_wave_voice(uint8_t volSel)
{
    REG_SOUND3CNT_L = (uint16_t)(REG_SOUND3CNT_L | 0x80u);        // DAC on
    REG_SOUND3CNT_H = (uint16_t)((volSel & 0x3u) << 13);
}

void psg_wave_period(uint16_t period) { REG_SOUND3CNT_X = (uint16_t)(period & 0x7FFu); }
void psg_wave_trigger(uint16_t period) { REG_SOUND3CNT_X = (uint16_t)((period & 0x7FFu) | TRIGGER); }

// ---- channel 4, noise ----
void psg_noise_voice(uint8_t vol)
{
    REG_SOUND4CNT_L = (uint16_t)((vol & 0xFu) << 12);
}

static uint8_t g_noiseCtl = 0;

void psg_noise_set(uint8_t divisor, uint8_t shift, uint8_t width)
{
    g_noiseCtl = (uint8_t)((divisor & 0x7u) | ((width & 1u) << 3) | ((shift & 0xFu) << 4));
    REG_SOUND4CNT_H = g_noiseCtl;
}

void psg_noise_trigger(void)
{
    REG_SOUND4CNT_H = (uint16_t)(g_noiseCtl | TRIGGER);
}
