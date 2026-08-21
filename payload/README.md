# GBA payload (89_GBA)

The GBA-side program uploaded into a cartridge-less Game Boy Advance via BIOS Multiboot.
It's deliberately tiny and dependency-free (no libgba) so it builds with a bare
`arm-none-eabi` toolchain targeting the GBA's ARM7TDMI (armv4t).

## What it does (v0)

- Puts the LCD in mode 3 (240×160 bitmap) and draws a live UI — proving it booted.
- Acts as a **normal-mode 32-bit serial slave** (external clock from the RP2040):
  each poll it preloads `SIODATA32` with `(0x600D << 16) | buttons` and, after the host
  clocks the word, reads the 4 parameter bytes the host sent.
- Draws four bars + a background tint from the host parameters and highlights pressed
  buttons, so both link directions are visibly working.

## Files

| File            | Purpose |
|-----------------|---------|
| `main.c`        | The payload (LCD + serial slave + UI) |
| `crt0.s`        | GBA ROM header + multiboot entry (`_start` sits at offset `0xC0`) |
| `multiboot.ld`  | Links the image for EWRAM (`0x02000000`) |
| `build.sh`      | Compile → link → `.mb` → insert logo/checksum → `../gba_payload.h` |
| `bin2h.py`      | Converts `payload.mb` to the C byte array |

## Building

```sh
cd payload
./build.sh
```

This overwrites `../gba_payload.h` with the compiled payload, then rebuild the RP2040
firmware to bake it in.

### Toolchain

`build.sh` needs two things:

1. **A compiler** — either devkitARM, or any `arm-none-eabi-gcc` on `PATH`. The Raspberry
   Pi Pico SDK already ships one (`~/.pico-sdk/toolchain/*/bin/arm-none-eabi-gcc`) which
   compiles the payload fine when invoked with `-mcpu=arm7tdmi -marm -mthumb-interwork`.

2. **A gbafix** — inserts the 156-byte Nintendo logo and fixes the header complement. **The
   GBA BIOS compares the logo against its own copy and locks up if it's wrong**, so this
   step is mandatory for real hardware. `build.sh` uses real `gbafix` (devkitARM, via
   `DEVKITARM`/`DEVKITPRO` or `PATH`) if present, otherwise the **bundled, dependency-free
   [`gbafix.py`](gbafix.py)** — a faithful reimplementation of the logo + complement fix, so
   no devkitPro install is needed.

The committed [`../gba_payload.h`](../gba_payload.h) is already a real, bootable image
(valid logo, passing header complement). Re-run `build.sh` only after editing the payload.

## Serial protocol (must match the host)

- 32-bit words, MSB-first, mode 3, RP2040 is the clock master.
- GBA → host: `[31:16] = 0x600D` framing tag, `[15:0]` = button bits (`GbaKey` order,
  active-high, from `~REG_KEYINPUT`).
- host → GBA: `p0<<24 | p1<<16 | p2<<8 | p3` (four parameter bytes).

See `../gba_link.cpp` for the host side.
