# 89_GBA — GBA Link

Turns the Music Thing Workshop Computer into a **Game Boy Advance link-cable master**:
it boots a cartridge-less GBA over the pulse jacks using BIOS **Multiboot**, then keeps a
live SPI connection so the GBA can act as a controller + screen for the module.

> **Status: WORKING on hardware (2026-09-06).** A cartridge-less GBA boots from the module,
> runs the uploaded payload, shows *"MTM - Workshop Computer Link"*, and reports its buttons
> back - A/B/L/R light LEDs 2-5. The payload is still a minimal bidirectional demo intended
> to grow into a real UI.
>
> Bring-up was not straightforward. **Read
> [`diagnostics/POSTMORTEM.md`](diagnostics/POSTMORTEM.md)** before touching the transport:
> four stacked faults, several of them in our own diagnostics, and the non-obvious facts that
> make this work are recorded there.

## Wiring

Wire a bare GBA link socket to the module's pulse jacks, **common ground**:

| GBA pin | Workshop jack | RP2040 GPIO | Direction | Series R |
|---------|---------------|-------------|-----------|----------|
| 5 SC (clock) | Pulse Out 1 | GPIO 8 | RP2040 → GBA (master) | **1 kΩ** |
| 3 SI (MOSI)  | Pulse Out 2 | GPIO 9 | RP2040 → GBA | **1 kΩ** |
| 2 SO (MISO)  | Pulse In 1  | GPIO 2 | GBA → RP2040 | **none** |
| 6 GND        | ground      | —      | common | — |

Pins 1 (VCC - an output the *GBA* sources) and 4 (SD, unused in normal/SIO32) stay
disconnected. The 1 kΩ resistors go on the two lines **we drive**: the Workshop pulse outputs
swing ~6 V into 3.3 V inputs. Never put one in series with SO - it fights the pull-up that
biases the Pulse In 1 transistor stage.

> **Many GBA link cables CROSS SO/SI between their two ends.** If yours does, swap the two
> data wires at the Workshop end (SC and GND stay put). Do not guess - flash
> `diagnostics/cablecheck.uf2` and run MODE 0, which listens on Pulse In 1 and Pulse In 2 at
> once and tells you which wire carries the GBA's SO. See
> [`diagnostics/CABLES.md`](diagnostics/CABLES.md).

Power the **Computer first, then the GBA** - the console only syncs if the master is already
clocking when it boots. Power-cycle the *GBA* to retry. Cartridge-less, on the logo screen.

## How it works

- **Multiboot upload.** The RP2040 runs the documented single-cartridge multiboot
  handshake (sync → header → palette/handshake → encrypted payload → CRC) and streams a
  small program into the GBA's EWRAM, which the BIOS then runs. See `gba_multiboot.cpp`.
- **Live link.** After boot, the RP2040 polls the GBA at **1 kHz**: it sends four parameter
  bytes and receives the button bitfield, framed with a `0x600D` tag. See `gba_link.cpp`.
  The inter-word gap is what matters, not the clock: the slave holds only one pending
  transfer and re-arms with a read-modify-write, so a poll landing before it has re-armed
  corrupts the word rather than merely wasting it. 2000 words/s is measured clean, so 1 kHz
  runs at half the proven rate.
- **Two cores.** ComputerCard's 48 kHz audio/CV loop runs on **core 0** (`main.cpp
  ProcessSample`). The entire GBA link engine runs on **core 1** and talks to core 0 only
  through the lock-free `GbaShared` struct — the audio path is never blocked. This reuses
  96_cathode's core-0/core-1 split for the pulse pins.
- **PIO SPI with baked-in inversion.** The pulse jacks are hardware-inverted and GPIO
  8/9/2 aren't hardware-SPI pins, so the transfer is a PIO state machine (`gba_spi.pio`,
  mode 3, MSB-first, 32-bit). All Workshop inversion is absorbed by IO-pad overrides, so
  the wire sees correct SPI polarity. Timing follows GBATEK's *SIO Normal Mode*: SC idles
  HIGH, both ends drive on the falling edge and sample on the rising edge (SPI mode 3).
  **Pulse In 1 inverts** - established by the GBA's own reply, not by inference; see the
  post-mortem. The multiboot clock is chosen at runtime from a ladder (100k, 50k, 16k, 5k,
  1k), trying each until the upload succeeds, since the working rate is a property of the
  cable and console rather than a constant.

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
- [x] **Hardware bring-up complete.** The GBA boots, runs the payload, shows
      *"MTM - Workshop Computer Link"*, and reports its buttons back. The Pulse In 1
      bandwidth question — flagged from the start as the make-or-break unknown — turned out
      **not** to be the problem at all; see [`diagnostics/POSTMORTEM.md`](diagnostics/POSTMORTEM.md).
- [x] **Link characterised**: multiboot 100 kHz, 2000 words/s sustained (~64 kbit/s),
      round trip ≤0.5 ms, 32 KB payload uploads in ~5 s
- [ ] Protocol v1: typed opcodes shared by both sides
- [ ] On-screen menu + app framework
- [ ] Apps: chiptune voice, performance pads, sequencer, scope

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
