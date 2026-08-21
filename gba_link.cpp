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
// line and only accept a well-formed reply. If several polls in a row fail the tag check,
// we assume the link dropped and fall back to re-running multiboot.
// ---------------------------------------------------------------------------

static constexpr uint16_t kReplyTag = 0x600D;   // "GOOD" — GBA payload stamps this in the high half

static inline uint32_t pack_params()
{
    return ((uint32_t)gGba.params[0] << 24) |
           ((uint32_t)gGba.params[1] << 16) |
           ((uint32_t)gGba.params[2] <<  8) |
           ((uint32_t)gGba.params[3]);
}

// SPI clock rates. Multiboot is deliberately slow to stay well within the Pulse In 1
// transistor input's bandwidth; the polling loop can try a little faster but stays modest.
static constexpr uint32_t kMultibootHz = 100'000;   // 100 kHz — matches slow reference uploaders
static constexpr uint32_t kPollHz      = 100'000;   // keep equal for v0; raise once proven

void gba_link_core1(const uint8_t *payload, uint32_t payload_size)
{
    gba_spi_init(kMultibootHz);

    for (;;) {
        // ---- (re)connect: run multiboot until it succeeds ----
        gGba.state = LinkState::Connecting;
        gba_spi_set_clock(kMultibootHz);

        MultibootResult r = gba_multiboot_send(payload, payload_size);
        if (r != MultibootResult::Ok) {
            gGba.lastError = (uint8_t)r;
            // NoGBA is the normal "not plugged in / not ready yet" case: just retry.
            // Other errors also retry, but flag Error briefly so the UI can show it.
            gGba.state = (r == MultibootResult::NoGBA) ? LinkState::Connecting
                                                       : LinkState::Error;
            sleep_ms(250);
            continue;
        }

        // ---- booted: give the payload a moment to start its serial slave ----
        gGba.state = LinkState::Booted;
        gba_spi_set_clock(kPollHz);
        sleep_ms(50);

        // ---- polling loop ----
        int consecutiveBad = 0;
        for (;;) {
            uint32_t reply = gba_spi_xfer32(pack_params());

            if ((reply >> 16) == kReplyTag) {
                gGba.buttons = (uint16_t)(reply & 0xFFFF);
                gGba.rxSeq++;
                consecutiveBad = 0;
            } else if (++consecutiveBad > 32) {
                // Link looks dead — drop back to reconnect.
                break;
            }

            // ~1 kHz poll: plenty for a UI, easy on the slow input line.
            sleep_us(1000);
        }
    }
}
