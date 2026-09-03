// linkcheck.cpp — LED-only GBA link diagnostic. No scope, no held switch.
//
// Written because the two things the earlier diagnostics assumed are not available at the
// moment they matter most:
//   * the SWITCH is momentary-down — you cannot hold it while also handling a console;
//   * the SCOPE probes are in the way once the GBA is actually plugged in.
//
// So: the switch CLICKS to advance (edge-triggered, latched — press and let go), and every
// result is reported on the six LEDs, including the 32-bit reply word, which the older tools
// could only hint at.
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// CONTROLS
//   Switch DOWN (click)  = next test / re-run current test
//   Switch UP   (click)  = previous test
//   Switch UP   (hold ~1s) = enter WORD READOUT for the last reply (see below)
//   Main knob            = SCK frequency (fully CCW ~1 kHz ... fully CW ~100 kHz)
//   Knob X               = sample edge   (CCW = trailing/cpha1, CW = leading/cpha0)
//   Knob Y               = SCK polarity  (CCW = normal, CW = inverted)
//
// LED LAYOUT (LEDs 0..5)
//   In a TEST, LED5 is always the heartbeat. LEDs 0..4 mean what the test says below.
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// TEST 0 — IDLE / WIRING  (safe with the GBA connected)
//   Drives nothing. Just reports what the pins are doing at rest.
//   LED0 = MISO pad reads HIGH right now. With the GBA connected and idle this should be
//          SOLID (its SO line idles high, and our pull-up holds it there when disconnected).
//          If it is DARK, SO is being held low — a short to GND, or a dead/absent cable.
//   LED1 = MISO has CHANGED at least once since the test started (any activity at all).
//   LED5 = heartbeat.
//
// TEST 1 — TRANSPORT SELF-TEST  (jumper Pulse Out 2 -> Pulse In 1, GBA DISCONNECTED)
//   Sends 0x00006202 through the loopback jumper and checks it comes back bit-exact.
//   LED0 = exact match (GOOD).   LED1 = mismatch.
//   LED2 = stuck (all 0s or all 1s — nothing is coming back).
//   LED3 = bit-inverted reply (a polarity error).
//   LED5 = heartbeat.
//
// TEST 2 — LIVE GBA HANDSHAKE  (GBA connected, cartridge-less, on the logo screen)
//   Sends 0x00006202 repeatedly at the knob-selected rate/edge/polarity.
//   LED0 = ECHO: low-16 of the reply == 0x6202. THE KEY LIGHT — bit timing is correct.
//   LED1 = SYNC: high-16 == 0x7202. Full multiboot recognition. The goal.
//   LED2 = the reply is STRUCTURED (not all-0s / all-1s) — i.e. the GBA is driving SO.
//   LED3 = SO showed activity during the burst (sampled independently of the shift register).
//   LED4 = brightness shows the CURRENT SCK polarity (dim = normal, bright = inverted).
//   LED5 = heartbeat.
//   NOTE: LED0/LED1/LED2 LATCH once seen, so a single good reply cannot be missed while you
//         are looking away. A latched bit PULSES; a live-right-now bit is SOLID.
//
// TEST 3 — AUTO SWEEP  (GBA connected)
//   Ignores the knobs and walks every combination of {edge} x {polarity} x {5 rates},
//   ~0.7 s each, latching the best result found. Use this when you do not know where to
//   start; then read the winning config out with the WORD READOUT.
//   LED0 = ECHO seen at some point.  LED1 = SYNC seen.  LED2 = structured reply seen.
//   LED3/LED4 = 2-bit binary of the sweep slot that produced the best result so far.
//   LED5 = heartbeat (fast = sweeping).
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// WORD READOUT (hold switch UP ~1s from any test)
//   Displays the last 32-bit reply as 8 nibbles, most-significant first, one at a time:
//     LED0..3 = the nibble in binary (LED0 = bit 0 ... LED3 = bit 3)
//     LED4    = position marker: BRIGHT for nibbles 0-3 (high half), DIM for 4-7 (low half)
//     LED5    = flashes once between nibbles to separate them
//   Each nibble shows for ~1.2 s, then the whole word repeats. Click DOWN to leave.
//   This is the readout the six-LED encoding was missing: you can transcribe the actual
//   word the GBA sent back instead of inferring it from three flags.
//
// Reminder: power the COMPUTER first, then the GBA (the master must already be clocking
// when the console boots). Power-cycle the GBA, not the Computer, to retry.

