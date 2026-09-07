// 89_GBA — GBA PSG voice for the Music Thing Workshop Computer
//
// Boots a cartridge-less Game Boy Advance over the pulse jacks via BIOS Multiboot, then runs a
// live SPI link so the GBA is a synth voice played by the modular — audio out of its own
// headphone jack, sound editing on its own screen.
//
//   Pulse Out 1 (GPIO 8) -> GBA SC  (serial clock, master-driven)
//   Pulse Out 2 (GPIO 9) -> GBA SI  (MOSI)
//   Pulse In  1 (GPIO 2) -> GBA SO  (MISO)
//   (common ground)
//
// THE DIVISION OF LABOUR: the Workshop senses, the GBA is the instrument. This side reads the
// five inputs and streams them down raw; the GBA owns pitch tracking, the modulation matrix,
// the envelope and the UI. That is what makes the on-screen editor free — re-mapping an input
// is a change to a table in GBA RAM, not a protocol change and not a firmware rebuild.
//
// Panel:
//   CV In 2      1V/oct pitch          Pulse In 2   gate / trigger
//   CV In 1      timbre (pulse duty)   Audio In 1   channel-2 detune
//   Audio In 2   noise level           (all four are re-assignable on the GBA's MAP page)
//   CV Out 2     quantised pitch, calibrated 1V/oct
//   CV Out 1     gate mirror, 5 V
//
// Architecture: ComputerCard's 48 kHz audio/CV loop runs on core 0 (ProcessSample below). The
// entire GBA link engine runs on core 1 (gba_link_core1) and communicates only through the
// lock-free GbaShared struct — the audio path is never blocked by the link. This mirrors
// 96_cathode's core-0/core-1 split for the pulse pins.

#include "ComputerCard.h"
#include "pico/multicore.h"

#include "gba_link.h"
#include "gba_proto.h"
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
        // Tell the GBA whether the quantised pitch out can be trusted, BEFORE core 1 starts:
        // the EEPROM calibration is read by ComputerCard's own constructor, which has already
        // run by the time we get here, and the link sends its first HELLO within milliseconds
        // of being launched below.
        gGba.caps = CVOutsCalibrated() ? GBA_CAP_CVOUT_CAL : 0;

        // Launch the link engine on core 1 from the constructor, before Run(). Core 1
        // immediately reassigns GPIO 8/9/2 to PIO; ComputerCard's own gpio use of those
        // pins (PulseOut1/2, PulseIn1) is then simply superseded on the pad. Core 1's
        // PIO/DMA claims don't collide with ComputerCard, which claims its resources later
        // inside Run() (see 96_cathode for the same pattern).
        multicore_launch_core1(core1_entry);
    }

    void __not_in_flash_func(ProcessSample)() override
    {
        // ---- Workshop -> GBA : the five inputs ----
        // Signed -2048..2047 biased to 12-bit unsigned for the wire. The GBA subtracts 2048
        // again, so 0 V is 0 V at both ends and there is exactly one place the convention
        // lives (gba_proto.h).
        gGba.inputs[GBA_IN_CV1]  = (uint16_t)(CVIn1()    + 2048);
        gGba.inputs[GBA_IN_CV2]  = (uint16_t)(CVIn2()    + 2048);
        gGba.inputs[GBA_IN_AUD1] = (uint16_t)(AudioIn1() + 2048);
        gGba.inputs[GBA_IN_AUD2] = (uint16_t)(AudioIn2() + 2048);

        gGba.gate = PulseIn2() ? 1 : 0;

        // A monotonic counter, not a sticky flag: core 1 is the only reader and it advances
        // its own tally by one per report, so a trigger far shorter than the 1 ms poll can
        // neither be lost to a clear-vs-set race nor collapsed into its neighbour.
        if (PulseIn2RisingEdge()) gGba.gateEdges = gGba.gateEdges + 1;

        gGba.knobs[0] = (uint16_t)KnobVal(Knob::Main);
        gGba.knobs[1] = (uint16_t)KnobVal(Knob::X);
        gGba.knobs[2] = (uint16_t)KnobVal(Knob::Y);
        gGba.switchPos = (uint8_t)SwitchVal();

        // ---- GBA -> Workshop : the note it is sounding ----
        // CVOut2MIDINote is CALIBRATED from the module's EEPROM, so the quantised pitch out is
        // in tune with the rest of the rack without any trimming here. (The CV INPUTS have no
        // such calibration, which is why the GBA carries a CAL page for the pitch input.)
        CVOut2MIDINote(gGba.note);
        CVOut1Millivolts(gGba.noteVel ? 5000 : 0);

        // Audio outs mirror the gate as a trigger pair — handy for chaining, and it makes the
        // module visibly alive even with nothing patched to CV.
        AudioOut1(gGba.noteVel ? 2000 : 0);
        AudioOut2(gGba.gate    ? 2000 : 0);

        // ---- status on the LEDs ----
        // 0 link, 1 gate in, 2 note sounding, 3 editing, 4+5 the edit page as a binary pair —
        // the readout convention every diagnostic in this project already uses.
        bool booted = (gGba.state == LinkState::Booted);
        LedOn(0, booted ? true : ((tick_ >> 12) & 1));   // solid = booted, blinking = connecting
        LedOn(1, gGba.gate);
        LedOn(2, gGba.noteVel != 0);
        LedOn(3, gGba.mode == GBA_MODE_EDIT);
        LedOn(4, (gGba.page & 1) != 0);
        LedOn(5, (gGba.page & 2) != 0);

        tick_++;
    }

private:
    uint32_t tick_ = 0;
};

int main()
{
    // 144 MHz — a clean multiple of 48 MHz (as 96_cathode uses), giving tidy PIO clkdivs.
    set_sys_clock_khz(144000, true);

    GbaCard card;
    card.EnableNormalisationProbe();   // so an unpatched input reads 0 V instead of noise
    // GPIO 2/8/9 are owned by the PIO link engine, not by ComputerCard's pulse I/O.
    card.Run();   // blocking; core 0 runs the 48 kHz loop, core 1 runs the GBA link
}
