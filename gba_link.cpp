// gba_link.cpp — Core-1 GBA link engine: multiboot, then the post-boot streaming loop.

#include "gba_link.h"
#include "gba_spi.h"
#include "gba_multiboot.h"
#include "gba_proto.h"

#include "pico/stdlib.h"

GbaShared gGba;

// ---------------------------------------------------------------------------
// Post-boot loop
//
// One 32-bit exchange per poll, mode 3 SPI, RP2040 still the clock master. Downstream is
// almost always a STREAM word carrying the gate plus one of the two input pairs; upstream is
// whatever the GBA had queued, identified by its 16-bit tag. See gba_proto.h.
//
// The tag check is what lets the host reject noise and half-synced words from the slow Pulse
// In 1 line. The tolerance for failures has to be generous: the GBA slave can only have one
// transfer pending and is briefly deaf between finishing one word and re-arming, so a short
// run of tag misses is NORMAL, not a dropped link. An early threshold of 32 at 1 kHz meant
// 32 ms of silence tore the link down and restarted multiboot, which is why it used to connect
// and immediately drop.
// ---------------------------------------------------------------------------

// ~5 s of unbroken silence at the 1 kHz poll rate before declaring the link dead.
static constexpr int kMaxConsecutiveBad = 5000;

// Multiboot rate LADDER, fastest first.
//
// 100 kHz is the measured, repeatable ceiling (mbrate.uf2, 2026-09-07): it succeeded on every
// power-cycle. 200 kHz was MARGINAL — it sometimes completed and often failed partway, leaving
// the console restarting its boot over and over, which is a far worse experience than simply
// running slower. So the ladder deliberately starts at 100 kHz and never attempts above it.
//
// This supersedes an earlier note claiming multiboot ran at 200 kHz. That figure came from an
// automatic ladder that kept the first rate to succeed, which meant it reported whichever rung
// it happened to be on when the console finished booting — timing luck, not a measurement.
// Manual per-speed testing with repeats is what settled it.
static constexpr uint32_t kMultibootLadder[] = { 100'000, 50'000, 25'000, 10'000, 5'000 };
static constexpr int kLadderLen = (int)(sizeof(kMultibootLadder) / sizeof(kMultibootLadder[0]));

// THE GAP IS THE POINT, and it applies to EVERY word we send — control words included.
//
// The GBA slave holds ONE pending transfer and re-arms with a read-modify-write of SIOCNT, so
// a word clocked before it has re-armed is not merely lost, it is CORRUPTED. Measured clean at
// 2000 words/s over 25 passes (linkrate.uf2); 1 kHz sits at half that with 1 ms of latency.
// At 100 kHz SCK a 32-bit word occupies 320 us, leaving ~680 us of slack.
//
// The rock-steady "dropped 2, corrupt 4" that survived a 20x rate range was one out-of-band
// word sent without this gap, destroying the word behind it. Do not special-case anything.
static inline void link_gap() { sleep_us(1000); }

void gba_link_core1(const uint8_t *payload, uint32_t payload_size)
{
    gba_spi_init(kMultibootLadder[0]);

    int ladderIx = 0;          // index of the rate to try first; sticks once one works

    for (;;) {
        // ---- (re)connect: walk the rate ladder until multiboot succeeds ----
        gGba.state = LinkState::Connecting;

        MultibootResult r = MultibootResult::NoGBA;
        int tried = 0;
        for (; tried < kLadderLen; tried++) {
            int ix = (ladderIx + tried) % kLadderLen;
            uint32_t hz = kMultibootLadder[ix];
            gGba.linkHz = hz;                      // published so the UI can show the rate
            gba_spi_set_clock(hz);

            r = gba_multiboot_send(payload, payload_size);
            if (r == MultibootResult::Ok) { ladderIx = ix; break; }

            gGba.lastError = (uint8_t)r;
            // NoGBA just means nothing answered at this rate — drop to the next one. Any
            // other error means we DID get a conversation and it broke partway, which is
            // much more interesting, so surface it.
            if (r != MultibootResult::NoGBA) gGba.state = LinkState::Error;
        }

        if (r != MultibootResult::Ok) {
            gGba.state = (gGba.lastError == (uint8_t)MultibootResult::NoGBA)
                       ? LinkState::Connecting : LinkState::Error;
            sleep_ms(250);
            continue;
        }

        // ---- booted: give the payload a moment to start its serial slave ----
        gGba.state = LinkState::Booted;
        gba_spi_set_clock(kMultibootLadder[ladderIx]);   // poll at the proven rate, not faster
        sleep_ms(50);

        // Introduce ourselves. Repeated because a single missed word would leave the GBA's CAL
        // page reporting the wrong calibration state for ever, and each one takes its gap like
        // any other word.
        for (int i = 0; i < 4; i++) {
            gba_spi_xfer32(gba_hello_pack(gGba.caps));
            link_gap();
        }

        // ---- streaming loop ----
        int      consecutiveBad = 0;
        int      pair           = 0;    // alternates {CV1,CV2} and {AUD1,AUD2}
        uint32_t reportedEdges  = gGba.gateEdges;
        uint32_t seq            = 0;

        for (;;) {
            uint32_t word;

            // Slow housekeeping, interleaved one word at a time so it never displaces more
            // than a single input update: knobs and switch round-robin, then a HELLO refresh.
            uint32_t slot = seq & 0x3Fu;
            if (slot == 0x10u) {
                word = gba_knob_pack(0, gGba.knobs[0]);
            } else if (slot == 0x20u) {
                word = gba_knob_pack(1, gGba.knobs[1]);
            } else if (slot == 0x30u) {
                word = gba_knob_pack(2, gGba.knobs[2]);
            } else if (slot == 0x38u) {
                word = gba_control_pack(GBA_OP_SWITCH, gGba.switchPos);
            } else if ((seq & 0x7FFu) == 0x7FFu) {
                word = gba_hello_pack(gGba.caps);
            } else {
                // The hot path. Report exactly one gate edge per increment: advancing by one
                // rather than jumping to the current count means a burst of triggers arrives
                // in order instead of being collapsed into one.
                uint32_t edges = gGba.gateEdges;
                int      edge  = (edges != reportedEdges);

                word = gba_stream_pack(pair, gGba.gate, edge,
                                       gGba.inputs[pair ? GBA_IN_AUD1 : GBA_IN_CV1],
                                       gGba.inputs[pair ? GBA_IN_AUD2 : GBA_IN_CV2]);
                if (edge) reportedEdges++;
                pair ^= 1;
            }
            seq++;

            uint32_t reply = gba_spi_xfer32(word);

            switch (GBA_UP_TAG(reply)) {
            case GBA_UP_BUTTONS:
                gGba.buttons = (uint16_t)GBA_UP_DATA(reply);
                break;
            case GBA_UP_NOTE:
                gGba.note    = (uint8_t)(GBA_UP_DATA(reply) >> 8);
                gGba.noteVel = (uint8_t)(GBA_UP_DATA(reply) & 0xFF);
                break;
            case GBA_UP_STATUS:
                gGba.page  = (uint8_t)((GBA_UP_DATA(reply) >> 12) & 0xF);
                gGba.mode  = (uint8_t)((GBA_UP_DATA(reply) >>  8) & 0xF);
                gGba.flags = (uint8_t)(GBA_UP_DATA(reply) & 0xFF);
                break;
            case GBA_UP_PARAM:
                break;                          // reserved: patch echo, unused in v1
            default:
                gGba.rxBad++;
                if (++consecutiveBad > kMaxConsecutiveBad) goto dropped;
                link_gap();
                continue;
            }

            gGba.rxSeq++;
            consecutiveBad = 0;
            link_gap();
        }

    dropped:
        ;   // fall out to the reconnect loop
    }
}
