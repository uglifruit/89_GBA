# 89_GBA — GBA Link

Turns the Music Thing Workshop Computer into a **Game Boy Advance link-cable master**:
it boots a cartridge-less GBA over the pulse jacks using BIOS **Multiboot**, then keeps a
live SPI connection so the GBA can act as a controller + screen for the module.

> **Status: early / experimental (v0.1.0).** The RP2040 firmware is complete and builds;
> the GBA payload is a minimal bidirectional smoke test intended to grow into a real UI.
> See [Current state](#current-state).

## Wiring

Wire a bare GBA link socket to the module's pulse jacks, **common ground**:

| GBA pin | Workshop jack | RP2040 GPIO | Direction |
|---------|---------------|-------------|-----------|
| SC (clock) | Pulse Out 1 | GPIO 8 | RP2040 → GBA (master) |
| SI (MOSI)  | Pulse Out 2 | GPIO 9 | RP2040 → GBA |
| SO (MISO)  | Pulse In 1  | GPIO 2 | GBA → RP2040 |
| GND        | ground      | —      | common |

Then power on a GBA with **no cartridge** (it shows the multiboot/"download" screen).

## How it works

- **Multiboot upload.** The RP2040 runs the documented single-cartridge multiboot
  handshake (sync → header → palette/handshake → encrypted payload → CRC) and streams a
  small program into the GBA's EWRAM, which the BIOS then runs. See `gba_multiboot.cpp`.
- **Live link.** After boot, the RP2040 polls the GBA ~1 kHz: it sends four parameter
  bytes and receives the button bitfield. See `gba_link.cpp`.
- **Two cores.** ComputerCard's 48 kHz audio/CV loop runs on **core 0** (`main.cpp
  ProcessSample`). The entire GBA link engine runs on **core 1** and talks to core 0 only
  through the lock-free `GbaShared` struct — the audio path is never blocked. This reuses
  96_cathode's core-0/core-1 split for the pulse pins.
- **PIO SPI with baked-in inversion.** The pulse jacks are hardware-inverted and GPIO
  8/9/2 aren't hardware-SPI pins, so the transfer is a PIO state machine (`gba_spi.pio`,
  mode 3, MSB-first, 32-bit). All Workshop inversion is absorbed by IO-pad overrides, so
  the wire sees correct SPI polarity. The clock is deliberately slow (100 kHz) to stay
  within the transistor-conditioned Pulse In 1 input's bandwidth.

## v0 behaviour

**On the GBA screen:** four bars + a background tint driven by the module's three knobs +
CV In 1, with the buttons you press highlighted along the top.

**On the module:**

| GBA input | Module output |
|-----------|---------------|
| D-pad Up/Down | ramps CV Out 1 up/down |
| A / B | CV Out 2 = +high / −low |
| L / R | Audio Out 1 / 2 gate high |

LEDs: **LED 0** = link booted, **LED 1** = connecting/error, **LED 2–5** = A / B / L / R.

## Building

### RP2040 firmware (this directory)

Standard Pico SDK build (SDK 2.2.0, toolchain 14_2_Rel1):

```sh
cmake -G Ninja -B build -S .
cmake --build build
# -> build/gba_link.uf2
```

Flash `build/gba_link.uf2` to the Computer the usual way (hold the program button / mount
the RPI-RP2 drive, copy the UF2).

### GBA payload

The uploaded GBA program lives in [`payload/`](payload/) and is baked into the firmware as
[`gba_payload.h`](gba_payload.h) (a real, header-valid multiboot image — logo + checksum in
place). To regenerate it after changing the payload:

```sh
cd payload && ./build.sh    # then rebuild the firmware
```

`build.sh` needs no devkitPro install: it uses the Pico SDK's own `arm-none-eabi-gcc` to
compile (the GBA is armv4t) and a bundled dependency-free `gbafix.py` to insert the
Nintendo logo + fix the header complement (both validated by the GBA BIOS). If you have
real `gbafix`/devkitARM it uses those instead. See [`payload/README.md`](payload/README.md).

## Current state

- [x] RP2040 multiboot uploader (verified constants; builds clean)
- [x] PIO SPI transport with hardware-inversion handling
- [x] Two-core architecture + live polling loop
- [x] GBA payload source + build pipeline (compiles & links; `_start` at `0xC0`)
- [x] **Real bootable payload baked in** — valid Nintendo logo + passing header complement
      (`gba_payload.h`, 624 bytes); firmware builds clean with it
- [ ] Hardware bring-up: confirm the Pulse In 1 transistor input can clock the GBA's SO
      cleanly at 100 kHz (a loopback diagnostic is described in the project plan). This is
      the one unproven hardware assumption.
- [ ] Grow the payload into a real on-screen UI

## Files

| File | Role |
|------|------|
| `main.cpp` | ComputerCard subclass (core 0) + core-1 launch + v0 mapping |
| `gba_spi.pio` / `gba_spi.h` | PIO SPI master transport (GPIO 8/9/2) |
| `gba_multiboot.h` / `.cpp` | Multiboot uploader + SPI init |
| `gba_link.h` / `.cpp` | Core-1 link engine + `GbaShared` cross-core state |
| `gba_payload.h` | Baked GBA `.mb` image (placeholder until `payload/build.sh` is run) |
| `payload/` | GBA-side program + build pipeline |
| `ComputerCard.h` | Vendored HAL (from the Workshop library) |

## Credits & references

Multiboot protocol + constants transcribed from the public open-source references
[akkera102/gba_01_multiboot](https://github.com/akkera102/gba_01_multiboot) and the RP2040
port in [copyrat90/gba-pico-gamepad](https://github.com/copyrat90/gba-pico-gamepad); the
PIO SPI core follows the Raspberry Pi
[pico-examples SPI](https://github.com/raspberrypi/pico-examples/tree/master/pio/spi)
CPHA=1 program. Built on Chris Johnson's ComputerCard HAL.
