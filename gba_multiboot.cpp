// gba_multiboot.cpp — PIO SPI transport + GBA Multiboot uploader.
//
// See gba_multiboot.h / gba_spi.h for the interface and gba_spi.pio for the SPI core.

#include "gba_multiboot.h"
#include "gba_spi.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "gba_spi.pio.h"   // generated from gba_spi.pio by pico_generate_pio_header()

// ============================================================================
// Low-level PIO SPI32 transport (mode 3, MSB-first, 32-bit), on GPIO 8/9/2.
// ============================================================================

static uint gba_pio_offset = 0;

static float clkdiv_for(uint32_t sck_hz)
{
    // The SM runs 3 instructions per SPI bit (see gba_spi.pio), so one SCK period is
    // 3 SM cycles. div = f_sys / (3 * sck_hz). Clamp to the PIO minimum of 1.0.
    float div = (float)clock_get_hz(clk_sys) / (3.0f * (float)sck_hz);
    if (div < 1.0f) div = 1.0f;
    return div;
}

void gba_spi_set_clock(uint32_t sck_hz)
{
    pio_sm_set_clkdiv(GBA_PIO, GBA_SM, clkdiv_for(sck_hz));
    pio_sm_clkdiv_restart(GBA_PIO, GBA_SM);
}

void gba_spi_init(uint32_t sck_hz)
{
    PIO pio = GBA_PIO;
    uint sm = GBA_SM;

    gba_pio_offset = pio_add_program(pio, &gba_spi_program);

    pio_sm_config c = gba_spi_program_get_default_config(gba_pio_offset);
    sm_config_set_out_pins(&c, GBA_MOSI_PIN, 1);
    sm_config_set_in_pins(&c, GBA_MISO_PIN);
    sm_config_set_sideset_pins(&c, GBA_SCK_PIN);
    // MSB-first: shift LEFT, autopull + autopush at 32-bit thresholds.
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_clkdiv(&c, clkdiv_for(sck_hz));

    // MOSI + SCK start low as outputs; MISO is an input.
    pio_sm_set_pins_with_mask(pio, sm, 0, (1u << GBA_SCK_PIN) | (1u << GBA_MOSI_PIN));
    pio_sm_set_pindirs_with_mask(pio, sm,
        (1u << GBA_SCK_PIN) | (1u << GBA_MOSI_PIN),
        (1u << GBA_SCK_PIN) | (1u << GBA_MOSI_PIN) | (1u << GBA_MISO_PIN));

    pio_gpio_init(pio, GBA_SCK_PIN);
    pio_gpio_init(pio, GBA_MOSI_PIN);
    pio_gpio_init(pio, GBA_MISO_PIN);

    // Net pad inversions — see gba_spi.pio for the derivation:
    //   SCK : CPOL=1 invert cancels the Workshop output invert  -> NORMAL
    //   MOSI: Workshop output invert                            -> INVERT
    //   MISO: Workshop input invert                             -> INVERT
    gpio_set_outover(GBA_SCK_PIN,  GPIO_OVERRIDE_NORMAL);
    gpio_set_outover(GBA_MOSI_PIN, GPIO_OVERRIDE_INVERT);
    gpio_set_inover (GBA_MISO_PIN, GPIO_OVERRIDE_INVERT);

    // MISO is a genuinely synchronous SPI input; bypass the input synchroniser to cut
    // ~2 cycles of read latency (matters as the clock rate rises). SDK 2.2.0 has no helper
    // for this, so poke the register directly (same as the cyw43 PIO SPI driver).
    hw_set_bits(&pio->input_sync_bypass, 1u << GBA_MISO_PIN);

    pio_sm_init(pio, sm, gba_pio_offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}

uint32_t __not_in_flash_func(gba_spi_xfer32)(uint32_t w)
{
    // Blocking full-duplex 32-bit exchange. TX FIFO is 32-bit; data is left-justified
    // already (we always push full 32-bit words), so no pre-shift needed.
    pio_sm_put_blocking(GBA_PIO, GBA_SM, w);
    return pio_sm_get_blocking(GBA_PIO, GBA_SM);
}

// ============================================================================
// Multiboot protocol
// ============================================================================

// Small inter-word settle. The GBA BIOS needs a gap between words to process each one;
// reference uploaders use 3–10 ms. Generous here because our Pulse In 1 is slow and this
// phase is one-shot, not a hot path.
static inline void mb_delay() { sleep_us(300); }

static inline uint32_t xfer(uint32_t w)
{
    uint32_t r = gba_spi_xfer32(w);
    mb_delay();
    return r;
}

MultibootResult gba_multiboot_send(const uint8_t *rom, size_t rom_size)
{
    if (!rom || rom_size < 0xC0) return MultibootResult::BadPayload;

    // Round the payload up to a 16-byte boundary, matching the reference uploaders.
    uint32_t fsize = ((uint32_t)rom_size + 0x0F) & ~0x0Fu;
    if (fsize > 0x40000) return MultibootResult::BadPayload; // EWRAM is 256 kB

    const uint16_t *rom16 = reinterpret_cast<const uint16_t *>(rom);
    const uint32_t *rom32 = reinterpret_cast<const uint32_t *>(rom);

    // ---- 1. Sync: wait for the recognition value 0x7202xxxx --------------------
    // Bounded so a disconnected GBA returns NoGBA promptly (caller retries).
    {
        const int kMaxSyncTries = 512;   // ~ up to a few hundred ms with mb_delay
        uint32_t r = 0;
        int tries = 0;
        do {
            r = xfer(0x00006202);
            if (++tries > kMaxSyncTries) return MultibootResult::NoGBA;
        } while ((r >> 16) != 0x7202);
    }

    xfer(0x00006202);   // acknowledge / exchange master-slave info
    xfer(0x00006102);   // recognition OK; header transfer follows

    // ---- 2. Header: first 0xC0 bytes as 16-bit words (0x60 words) ---------------
    for (uint32_t i = 0; i < 0xC0; i += 2) {
        xfer(rom16[i / 2]);
    }

    xfer(0x00006200);   // header transfer complete
    xfer(0x00006202);   // exchange master/slave info again

    // ---- 3. Palette / handshake: derive encryption seed + crc seeds -------------
    // Palette byte 0xD1 (matches references). Send twice; the 2nd reply carries client
    // data in the form 0x73hh****.
    xfer(0x000063D1);
    uint32_t token = xfer(0x000063D1);
    if ((token >> 24) != 0x73) return MultibootResult::BadHandshake;

    uint32_t crcA = (token >> 16) & 0xFF;
    uint32_t seed = 0xFFFF00D1u | (crcA << 8);   // encryption LCG seed (m)
    crcA = (crcA + 0x0F) & 0xFF;

    xfer(0x00006400 | crcA);                      // handshake data

    // ---- 4. Length word; receive the GBA's random seed --------------------------
    token = xfer((fsize - 0x190) / 4);
    uint32_t crcB = (token >> 16) & 0xFF;
    uint32_t crcC = 0x0000C387;                   // running CRC accumulator

    // ---- 5. Encrypted main transfer (from offset 0xC0 to fsize) -----------------
    for (uint32_t i = 0xC0; i < fsize; i += 4) {
        uint32_t dat = rom32[i / 4];

        // CRC step over the *plaintext* word.
        uint32_t tmp = dat;
        for (int b = 0; b < 32; b++) {
            uint32_t bit = (crcC ^ tmp) & 1u;
            crcC = (crcC >> 1) ^ (bit ? 0xC37Bu : 0u);
            tmp >>= 1;
        }

        // Encrypt: LCG advance, then XOR the fixed transform.
        seed = seed * 0x6F646573u + 1u;
        uint32_t enc = seed ^ dat ^ (0xFE000000u - i) ^ 0x43202F2Fu;

        // Each data word echoes back the *previous* offset in its high half; the GBA
        // reports the current offset & 0xFFFF. A mismatch means the link corrupted.
        uint32_t chk = xfer(enc) >> 16;
        if (chk != (i & 0xFFFF)) return MultibootResult::TransferError;
    }

    // ---- 6. Final CRC handshake -------------------------------------------------
    // Finalise the CRC with the magic 0xFFFF0000 | crcB<<8 | crcA word.
    uint32_t fin = 0xFFFF0000u | (crcB << 8) | crcA;
    for (int b = 0; b < 32; b++) {
        uint32_t bit = (crcC ^ fin) & 1u;
        crcC = (crcC >> 1) ^ (bit ? 0xC37Bu : 0u);
        fin >>= 1;
    }

    // Wait for the GBA to signal it has computed its CRC (0x0075 in the high half).
    {
        const int kMaxCrcTries = 256;
        int tries = 0;
        uint32_t r;
        xfer(0x00000065);
        do {
            r = xfer(0x00000065) >> 16;
            if (++tries > kMaxCrcTries) return MultibootResult::CrcMismatch;
        } while (r != 0x0075);
    }

    xfer(0x00000066);                              // ready to exchange CRC
    uint32_t crcGBA = xfer(crcC & 0xFFFF) >> 16;   // send ours, receive theirs

    return (crcGBA == (crcC & 0xFFFF)) ? MultibootResult::Ok
                                       : MultibootResult::CrcMismatch;
}
