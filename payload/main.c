// payload/main.c — Minimal GBA-side multiboot payload (v0 bidirectional smoke test).
//
// Runs from EWRAM after the RP2040 uploads it via BIOS Multiboot. It:
//   1. Puts the LCD in mode 3 (240x160 16bpp bitmap) and fills the screen, proving we booted.
//   2. Acts as a serial NORMAL-mode 32-bit SLAVE (external clock from the RP2040):
//        - preloads SIODATA32 with  (0x600D << 16) | buttonBits  before each transfer,
//        - after each host-clocked word, reads the 4 param bytes the host sent and shows them.
//   3. Redraws a simple UI: background tint from param[0], four bars from the params, and
//      button state, so both link directions are visibly working.
//
// Deliberately tiny and dependency-free (no libgba) so it builds with a bare arm-none-eabi
// toolchain targeting armv4t. See build.sh.

#include <stdint.h>

// ---- GBA memory-mapped I/O ----
#define REG_BASE        0x04000000
#define REG_DISPCNT     (*(volatile uint16_t*)(REG_BASE + 0x0000))
#define REG_KEYINPUT    (*(volatile uint16_t*)(REG_BASE + 0x0130))
#define REG_SIODATA32   (*(volatile uint32_t*)(REG_BASE + 0x0120))
#define REG_SIOCNT      (*(volatile uint16_t*)(REG_BASE + 0x0128))
#define REG_RCNT        (*(volatile uint16_t*)(REG_BASE + 0x0134))

#define VRAM            ((volatile uint16_t*)0x06000000)
#define SCREEN_W        240
#define SCREEN_H        160

// DISPCNT: mode 3 + enable BG2 (the bitmap layer).
#define MODE3           0x0003
#define BG2_ON          0x0400

// SIOCNT bits (normal mode):
//   bit0  : internal clock select (0 = external/slave)  -> we are the slave, so 0
//   bit2  : SI state (read-only)
//   bit3  : SO during inactivity (read-only-ish)
//   bit7  : start/active (master sets 1; for a slave, hardware clears it when a word arrives)
//   bit12 : transfer length (0 = 8-bit, 1 = 32-bit)  -> 32-bit
//   bit14 : IRQ enable (unused here; we poll)
#define SIO_32BIT       (1 << 12)
#define SIO_START       (1 << 7)
#define SIO_SLAVE       (0 << 0)      // external clock

// RCNT: 0x0000 selects normal serial mode (not GPIO/JOYBUS).
#define RCNT_SERIAL     0x0000

static inline uint16_t rgb15(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)((r & 31) | ((g & 31) << 5) | ((b & 31) << 10));
}

static void fill(uint16_t color)
{
    volatile uint16_t *p = VRAM;
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) p[i] = color;
}

// Draw a filled rectangle (clipped-free; caller keeps it in bounds).
static void rect(int x, int y, int w, int h, uint16_t color)
{
    for (int j = 0; j < h; j++) {
        volatile uint16_t *row = VRAM + (y + j) * SCREEN_W + x;
        for (int i = 0; i < w; i++) row[i] = color;
    }
}

// Read buttons. REG_KEYINPUT is active-LOW (0 = pressed); invert to active-high and mask
// to the 10 valid key bits, matching the host's GbaKey layout.
static uint16_t read_buttons(void)
{
    return (uint16_t)(~REG_KEYINPUT) & 0x03FF;
}

int main(void)
{
    // ---- LCD: mode 3 bitmap ----
    REG_DISPCNT = MODE3 | BG2_ON;

    // ---- Serial: normal mode, 32-bit, slave (external clock) ----
    REG_RCNT  = RCNT_SERIAL;
    REG_SIOCNT = SIO_32BIT | SIO_SLAVE;

    uint32_t params = 0;   // last 4 bytes received from the host (p0<<24|p1<<16|p2<<8|p3)

    for (;;) {
        uint16_t buttons = read_buttons();

        // Preload our outgoing word for the next host-clocked transfer.
        REG_SIODATA32 = (0x600Du << 16) | buttons;

        // Arm the slave: set the active bit; the GBA hardware clears it once the master
        // has clocked a full 32-bit word in/out.
        REG_SIOCNT |= SIO_START;

        // Poll for the transfer to complete, but don't hang forever if the host is idle —
        // fall through and redraw so the screen stays live (and buttons keep updating).
        int spins = 0;
        while (REG_SIOCNT & SIO_START) {
            if (++spins > 200000) break;   // ~ a few ms; host poll is ~1 kHz
        }
        if (!(REG_SIOCNT & SIO_START)) {
            params = REG_SIODATA32;        // 4 param bytes the host just sent
        }

        // ---- redraw: prove both directions ----
        uint8_t p0 = (params >> 24) & 0xFF;
        uint8_t p1 = (params >> 16) & 0xFF;
        uint8_t p2 = (params >>  8) & 0xFF;
        uint8_t p3 = (params >>  0) & 0xFF;

        // Background tint driven by p0 (host Knob Main) — visibly changes as you turn it.
        fill(rgb15(p0 >> 3, 8, 16));

        // Four vertical bars for the four params.
        const uint8_t vals[4] = { p0, p1, p2, p3 };
        for (int k = 0; k < 4; k++) {
            int h = 1 + (vals[k] * (SCREEN_H - 20)) / 255;
            rect(20 + k * 50, SCREEN_H - 10 - h, 30, h, rgb15(31, 31, 31));
        }

        // Button indicators along the top: lit white when pressed.
        for (int b = 0; b < 10; b++) {
            uint16_t c = (buttons & (1 << b)) ? rgb15(31, 31, 0) : rgb15(4, 4, 4);
            rect(6 + b * 22, 6, 18, 10, c);
        }
    }
}
