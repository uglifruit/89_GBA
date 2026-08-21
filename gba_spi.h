// gba_spi.h — Pin map and low-level SPI32 transport for the GBA link.
//
// The transport is a PIO state machine (gba_spi.pio) running mode-3, MSB-first, 32-bit
// full-duplex SPI on the Workshop Computer's pulse jacks. All Workshop hardware inversion
// is absorbed by IO-pad overrides in gba_spi_init(), so callers work in clean logical terms.
#pragma once

#include <cstdint>
#include "hardware/pio.h"

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
