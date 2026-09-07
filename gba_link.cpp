// gba_link.cpp — Core-1 GBA link engine: multiboot then the post-boot polling loop.

#include "gba_link.h"
#include "gba_spi.h"
#include "gba_multiboot.h"

#include "pico/stdlib.h"

GbaShared gGba;

// ---------------------------------------------------------------------------
// Post-boot packet format (v0 bidirectional smoke test)
//
// One 32-bit exchange per poll, mode 3 SPI, RP2040 still the clock master.
//   RP2040 -> GBA (MOSI word):  [31:24]=0xC0 tag  [23:16]=params[0] ... actually 4 bytes:
//        we pack all four params into the 32-bit word: p0<<24 | p1<<16 | p2<<8 | p3.
//   GBA   -> RP2040 (MISO word): [31:16]=0x600D framing tag  [15:0]=button bitfield.
//
// The framing tag lets the host reject noise / half-synced words from the slow Pulse In 1
// line and only accept a well-formed reply. If MANY polls in a row fail the tag check, we
// assume the link dropped and fall back to re-running multiboot.
//
// The tolerance has to be generous. The GBA slave can only have one transfer pending at a
// time, and it is briefly deaf between finishing one word and re-arming for the next — plus
// whatever time it spends drawing. A short run of tag misses is NORMAL, not a dropped link.
// The original threshold of 32 at a 1 kHz poll rate meant roughly 32 ms of silence tore the
// link down and restarted multiboot, which is why it connected and immediately dropped.
// ---------------------------------------------------------------------------

static constexpr uint16_t kReplyTag = 0x600D;   // "GOOD" — GBA payload stamps this in the high half

// ~5 s of unbroken silence at the 1 kHz poll rate before declaring the link dead.
static constexpr int kMaxConsecutiveBad = 5000;

static inline uint32_t pack_params()
{
    return ((uint32_t)gGba.params[0] << 24) |
           ((uint32_t)gGba.params[1] << 16) |
           ((uint32_t)gGba.params[2] <<  8) |
           ((uint32_t)gGba.params[3]);
}

// SPI clock rates. Multiboot is deliberately slow to stay well within the Pulse In 1
// transistor input's bandwidth; the polling loop can try a little faster but stays modest.
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

// The post-boot poll runs at the rate that actually worked for multiboot, never faster: the
// payload's serial slave is on the same wire with the same bandwidth limit.

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

        // ---- polling loop ----
        int consecutiveBad = 0;
        for (;;) {
            uint32_t reply = gba_spi_xfer32(pack_params());

            if ((reply >> 16) == kReplyTag) {
                gGba.buttons = (uint16_t)(reply & 0xFFFF);
                gGba.rxSeq++;
                consecutiveBad = 0;
            } else if (++consecutiveBad > kMaxConsecutiveBad) {
                // Link looks genuinely dead — drop back to reconnect.
                break;
            }

            // 1 kHz poll. Measured clean at 2000 words/s over 25 passes (linkrate.uf2), so
            // this sits at half the proven rate — comfortable margin, and 1 ms of latency
            // instead of 5 ms.
            //
            // The gap is what matters, not the clock: the GBA slave holds ONE pending transfer
            // and re-arms with a read-modify-write, so a poll that lands before it has re-armed
            // is not merely wasted, it corrupts the word. At 100 kHz SCK a 32-bit word occupies
            // 320 us, so this leaves ~680 us of slack. Do not remove the gap.
            sleep_us(1000);
        }
    }
}
