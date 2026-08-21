// 89_GBA — GBA Multiboot link for the Music Thing Workshop Computer
//
// Turns the module's pulse jacks into a Game Boy Advance link-cable master: uploads a
// small payload into a cartridge-less GBA via BIOS Multiboot, then uses the GBA as a
// controller/screen for the synth over a live SPI link.
//
//   Pulse Out 1 (GPIO 8) -> GBA SC  (serial clock, master-driven)
//   Pulse Out 2 (GPIO 9) -> GBA SI  (MOSI)
//   Pulse In  1 (GPIO 2) -> GBA SO  (MISO)
//   (common ground)
//
// Architecture: ComputerCard's 48 kHz audio/CV loop runs on core 0 (ProcessSample below).
// The entire GBA link engine runs on core 1 (gba_link_core1) and communicates only through
// the lock-free GbaShared struct — the audio path is never blocked by the link. This mirrors
// 96_cathode's core-0/core-1 split for the pulse pins.

#include "ComputerCard.h"
#include "pico/multicore.h"

#include "gba_link.h"
#include "gba_payload.h"   // baked multiboot .mb image: gba_payload[] / gba_payload_size

// ─────────────────────────────────────────────────────────────────────────────
// Core 1 trampoline — hands the baked payload to the link engine.
// ─────────────────────────────────────────────────────────────────────────────
static void core1_entry()
{
    gba_link_core1(gba_payload, gba_payload_size);
}

// ─────────────────────────────────────────────────────────────────────────────
// ComputerCard subclass — Core 0 (48 kHz)
// ─────────────────────────────────────────────────────────────────────────────
class GbaCard : public ComputerCard
{
public:
    GbaCard()
    {
        // Launch the link engine on core 1 from the constructor, before Run(). Core 1
        // immediately reassigns GPIO 8/9/2 to PIO; ComputerCard's own gpio use of those
        // pins (PulseOut1/2, PulseIn1) is then simply superseded on the pad. Core 1's
        // PIO/DMA claims don't collide with ComputerCard, which claims its resources later
        // inside Run() (see 96_cathode for the same pattern).
        multicore_launch_core1(core1_entry);
    }

    void __not_in_flash_func(ProcessSample)() override
    {
        // ---- RP2040 -> GBA : stream modular parameters ----
        // v0 smoke test: send the three knobs + CV1 as 0..255 bytes for the GBA to display.
        gGba.params[0] = (uint8_t)(KnobVal(Knob::Main) >> 4);   // 12-bit -> 8-bit
        gGba.params[1] = (uint8_t)(KnobVal(Knob::X)    >> 4);
        gGba.params[2] = (uint8_t)(KnobVal(Knob::Y)    >> 4);
        gGba.params[3] = (uint8_t)((CVIn1() + 2048) >> 4);      // -2048..2047 -> 0..255

        // ---- GBA -> RP2040 : map buttons to synth outputs ----
        uint16_t btn = gGba.buttons;   // published by core 1

        // D-pad up/down nudges a CV; A/B set the two CV outputs to fixed levels; the
        // shoulder buttons gate a pulse-style output on the audio jacks. Deliberately
        // simple — this is the "both directions visibly work" demo.
        static int32_t cv = 0;
        if (btn & GBA_UP)    cv += 4;
        if (btn & GBA_DOWN)  cv -= 4;
        if (cv >  2047) cv =  2047;
        if (cv < -2048) cv = -2048;
        CVOut1((int16_t)cv);

        CVOut2((btn & GBA_A) ? 2000 : ((btn & GBA_B) ? -2000 : 0));

        AudioOut1((btn & GBA_L) ? 2000 : 0);
        AudioOut2((btn & GBA_R) ? 2000 : 0);

        // ---- status on the LEDs (visible without a scope) ----
        // LED0: link booted.  LED1: connecting/uploading.  LED2..5: mirror A/B/L/R.
        LedOn(0, gGba.state == LinkState::Booted);
        LedOn(1, gGba.state == LinkState::Connecting || gGba.state == LinkState::Error);
        LedOn(2, btn & GBA_A);
        LedOn(3, btn & GBA_B);
        LedOn(4, btn & GBA_L);
        LedOn(5, btn & GBA_R);
    }
};

int main()
{
    // 144 MHz — a clean multiple of 48 MHz (as 96_cathode uses), giving tidy PIO clkdivs.
    set_sys_clock_khz(144000, true);

    GbaCard card;
    // No normalisation probe: we don't use pulse-jack connection detection, and GPIO 2/8/9
    // are owned by the PIO link engine, not by ComputerCard's pulse I/O.
    card.Run();   // blocking; core 0 runs the 48 kHz loop, core 1 runs the GBA link
}
