// gba_link.h — Cross-core shared state + the whole core-1 GBA link engine.
//
// Core 1 owns the physical link: it runs multiboot, then the post-boot polling loop, and
// publishes the GBA's button state / consumes the module's parameters through GbaShared.
// Core 0 (ComputerCard's 48 kHz ProcessSample) only ever touches GbaShared — never the SPI.
//
// Concurrency model (mirrors 96_cathode's single-flag core sync): each field has a single
// writer. Buttons: written by core 1, read by core 0. params[]: written by core 0, read by
// core 1. `volatile` + word-aligned 32-bit accesses are atomic enough on RP2040 for this
// (no field is wider than a word, and we never need a consistent multi-field snapshot).
#pragma once

#include <cstdint>

// GBA button bitfield, active-high (already de-inverted from REG_KEYINPUT by the payload).
// Matches the standard GBA key bit order so the payload and host agree.
enum GbaKey : uint16_t {
    GBA_A      = 1u << 0,
    GBA_B      = 1u << 1,
    GBA_SELECT = 1u << 2,
    GBA_START  = 1u << 3,
    GBA_RIGHT  = 1u << 4,
    GBA_LEFT   = 1u << 5,
    GBA_UP     = 1u << 6,
    GBA_DOWN   = 1u << 7,
    GBA_R      = 1u << 8,
    GBA_L      = 1u << 9,
};

enum class LinkState : uint8_t {
    Idle,        // not started
    Connecting,  // running multiboot handshake
    Booted,      // payload uploaded, polling loop running
    Error,       // multiboot failed; will retry
};

struct GbaShared {
    // ---- core 1 -> core 0 ----
    volatile uint16_t  buttons   = 0;   // current GBA key state (GbaKey bits)
    volatile uint32_t  rxSeq     = 0;   // increments each successful poll (liveness)
    volatile LinkState state     = LinkState::Idle;
    volatile uint8_t   lastError = 0;   // MultibootResult of the last failed attempt
    // SCK rate (Hz) currently being used. Core 1 walks a ladder of rates until multiboot
    // succeeds — the link was first proven at ~1 kHz and 100 kHz has never been shown to
    // work through the slow Pulse In 1 transistor stage — so the working rate is a runtime
    // fact, not a constant, and worth surfacing.
    volatile uint32_t  linkHz    = 0;

    // ---- core 0 -> core 1 ----
    // Up to 4 modular parameters streamed to the GBA (e.g. knob/CV values, 0..255).
    // The payload displays these; v0 uses params[0] as a bar / colour.
    volatile uint8_t   params[4] = { 0, 0, 0, 0 };
};

// The single shared instance (defined in gba_link.cpp).
extern GbaShared gGba;

// Core-1 entry point. Init the SPI transport, upload the payload (retrying on NoGBA),
// then loop forever exchanging button/param packets. Never returns.
// `payload`/`payload_size` is the multiboot .mb image baked into flash.
void gba_link_core1(const uint8_t *payload, uint32_t payload_size);