#include "ComputerCard.h"
#include "gba_spi.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "gba_spi.pio.h"

class LinkCheck : public ComputerCard
{
public:
    void __not_in_flash_func(ProcessSample)() override
    {
        tick_++;
        readSwitch();

        if (readout_) { runReadout(); return; }

        switch (test_) {
        case 0: test0_idle();      break;
        case 1: test1_loopback();  break;
        case 2: test2_gba();       break;
        case 3: test3_sweep();     break;
        }
    }

private:
    // ── switch: edge-triggered so it never has to be held ────────────────────────────────
    void readSwitch()
    {
        Switch sw = SwitchVal();

        // Hold UP for ~1s to toggle the word readout.
        if (sw == Switch::Up) {
            if (++upHeld_ == 48000) { readout_ = !readout_; roIdx_ = 0; roTick_ = 0; }
        } else {
            upHeld_ = 0;
        }

        if (sw != lastSw_) {
            // Act on the transition INTO a position, then require a return to Middle before
            // the next one counts — that is what makes a momentary switch usable as a click.
            if (lastSw_ == Switch::Middle) {
                if (sw == Switch::Down) {
                    if (readout_) { readout_ = false; }
                    else { test_ = (test_ + 1) & 3; enterTest(); }
                } else if (sw == Switch::Up && !readout_) {
                    test_ = (test_ + 3) & 3; enterTest();
                }
            }
            lastSw_ = sw;
        }
    }

    void enterTest()
    {
        // Every test re-arms its own latches; the reply word deliberately survives so it can
        // still be read out after switching away from the test that produced it.
        echoLatch_ = syncLatch_ = structLatch_ = false;
        misoSeenHigh_ = misoSeenLow_ = false;
        cnt_ = 0; busy_ = false; sweepSlot_ = 0; sweepTick_ = 0; bestSlot_ = 0; bestScore_ = 0;

        if (test_ == 0) {
            // Idle test drives nothing: hand the pins back to plain SIO inputs so we can
            // watch what the far end is doing without contributing to it.
            if (pioLoaded_) { pio_sm_set_enabled(GBA_PIO, GBA_SM, false); pioLoaded_ = false; }
            gpio_init(GBA_MISO_PIN);
            gpio_set_dir(GBA_MISO_PIN, GPIO_IN);
            gpio_set_function(GBA_MISO_PIN, GPIO_FUNC_SIO);
            gpio_pull_up(GBA_MISO_PIN);          // Pulse In 1 transistor bias — dead without it
            gpio_set_inover(GBA_MISO_PIN, GBA_MISO_INOVER);
        } else {
            loadPio(wantLeadingEdge());
        }
    }

    // ── knobs ────────────────────────────────────────────────────────────────────────────
    // Knob range is 0..4095. Treat the outer thirds as the two choices so a roughly-centred
    // knob is never ambiguous.
    bool wantLeadingEdge() { return KnobVal(Knob::X) > 2048; }
    bool wantSckInvert()   { return KnobVal(Knob::Y) > 2048; }

    uint32_t knobRate()
    {
        // ~1 kHz .. ~100 kHz, geometric-ish so the low end (where the slow transistor input
        // is most likely to behave) gets most of the travel.
        uint32_t k = (uint32_t)KnobVal(Knob::Main);      // 0..4095
        return 1000u + (k * k) / 170u;                   // 1k .. ~99k
    }

