// linkrate.cpp — How fast can the POST-BOOT link run, and how reliably?
//
// Boots the GBA at 100 kHz, the rate mbrate.uf2 proved repeatable, then lets you step the
// LIVE link speed by hand while the GBA reports the result on its own screen.
//
// This replaces bandwidth.uf2's automatic sweep, which never produced a usable number. The
// sweep tried to decide "clean or not" itself, so a fault anywhere in the harness came back
// as a bare zero that could not be told from a genuine ceiling of zero. Stepping one rate at
// a time and reading a live word count is slower to run and far harder to fool.
//
// Multiboot rate is FIXED at 100 kHz on purpose: it is the one number already established by
// repeated measurement, so a failure here always means the live link, never the upload.
//
// ─────────────────────────────────────────────────────────────────────────────────────────
// WHAT THE GBA SHOWS
//     SCK      the rate the host is currently driving, in kHz
//     WORDS    sequence-numbered words received at this rate
//     ERRORS   gaps in that sequence — dropped or corrupted words
//     verdict  MEASURING until 500 words, then CLEAN or ERRORS
// Changing rate resets the counts, so the figures always describe the rate beside them.
//
// WORKSHOP LEDS — exactly one lit, showing the rate slot:
//     LED0  50 kHz     LED1 100 kHz    LED2 200 kHz
//     LED3 400 kHz     LED4 600 kHz    LED5   1 MHz
// It is SOLID while the upstream tag is intact and BLINKS when the host is seeing bad
// replies, so both directions are visible without touching the GBA.
//
// CONTROLS
//     DOWN click = next rate up      UP click = previous rate down
//
// HOW TO USE
//     Let it boot (100 kHz, ~1 s). Step up one rate at a time. At each, wait for the GBA to
//     leave MEASURING and read the verdict. The fastest rate that stays CLEAN over several
//     thousand words is the answer.
//
// PROTOCOL
//     Host -> GBA   0xA5 <seq:24>      sequence counter, +1 per word; gaps are the error metric
//                   0xA6 <rateKHz:16>  current rate, sent ~1/s so the GBA can display it
//     GBA  -> host  0x600D <errors:16> the GBA's running error count

#include "ComputerCard.h"
#include "gba_spi.h"
#include "gba_multiboot.h"
#include "gba_payload.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"

static constexpr uint32_t kMultibootHz = 100'000;   // proven by mbrate.uf2; do not raise
static const uint32_t kRates[6] = { 50'000, 100'000, 200'000, 400'000, 600'000, 1'000'000 };

struct Shared {
    volatile uint8_t  slot     = 1;   // start at 100 kHz, the known-good rate
    volatile bool     linkUp   = false;
    volatile bool     upstreamOk = false;
    volatile uint32_t gbaErrors = 0;  // as reported back by the GBA
    volatile uint32_t sent      = 0;
};
static Shared gS;

static void core1_entry()
{
    gba_spi_init(kMultibootHz);

    for (;;) {
        gS.linkUp = false;
        gba_spi_set_clock(kMultibootHz);
        if (gba_multiboot_send(gba_payload, gba_payload_size) != MultibootResult::Ok) {
            sleep_ms(200);
            continue;
        }
        gS.linkUp = true;
        sleep_ms(150);                       // let the payload reach its loop

        uint8_t  lastSlot = 0xFF;
        uint32_t seq = 0, sinceRate = 0, badRun = 0;

        for (;;) {
            uint8_t s = gS.slot;
            if (s != lastSlot) {
                lastSlot = s;
                gba_spi_set_clock(kRates[s]);
                sinceRate = 0;
                // Announce the new rate immediately and repeatedly: the GBA resets its counts
                // when the rate changes, and until it hears about it the figures on screen
                // would describe a blend of two rates.
                for (int i = 0; i < 8; i++) {
                    gba_spi_xfer32(0xA6000000u | (kRates[s] / 1000u));
                    sleep_us(300);
                }
            }

            uint32_t r = gba_spi_xfer32(0xA5000000u | (seq & 0x00FFFFFFu));
            seq++;
            gS.sent = seq;

            if ((r >> 16) == 0x600D) {
                gS.upstreamOk = true;
                gS.gbaErrors  = r & 0xFFFF;
                badRun = 0;
            } else {
                gS.upstreamOk = false;
                // Only give up after a long run: at the highest rates an occasional bad reply
                // is the very thing being measured, not a reason to tear the link down.
                if (++badRun > 4000) break;
            }

            // Re-announce the rate about once a second so a missed announcement cannot leave
            // the display permanently wrong.
            if (++sinceRate > 2000) {
                sinceRate = 0;
                gba_spi_xfer32(0xA6000000u | (kRates[s] / 1000u));
            }
        }
    }
}

class LinkRate : public ComputerCard
{
public:
    LinkRate() { multicore_launch_core1(core1_entry); }

    void __not_in_flash_func(ProcessSample)() override
    {
        tick_++;
        Switch sw = SwitchVal();
        if (sw != lastSw_) {
            if (lastSw_ == Switch::Middle) {
                if      (sw == Switch::Down) gS.slot = (uint8_t)((gS.slot + 1) % 6);
                else if (sw == Switch::Up)   gS.slot = (uint8_t)((gS.slot + 5) % 6);
            }
            lastSw_ = sw;
        }

        for (int i = 0; i < 6; i++) LedOff(i);
        if (!gS.linkUp) {                       // still uploading: sweep so it is obviously busy
            LedOn((tick_ >> 13) % 6, true);
            return;
        }
        // Solid = upstream replies are framed correctly; blinking = the host is seeing bad
        // words. The GBA screen reports the downstream direction, so between the two every
        // direction is covered.
        LedOn(gS.slot, gS.upstreamOk ? true : ((tick_ >> 12) & 1));
    }

private:
    uint32_t tick_ = 0;
    Switch lastSw_ = Switch::Middle;
};

int main()
{
    set_sys_clock_khz(144000, true);
    LinkRate card;
    card.Run();
}
