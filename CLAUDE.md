# 89_GBA — Claude working notes

GBA Multiboot link for the Music Thing Modular **Workshop Computer** (RP2040-based Eurorack
module). Boots a cartridge-less Game Boy Advance over the pulse jacks via BIOS Multiboot,
then runs a live SPI link so the GBA acts as a controller/screen for the synth.

Read this before editing. It records the non-obvious facts that make this applet work — most
would take a full re-investigation to rediscover.

## What this is

- **Standalone repo** (`uglifruit/89_GBA`), but the applet is a **Workshop Computer release**
  and is intended to be PR'd into `TomWhitwell/Workshop_Computer` under `releases/89_GBA/`
  later. Keep it self-contained (vendored `ComputerCard.h`, no external path deps) so that
  copy-in is clean.
- `ComputerCard.h` is the **vendored Workshop HAL** (Chris Johnson's library, base v0.3.0),
  copied per-applet. **This applet uses Andy's improved copy**, not stock — see below.

## Andy's ComputerCard.h fixes (use this copy, don't regress to stock)

The vendored HAL here is a **pure superset of upstream v0.3.0** (+18 lines, nothing removed),
carrying two fixes Andy developed across his projects. The canonical copy lives in
`91_Chorgan` / `95_OffAir` (byte-identical); `89_GBA` matches them. `96_Cathode` / `60_Markov`
still carry **stock** v0.3.0 — do not copy their HAL over this one.

1. **Power-on click removal.** Pre-fills the SPI DAC buffers with 0V "silence" words before
   the first DMA transfer, so the first output is silence rather than uninitialised RAM.
   Applies to any applet using audio/CV out (89_GBA does).
2. **ADC channel-alignment fix.** Stops the ADC (`ADC_CS_START_MANY`) and drains the FIFO
   before touching AINSEL, guaranteeing the DMA burst always restarts on channel 0. Without
   it, an in-progress conversion lands as `ADC_Buffer[n][0]` and shifts the whole burst by
   1–3 slots (knobs/CV read from the wrong channels). Applies to any applet reading ADC.

When starting a **new** Workshop applet, seed `ComputerCard.h` from `95_OffAir`/`91_Chorgan`
(or this repo), not from a stock-HAL project. These fixes are Andy's preferred baseline.

## Hardware facts you must not forget

These are the ones that silently break things if ignored:

1. **Sample callback is 48 kHz.** Applets subclass `ComputerCard` and override
   `ProcessSample()`, called once per audio sample (~20.8 µs budget). Not configurable. Use
   integer/fixed-point math; float is soft-emulated and slow. This is the base rate for
   everything (timers, etc.).
2. **All four pulse jacks are inverted in hardware.** The HAL hides this
   (`PulseOut1(true)` → GPIO low; `PulseIn1()` = `!gpio_get`). But the moment you drive the
   pins **directly** (PIO/bitbang), you own the inversion. Here it's absorbed by IO-pad
   overrides in `gba_multiboot.cpp` (`gpio_set_outover`/`gpio_set_inover`) — see the net-
   inversion derivation in `gba_spi.pio`. Getting this wrong = "nothing works".
3. **Pin map** (RP2040 GPIO): Pulse Out 1 = **8**, Pulse Out 2 = **9**, Pulse In 1 = **2**,
   Pulse In 2 = **3**. LEDs 10–15. Audio DAC on SPI0 (18/19/21) — **do not touch SPI0**.
4. **Pulse In 1 (GPIO 2) is a slow transistor gate input**, not a clean logic pin, and it
   **INVERTS** — `GBA_MISO_INOVER = GPIO_OVERRIDE_INVERT`. This was the single most expensive
   bug in the project: a scope check that appeared to prove otherwise had both probes on the
   same net, either side of a patch cable. The GBA settled it by replying `0x8DFD9DFD`, the
   exact bit-complement of the expected `0x72026202`.
   Its **bandwidth was never the problem** — that long-flagged risk is closed. It carries
   2000 words/s cleanly (~64 kbit/s). What limits the link is the **GBA slave's re-arm gap**,
   not this input.
5. **EVERY transfer needs an inter-word gap.** The GBA slave holds exactly ONE pending
   transfer and re-arms with a read-modify-write of `SIOCNT`; a word clocked before it has
   re-armed is not merely lost, it is corrupted (bit-slip). At 100 kHz SCK a 32-bit word takes
   320 µs and ~180 µs of gap is ample; zero gap fails completely. **This applies to every
   message, including out-of-band ones** — a rate-announce word sent without a gap silently
   destroyed the word after it and produced a fixed error count that looked like a hardware
   floor.
6. **The link cable CROSSES SO/SI.** Measured, not assumed — `cablecheck.uf2` listens on Pulse
   In 1 and Pulse In 2 at once and reports which wire actually carries the GBA's SO. Sources
   genuinely disagree about GBA cable wiring; do not reason about it, measure it.
7. **The HAL samples pulse inputs only at 48 kHz.** For the SPI link we read GPIO 2 via PIO,
   never `PulseIn1()`.

## Architecture (two cores)

- **Core 0**: ComputerCard's 48 kHz loop (`main.cpp` `ProcessSample`). Runs inside
  `DMA_IRQ_0`. Never block it.
- **Core 1**: the **entire GBA link engine** (`gba_link_core1`) — runs multiboot, then the
  post-boot polling loop. Owns **PIO0 SM0** and reassigns GPIO 8/9/2 to PIO on entry.
  ComputerCard leaves both PIO blocks, SPI1, `DMA_IRQ_1`, and high DMA channels free.
- Cores communicate **only** through the lock-free `GbaShared gGba` struct (`gba_link.h`):
  each field single-writer, `volatile`, ≤32-bit so writes are atomic on RP2040. Core 1
  publishes `buttons`; core 0 writes `params[]`. This mirrors 96_cathode's core-split.
- Reference precedent for pulse-pin PIO + DMA on core 1 is **`releases/96_cathode`** in the
  monorepo (composite-video applet). It's the template for this pattern.

## The multiboot protocol

- `gba_multiboot.cpp` implements the single-cartridge / download-play upload: sync (`0x6202`
  → `0x7202`) → header (0xC0 bytes) → palette/handshake (`0x63D1`, `0x64hh`) → length/seed →
  encrypted main transfer → final CRC handshake.
- **The encryption/CRC magic constants are not secrets** — they're a fixed, publicly
  documented BIOS obfuscation. Transcribed from two agreeing open references:
  `akkera102/gba_01_multiboot` and the RP2040 port `copyrat90/gba-pico-gamepad`. If you touch
  the transfer loop, cross-check against both. Key numbers: LCG mult `0x6F646573`, key
  `0x43202F2F`, CRC poly `0xC37B`, CRC seed `0xC387`, offset transform `0xFE000000 - i`.
- Transport is **mode 3 SPI (CPOL=1, CPHA=1), MSB-first, 32-bit**, RP2040 = clock master.
  GBATEK, *SIO Normal Mode*: SC idles HIGH, both ends drive on the falling edge and sample on
  the rising edge. **Multiboot runs at 100 kHz** — measured repeatable; 200 kHz is marginal and
  leaves the console restarting its boot, which is worse than running slower. Post-boot the
  applet polls at **1 kHz**.
- PIO core follows the Raspberry Pi pico-examples SPI CPHA=1 program.

## The GBA payload

- `payload/` is a **self-contained GBA-side sub-project** (no libgba) built for ARM7TDMI
  (armv4t). It's a serial-slave that mirrors the host packet format and draws a UI.
- **Multiboot entry is at offset `0xC0`** (not the header branch at 0x00). `crt0.s` must keep
  `_start` at exactly `0x020000C0`, image linked for EWRAM (`multiboot.ld`).
- **The GBA BIOS validates the Nintendo logo (0x04..0x9F) and the header complement (0xBD)**
  and locks up if either is wrong. `payload/build.sh` fixes both: it uses the Pico SDK's own
  `arm-none-eabi-gcc` to compile and a **bundled dependency-free `gbafix.py`** (logo +
  complement) — **no devkitPro install needed**. Real `gbafix`/devkitARM is used if present.
- `gba_payload.h` is the **generated** byte array (via `bin2h.py`, padded to 16 bytes). It's
  committed as a real bootable image; regenerate with `payload/build.sh` after editing the
  payload, then rebuild the firmware.

## Build

RP2040 firmware (produces `build/gba_link.uf2`):
```sh
cmake -G Ninja -B build -S .
cmake --build build
```
On this machine the toolchain lives under `~/.pico-sdk` (SDK 2.2.0, toolchain 14_2_Rel1) via
the Pico VS Code extension; `PICO_SDK_PATH`/`PICO_TOOLCHAIN_PATH` may need pointing there when
building from a bare shell. SDK 2.2.0 has no `pio_set_input_sync_bypass_with_mask` helper —
poke `pio->input_sync_bypass` directly (see `gba_multiboot.cpp`).

GBA payload → `gba_payload.h`:
```sh
cd payload && ./build.sh
```

## Conventions

- Match `96_cathode`'s CMake link set and `set_sys_clock_khz(144000, true)` (clean multiple
  of 48 MHz → tidy PIO clkdivs).
- `info.yaml` follows Tom's format (no quotes, `draft: false`); it documents the jack/LED
  panel labels for the release.
- **Measured link numbers** (`diagnostics/POSTMORTEM.md` has the full record): multiboot
  100 kHz, sustained 2000 words/s clean, ~64 kbit/s, round trip ≤0.5 ms, 32 KB payload uploads
  in ~5 s. SCK above 100 kHz has never been *fairly* tested — the readings that suggested a
  ceiling were taken with zero inter-word gap, so they measured the re-arm race instead.
- **When a measurement contradicts something already known to work, doubt the measurement.**
  Several days went into faults that turned out to be in the diagnostics rather than the
  hardware: a scope probe on the wrong net, a loopback with a structural blind spot, a
  diagnostic that blocked the 48 kHz callback, and an instrument that corrupted the very
  words it was counting. `POSTMORTEM.md` records each one and how it was caught.