    // ── PIO ──────────────────────────────────────────────────────────────────────────────
    void loadPio(bool leadingEdge)
    {
        if (pioLoaded_) { pio_sm_set_enabled(GBA_PIO, GBA_SM, false); pio_remove_program(GBA_PIO, prog_, off_); }
        prog_ = leadingEdge ? &gba_spi_cpha0_program : &gba_spi_program;
        off_  = pio_add_program(GBA_PIO, prog_);
        pio_sm_config c = leadingEdge ? gba_spi_cpha0_program_get_default_config(off_)
                                      : gba_spi_program_get_default_config(off_);
        sm_config_set_out_pins(&c, GBA_MOSI_PIN, 1);
        sm_config_set_in_pins(&c, GBA_MISO_PIN);
        sm_config_set_sideset_pins(&c, GBA_SCK_PIN);
        sm_config_set_out_shift(&c, false, true, 32);
        sm_config_set_in_shift(&c, false, true, 32);
        sm_config_set_clkdiv(&c, clkdiv(curRate_));

        pio_sm_set_pins_with_mask(GBA_PIO, GBA_SM, 0, (1u<<GBA_SCK_PIN)|(1u<<GBA_MOSI_PIN));
        pio_sm_set_pindirs_with_mask(GBA_PIO, GBA_SM,
            (1u<<GBA_SCK_PIN)|(1u<<GBA_MOSI_PIN),
            (1u<<GBA_SCK_PIN)|(1u<<GBA_MOSI_PIN)|(1u<<GBA_MISO_PIN));
        pio_gpio_init(GBA_PIO, GBA_SCK_PIN);
        pio_gpio_init(GBA_PIO, GBA_MOSI_PIN);
        pio_gpio_init(GBA_PIO, GBA_MISO_PIN);
        gpio_pull_up(GBA_MISO_PIN);                       // must follow pio_gpio_init
        applySck(sckInv_);
        gpio_set_outover(GBA_MOSI_PIN, GBA_MOSI_OUTOVER); // shared constants — gba_spi.h
        gpio_set_inover (GBA_MISO_PIN, GBA_MISO_INOVER);
        hw_set_bits(&GBA_PIO->input_sync_bypass, 1u << GBA_MISO_PIN);
        pio_sm_init(GBA_PIO, GBA_SM, off_, &c);
        pio_sm_set_enabled(GBA_PIO, GBA_SM, true);
        pioLoaded_ = true;
        leading_ = leadingEdge;
    }

    static float clkdiv(uint32_t hz)
    {
        float d = (float)clock_get_hz(clk_sys) / (GBA_PIO_CYCLES_PER_BIT * (float)hz);
        return d < 1.0f ? 1.0f : d;
    }

