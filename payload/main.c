// payload/main.c — GBA-side multiboot payload for the Workshop Computer link.
//
// Runs from EWRAM after the RP2040 uploads it via BIOS Multiboot. It is a PSG synth voice
// played by the modular: the Workshop streams its five inputs down, this side owns pitch
// tracking, the modulation matrix, the envelope, the sound and the UI.
//
// Order of business, and the order matters:
//   1. LCD up and the title drawn IMMEDIATELY, before anything else. A blank screen therefore
//      means the image never ran — a completely different fault from "ran but the link is
//      quiet", and before this the two were indistinguishable from the bench.
//   2. PSG up (psg_init sets SOUNDCNT_X bit 7 first, without which every other sound register
//      write is silently discarded).
//   3. Serial slave up, armed, and the IRQ pointed at it.
//   4. Loop: run the control tick, draw a frame, keep the link serviced throughout.
//
// Dependency-free (no libgba, no libc) so it builds with a bare arm-none-eabi toolchain
// targeting armv4t. See build.sh.

#include <stdint.h>

#include "gfx.h"
#include "link.h"
#include "psg.h"
#include "synth.h"
#include "ui.h"
#include "diag.h"

#define REG_BASE   0x04000000
#define REG_VCOUNT (*(volatile uint16_t *)(REG_BASE + 0x0006))

int main(void)
{
    // ---- 1. LCD up and the boot proof on screen ----
    gfx_init();
    rect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
    text_centre(60, "MTM - Workshop Computer Link", COL_TITLE, 1);
    text_centre(76, "GBA PSG VOICE", COL_OK, 1);
    rect(40, 92, SCREEN_W - 80, 1, COL_DIM);
    text_centre(100, "waiting for host", COL_DIM, 1);

    // ---- 2. Sound, 3. link ----
    synth_init();      // calls psg_init(), which sets the master enable first
    link_init();
    ui_init();

    // Hold the splash until the host actually says something, so "booted" and "connected" stay
    // visibly different states.
    {
        uint32_t guard = 0;
        while (g_rx == 0 && ++guard < 2000000u) link_service();
    }

    for (;;) {
        synth_update();

        // The diagnostics take over the whole screen when their magic words arrive. Their
        // magics live under the protocol tag reserved for them, so a live stream word can
        // never fall into this by accident.
        if (diag_active()) {
            diag_frame();
            continue;
        }

        ui_frame();

        // Pace to roughly one frame without ever blocking the link.
        while (REG_VCOUNT <  SCREEN_H) link_service();
        while (REG_VCOUNT >= SCREEN_H) link_service();
    }
}
