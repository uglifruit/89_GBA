# 89_GBA — GBA PSG Voice

Turns the Music Thing Workshop Computer into a **Game Boy Advance link-cable master**, and
the GBA into a **chiptune synth voice played by the modular**. The module boots a
cartridge-less GBA over the pulse jacks using BIOS **Multiboot**, then streams its inputs
down a live SPI link. The GBA makes the sound on its own PSG hardware, out of its own
headphone jack, and carries the whole sound editor on its own screen.

**The division of labour is the design.** The Workshop senses; the GBA is the instrument.
This side reads five inputs and sends them raw; the GBA owns pitch tracking, the modulation
matrix, the envelope and the UI. That is what makes the on-screen editor free — re-mapping
an input is a change to a table in GBA RAM, not a protocol change and not a firmware rebuild.

> **Status: link WORKING on hardware (2026-09-06), PSG voice awaiting bench test.** A
> cartridge-less GBA boots from the module, runs the uploaded payload and talks both ways.
> **Testing the voice for the first time? Work down [`BENCH.md`](BENCH.md)** — each step
> isolates one thing, so a failure tells you where rather than just that.
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

## Playing it

Patch the GBA's **headphone jack** into the rack. It is around 1 Vpp against Eurorack's
~10 Vpp, so expect it to be quiet — a mixer or the next module's input gain will sort it.
The original GBA (AGB-001) and the Game Boy Micro have a headphone socket; the **GBA SP has
none** and needs the official SP adapter.

### Module panel

| Jack | Default role | Re-assignable? |
|------|--------------|----------------|
| **CV In 2** | 1V/oct pitch | yes, MAP page |
| **Pulse In 2** | gate / trigger | fixed |
| **CV In 1** | pulse duty (timbre) | yes, MAP page |
| **Audio In 1** | channel-2 detune | yes, MAP page |
| **Audio In 2** | noise level | yes, MAP page |
| **CV Out 2** | quantised pitch, calibrated 1V/oct | — |
| **CV Out 1** | gate mirror, 5 V | — |
| **Audio Out 1 / 2** | note / gate triggers | — |

LEDs: **0** link (solid booted, blinking connecting), **1** gate in, **2** note sounding,
**3** editing, **4+5** edit page as a binary pair.

### GBA controls

**PLAY** — D-pad up/down octave, left/right detune, **L**/**R** cycle the two square duties,
**A** manual trigger, **B** hold/latch, **START** opens the editor.

**EDIT** — **SELECT** cycles the five pages, D-pad moves and changes, **L**/**R** change in
steps of 8, **START** returns to play.

| Page | What it holds |
|------|---------------|
| VOICE | per-channel on/off, duties, detune, wavetable, noise pitch and type |
| ENVELOPE | attack, decay, sustain, release, retrigger, noise level |
| SWEEP | channel-1 hardware sweep, glide, octave |
| MAP | the modulation matrix: each input's destination and bipolar depth |
| CAL | **CV input scale and offset trim**, base note, master volume, PSG level |

**The CAL page is not filler.** ComputerCard calibrates the CV *outputs* from the module's
EEPROM, so the quantised pitch out is in tune for free — but there is **no calibration for
the CV inputs**, so 1V/oct tracking on CV In 2 depends on a scale constant that has to be
trimmed. Doing that on a screen with a live note readout beside it, rather than by
recompiling, is exactly what having a display is for.

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
- [x] **Protocol v1** (`gba_proto.h`), shared verbatim by firmware and payload
- [x] **PSG voice**: all four channels, software ADSR, 4-source modulation matrix
- [x] **On-screen editor**: five pages including CV input calibration
- [x] **Serial-IRQ link servicing**, with the proven polled path kept as a live fallback
- [ ] Patch persistence (the patch resets when the GBA powers off; upload is ~2.5 s)
- [ ] Further apps: performance pads, sequencer, scope

## Files

| File | Role |
|------|------|
| `main.cpp` | ComputerCard subclass (core 0): reads the inputs, drives CV/gate out |
| `gba_proto.h` | **The wire protocol, shared verbatim by both sides** |
| `gba_spi.pio` / `gba_spi.h` | PIO SPI master transport (GPIO 8/9/2) |
| `gba_multiboot.h` / `.cpp` | Multiboot uploader + SPI init |
| `gba_link.h` / `.cpp` | Core-1 link engine + `GbaShared` cross-core state |
| `gba_payload.h` | Baked GBA `.mb` image (placeholder until `payload/build.sh` is run) |
| `payload/link.c` | GBA serial slave + serial IRQ handler |
| `payload/psg.c` | PSG register layer, wavetables, note period table |
| `payload/synth.c` | Voice model: pitch, modulation matrix, envelope |
| `payload/ui.c` | Play screen + five-page editor |
| `payload/diag.c` | Link-characterisation screens (linkrate / bandwidth) |
| `payload/` | GBA-side program + build pipeline |
| `ComputerCard.h` | Vendored HAL (from the Workshop library) |

## Credits & references

Multiboot protocol + constants transcribed from the public open-source references
[akkera102/gba_01_multiboot](https://github.com/akkera102/gba_01_multiboot) and the RP2040
port in [copyrat90/gba-pico-gamepad](https://github.com/copyrat90/gba-pico-gamepad); the
PIO SPI core follows the Raspberry Pi
[pico-examples SPI](https://github.com/raspberrypi/pico-examples/tree/master/pio/spi)
CPHA=1 program. Built on Chris Johnson's ComputerCard HAL.