    void applySck(bool inv)
    {
        // GBA_SCK_OUTOVER is the measured resting value; the sweep needs to try its opposite,
        // so this is the one override the diagnostic is allowed to flip.
        gpio_set_outover(GBA_SCK_PIN, inv ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
        sckInv_ = inv;
    }

    // NON-BLOCKING transport. ProcessSample() has a 20.8 us budget and a 32-bit word takes
    // hundreds of us to milliseconds, so a blocking xfer here would stall the audio callback
    // and leave the SM half-fed. Post, return, collect later.
    void __not_in_flash_func(post)(uint32_t w)
    {
        if (pio_sm_is_tx_fifo_full(GBA_PIO, GBA_SM)) return;
        pio_sm_put(GBA_PIO, GBA_SM, w);
        busy_ = true;
        // Watch SO independently of the shift register while the burst is in flight: it
        // catches a GBA that is driving the line even when the sampled bits are garbage.
        misoSeenHigh_ = misoSeenLow_ = false;
    }

    bool __not_in_flash_func(poll)(uint32_t *out)
    {
        if (busy_) { if (gpio_get(GBA_MISO_PIN)) misoSeenHigh_ = true; else misoSeenLow_ = true; }
        if (!busy_ || pio_sm_is_rx_fifo_empty(GBA_PIO, GBA_SM)) return false;
        *out = pio_sm_get(GBA_PIO, GBA_SM);
        lastWord_ = *out;
        busy_ = false;
        return true;
    }

    // ── TEST 0: idle wiring check ────────────────────────────────────────────────────────
    void test0_idle()
    {
        bool hi = gpio_get(GBA_MISO_PIN);
        if (hi) misoSeenHigh_ = true; else misoSeenLow_ = true;
        LedOn(0, hi);
        LedOn(1, misoSeenHigh_ && misoSeenLow_);
        LedOff(2); LedOff(3);
        showTestId();
        heartbeat(9);
    }

    // ── TEST 1: loopback ─────────────────────────────────────────────────────────────────
    void test1_loopback()
    {
        retuneIfKnobsMoved();
        uint32_t got;
        if (!poll(&got)) {
            if (!busy_ && ++cnt_ >= 2000) { cnt_ = 0; post(0x00006202); }
            return;
        }
        const uint32_t sent = 0x00006202;
        LedOn(0, got == sent);
        LedOn(1, got != sent);
        LedOn(2, got == 0 || got == 0xFFFFFFFF);
        LedOn(3, got == ~sent);
        showTestId();
        heartbeat(9);
    }

    // ── TEST 2: live GBA ─────────────────────────────────────────────────────────────────
    void test2_gba()
    {
        retuneIfKnobsMoved();
        uint32_t r;
        if (!poll(&r)) {
            if (!busy_ && ++cnt_ >= 400) { cnt_ = 0; post(0x00006202); }
            // Keep the latched view alive between words so the LEDs do not flicker off.
            paintTest2(false, false, false);
            return;
        }
        bool echo = ((r & 0xFFFF) == 0x6202);
        bool sync = ((r >> 16) == 0x7202);
        bool structured = (r != 0 && r != 0xFFFFFFFF);
        if (echo) echoLatch_ = true;
        if (sync) syncLatch_ = true;
        if (structured) structLatch_ = true;
        paintTest2(echo, sync, structured);
    }

    // Live-now = solid; latched-earlier = pulsing. Lets one good reply be seen minutes later
    // without pretending it is still happening.
    void paintTest2(bool echo, bool sync, bool structured)
    {
        latched(0, echo, echoLatch_);
        latched(1, sync, syncLatch_);
        latched(2, structured, structLatch_);
        LedOn(3, misoSeenHigh_ && misoSeenLow_);
        LedBrightness(4, sckInv_ ? 4095 : 250);
        heartbeat(9);
    }

    // ── TEST 3: auto sweep ───────────────────────────────────────────────────────────────
    void test3_sweep()
    {
        static const uint32_t kRates[5] = { 1000, 5000, 16000, 50000, 100000 };

        if (++sweepTick_ >= 34000) {          // ~0.7 s per slot
            sweepTick_ = 0;
            sweepSlot_ = (sweepSlot_ + 1) % (2 * 2 * 5);
            uint32_t rate = kRates[sweepSlot_ % 5];
            bool inv      = ((sweepSlot_ / 5) & 1) != 0;
            bool leading  = ((sweepSlot_ / 10) & 1) != 0;
            curRate_ = rate;
            if (leading != leading_) { loadPio(leading); }
            else { pio_sm_set_clkdiv(GBA_PIO, GBA_SM, clkdiv(rate)); pio_sm_clkdiv_restart(GBA_PIO, GBA_SM); }
            applySck(inv);
            busy_ = false;
        }

        uint32_t r;
        if (!poll(&r)) {
            if (!busy_ && ++cnt_ >= 400) { cnt_ = 0; post(0x00006202); }
        } else {
            bool echo = ((r & 0xFFFF) == 0x6202);
            bool sync = ((r >> 16) == 0x7202);
            bool structured = (r != 0 && r != 0xFFFFFFFF);
            if (echo) echoLatch_ = true;
            if (sync) syncLatch_ = true;
            if (structured) structLatch_ = true;
            int score = (sync ? 4 : 0) + (echo ? 2 : 0) + (structured ? 1 : 0);
            if (score > bestScore_) { bestScore_ = score; bestSlot_ = sweepSlot_; }
        }

        latched(0, false, echoLatch_);
        latched(1, false, syncLatch_);
        latched(2, false, structLatch_);
        // Which slot won, as 2 bits — enough to say "leading vs trailing" and "inv vs normal".
        LedOn(3, (bestSlot_ / 5)  & 1);
        LedOn(4, (bestSlot_ / 10) & 1);
        heartbeat(7);
    }

    // ── WORD READOUT ─────────────────────────────────────────────────────────────────────
    void runReadout()
    {
        // ~1.2 s per nibble, with a short gap so two identical nibbles are distinguishable.
        if (++roTick_ >= 58000) { roTick_ = 0; roIdx_ = (roIdx_ + 1) & 7; }
        bool gap = (roTick_ > 50000);
        uint32_t nib = (lastWord_ >> (28 - 4 * roIdx_)) & 0xF;
        for (int i = 0; i < 4; i++) LedOn(i, !gap && ((nib >> i) & 1));
        LedBrightness(4, (roIdx_ < 4) ? 4095 : 250);
        LedOn(5, gap);
    }

    // ── LED helpers ──────────────────────────────────────────────────────────────────────
    void latched(uint32_t led, bool live, bool latch)
    {
        if (live)       LedBrightness(led, 4095);
        else if (latch) LedBrightness(led, ((tick_ >> 12) & 1) ? 1800 : 120);  // slow pulse
        else            LedOff(led);
    }

    // Test 0 and 1 have LED4 free, so use it to show which test you are in.
    void showTestId() { LedBrightness(4, test_ == 0 ? 200 : 1500); }

    void heartbeat(uint32_t shift) { LedOn(5, (tick_ >> shift) & 1); }

    void retuneIfKnobsMoved()
    {
        // Re-read the knobs periodically rather than every sample: changing the divider
        // mid-word would corrupt the transfer in flight.
        if (busy_ || (++knobTick_ & 0x3FF)) return;
        uint32_t r = knobRate();
        bool le = wantLeadingEdge();
        bool inv = wantSckInvert();
        if (le != leading_) { curRate_ = r; loadPio(le); }
        else if (r != curRate_) { curRate_ = r; pio_sm_set_clkdiv(GBA_PIO, GBA_SM, clkdiv(r)); pio_sm_clkdiv_restart(GBA_PIO, GBA_SM); }
        if (inv != sckInv_) applySck(inv);
    }

    // ── state ────────────────────────────────────────────────────────────────────────────
    const pio_program_t *prog_ = nullptr;
    uint off_ = 0;
    uint32_t tick_ = 0, cnt_ = 0, knobTick_ = 0, upHeld_ = 0;
    uint32_t sweepTick_ = 0, roTick_ = 0;
    uint32_t curRate_ = 25000;
    uint32_t lastWord_ = 0;
    int  test_ = 0, sweepSlot_ = 0, bestSlot_ = 0, bestScore_ = 0, roIdx_ = 0;
    bool pioLoaded_ = false, busy_ = false, leading_ = true, sckInv_ = false;
    bool echoLatch_ = false, syncLatch_ = false, structLatch_ = false;
    bool misoSeenHigh_ = false, misoSeenLow_ = false, readout_ = false;
    Switch lastSw_ = Switch::Middle;

public:
    LinkCheck() { enterTest(); }
};

int main()
{
    set_sys_clock_khz(144000, true);
    LinkCheck card;
    card.Run();
}
