// gba_spi.h — Pin map and low-level SPI32 transport for the GBA link.
//
// The transport is a PIO state machine (gba_spi.pio) running mode-3, MSB-first, 32-bit
// full-duplex SPI on the Workshop Computer's pulse jacks. All Workshop hardware inversion
// is absorbed by IO-pad overrides in gba_spi_init(), so callers work in clean logical terms.
#pragma once

#include <cstdint>
#include "hardware/pio.h"
#include "hardware/gpio.h"   // GPIO_OVERRIDE_* used by the constants below

// ---- Pin routing (fixed by the user's wiring) --------------------------------
//   SCK (GBA SC) -> Pulse Out 1 -> GPIO 8
//   SI  (MOSI)   -> Pulse Out 2 -> GPIO 9
//   SO  (MISO)   -> Pulse In 1  -> GPIO 2
static constexpr uint GBA_SCK_PIN  = 8;
static constexpr uint GBA_MOSI_PIN = 9;
static constexpr uint GBA_MISO_PIN = 2;

// Which PIO block/SM the link engine owns. ComputerCard leaves both PIO blocks free;
// we take PIO0 SM0 (cathode's precedent for pulse-pin PIO on core 1).
#define GBA_PIO   pio0
#define GBA_SM    0u

// Bring up the PIO SPI at the given SCK frequency (Hz). Reclaims GPIO 8/9/2 from
// ComputerCard's gpio control, installs pad inversion, loads and starts the SM.
void gba_spi_init(uint32_t sck_hz);

// Change the SCK frequency without reloading the program (used to slow down for
// multiboot vs. speed up — modestly — for the post-boot polling loop).
void gba_spi_set_clock(uint32_t sck_hz);

// One blocking 32-bit full-duplex exchange. Sends `w` MSB-first, returns what the GBA
// clocked back on MISO over the same 32 clocks. Runs entirely from RAM (core 1).
uint32_t gba_spi_xfer32(uint32_t w);

// ---- Measured pad inversions (scope-verified 2026-09-03) ----------------------
// Single source of truth: the applet and every diagnostic MUST use these, so the four
// init sites cannot silently disagree again.
//
// MISO: measured on the scope with a patch cable Pulse Out 2 -> Pulse In 1, probing both
// ends. The receiving end followed the driving end (both high together) => the Workshop
// input stage does NOT invert. ComputerCard's PulseIn1() returning !gpio_get is a SOFTWARE
// convention, not a pad inversion; the earlier INVERT here was that conflation and was a
// real bug (it would have broken multiboot even over a perfect cable).
//
// SCK: CPOL=1 wants SCK inverted, and the Workshop output stage already inverts Pulse Out 1;
// the two cancel => NORMAL. Confirmed on the scope: with the SM stalled on `side 0`, the SC
// jack idles HIGH at ~6 V, which is what a low pad through an inverting output looks like.
//
// MOSI: Workshop output stage inverts Pulse Out 2 once => INVERT.
#define GBA_SCK_OUTOVER   GPIO_OVERRIDE_NORMAL
#define GBA_MOSI_OUTOVER  GPIO_OVERRIDE_INVERT
#define GBA_MISO_INOVER   GPIO_OVERRIDE_NORMAL

// PIO cycles consumed per SPI bit, per program. Both programs are 4 cycles/bit:
//   gba_spi       : out(1) + mov[1](2) + in(1)   = 4
//   gba_spi_cpha0 : out[1](2) + in[1](2)         = 4
// The old code divided by 3 here, so every requested rate ran 4/3 too fast (100 kHz asked
// -> 75 kHz actual). Kept as a named constant so the scope agrees with the source.
static constexpr float GBA_PIO_CYCLES_PER_BIT = 4.0f;
