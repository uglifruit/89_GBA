# 89_GBA — GBA PSG Voice

Turns the Music Thing Workshop Computer into a **Game Boy Advance link-cable master**, and the
GBA into a **chiptune synth voice played by the modular**. The module boots a cartridge-less GBA
over the pulse jacks using BIOS **Multiboot**, then streams its inputs down a live SPI link. The
GBA makes the sound on its own PSG hardware, out of its own headphone jack, and carries the
whole editor — eleven pages — on its own screen.

**The division of labour is the design.** The Workshop senses; the GBA is the instrument. This
side reads seven inputs and sends them raw; the GBA owns pitch tracking, the modulation matrix,
the envelopes, the ornaments, the drums and the UI. That is why the on-screen editor costs no
protocol: re-mapping Audio In 1 from detune to vibrato is a table write in GBA RAM, not a
firmware rebuild. Had the mapping lived on the Workshop side, every edit page would have needed
its own downstream opcode.

> **Status: working on hardware.** Boots, plays, edits, and stores patches on the card.
> **Testing it for the first time? Work down [`BENCH.md`](BENCH.md)** — each step isolates one
> thing, so a failure tells you where rather than just that.
>
> Bring-up was not straightforward. **Read
> [`diagnostics/POSTMORTEM.md`](diagnostics/POSTMORTEM.md)** before touching the transport.

## Wiring

Wire a bare GBA link socket to the module's pulse jacks, **common ground**:

| GBA pin | Workshop jack | RP2040 GPIO | Direction | Series R |
|---------|---------------|-------------|-----------|----------|
| 5 SC (clock) | Pulse Out 1 | GPIO 8 | RP2040 → GBA (master) | **1 kΩ** |
| 3 SI (MOSI)  | Pulse Out 2 | GPIO 9 | RP2040 → GBA | **1 kΩ** |
| 2 SO (MISO)  | Pulse In 1  | GPIO 2 | GBA → RP2040 | **none** |
| 6 GND        | ground      | —      | common | — |

Pins 1 (VCC — an output the *GBA* sources) and 4 (SD, unused in normal/SIO32) stay
disconnected. The 1 kΩ resistors go on the two lines **we drive**: the Workshop pulse outputs
swing ~6 V into 3.3 V inputs. Never put one in series with SO — it fights the pull-up that
biases the Pulse In 1 transistor stage.

> **Many GBA link cables CROSS SO/SI between their two ends.** If yours does, swap the two data
> wires at the Workshop end (SC and GND stay put). Do not guess — flash
> `diagnostics/cablecheck.uf2` and run MODE 0, which listens on Pulse In 1 and Pulse In 2 at once
> and reports which wire actually carries the GBA's SO. See
> [`diagnostics/CABLES.md`](diagnostics/CABLES.md).

Power the **Computer first, then the GBA** — the console only syncs if the master is already
clocking when it boots. Cartridge-less, on the logo screen. The upload takes about six seconds.

**Power-cycle the GBA whenever you reflash the Workshop.** A console already running a payload
never answers a multiboot sync, so the host sits in its retry loop for ever: LED 0 blinking,
LED 1 lit (NoGBA), and no Workshop data reaching the screen.

## Playing it

Patch the GBA's **headphone jack** into the rack. It is around 1 Vpp against Eurorack's ~10 Vpp,
so expect it to be quiet. The original GBA (AGB-001) and the Game Boy Micro have a headphone
socket; the **GBA SP has none** and needs the official SP adapter.

### The default patch

It boots playing something. A hard-panned square pair with a shimmer over the top, and **the
module's own switch triggers it** — no patch cables needed to hear it work.

| | |
|---|---|
| **Channel 1** | left, 50% duty, 10 ms attack, fast portamento |
| **Channel 2** | right, 25% duty, 250 ms attack, no portamento, +2 detune |
| **Channel 3** | both, sine on a permanent octave trill, level under the Main knob |
| **Channel 4** | off — the DRUM page is where noise earns its place |

The pair arrives from different sides at different times, which is most of why it sounds wide.

| Control | Does |
|---|---|
| **Switch down** | trigger |
| **CV In 2** | 1V/oct pitch |
| **Main knob** | level of channel 3 |
| **X knob** | attack, both envelopes together |
| **Y knob** | release, both envelopes together |
| **A** / **B** | trigger / hold |
| **L** | cycle both duty cycles |
| **R** | cycle channel 1's sweep time |
| **D-pad ↑ ↓ ← →** | ornament: major chord, minor chord, octave drop-and-leap, chromatic run |

The D-pad ornaments apply to the melodic pair only, so channel 3 keeps its trill underneath
while the lead switches figures. Up and Down loop; Left and Right are one-shots.

CV In 1 and both Audio Ins are deliberately unassigned — nothing should move that you did not
patch. The MAP page is where you give them a job.

### Module panel

| Jack | Default role | Re-assignable? |
|------|--------------|----------------|
| **CV In 2** | 1V/oct pitch | yes, MAP page |
| **Pulse In 2** | gate / trigger | TRIG page |
| **CV In 1** | pulse duty (timbre) | yes, MAP page |
| **Audio In 1** | channel-2 detune | yes, MAP page |
| **Audio In 2** | channel-4 level | yes, MAP page |
| **Main / X / Y knobs** | unassigned | yes, MAP page |
| **Switch (down)** | trigger | TRIG page |
| **CV Out 2** | quantised pitch, calibrated 1V/oct | — |
| **CV Out 1** | gate mirror, 5 V | — |
| **Audio Out 1 / 2** | note / gate triggers | — |

**LEDs while the link is up:** 0 link, 1 gate in, 2 note sounding, 3 editing, 4+5 edit page as a
binary pair.

**LEDs while it is not:** LED 0 blinks. During an upload, LEDs 1–5 are a five-segment progress
bar. Idle or failed, LEDs 1–3 are the last multiboot result as a 3-bit code — **1** NoGBA,
**2** BadHandshake, **3** TransferError, **4** CrcMismatch, **5** BadPayload.

### GBA controls

Every button is re-assignable on the BTN page. Out of the box: D-pad up/down octave, left/right
detune, **L**/**R** cycle the square duties, **A** trigger, **B** hold. **START** opens the
editor.

**Inside the editor no button carries its performance meaning** — not trigger, not hold, not the
mapped D-pad actions. Those belong to the performance screen. HOLD set before you enter still
holds, so latch a drone in PLAY and then go and edit it.

| | |
|---|---|
| D-pad alone | move the cursor (row, and on grid pages a column) |
| **A** + Up/Down | change the value |
| **A** + Left/Right | change coarsely, or the row's second field |
| **SELECT** + Left/Right | change page — hold it down to run through them quickly |
| **START** | back to the performance screen |

### The eleven pages

| Page | What it holds |
|------|---------------|
| **CHAN** | all four channels side by side: output, semitone, ornament, timbre, detune, noise, sweep |
| **TRIG** | pin grid — which sources trigger which channel (PU2, the Workshop switch, a GBA button) |
| **ENV** | per-channel ADSR with a drawn envelope, a live level tick, and per-voice portamento |
| **MIX** | four faders: level and OFF/L/R/BOTH, with the live envelope drawn inside the set level |
| **BTN** | what each of the eight GBA buttons does |
| **MAP** | modulation matrix: seven sources × destination × amount × per-voice tickboxes |
| **ORN** | ornaments — sixteen-step semitone sequences, edited graphically, with a loop/end marker |
| **DRUM** | a drum sound per input; any input going high fires it. Borrows channels 3 and 4 |
| **MEM** | sixteen patch slots on the card. A+Up saves, A+Down loads |
| **CAL** | **CV input scale and offset trim**, base note, master volume, PSG level, link mode |
| **SET** | master tuning in cents, key, scale (19 of them), octave, user-scale editor |

**The CAL page is not filler.** ComputerCard calibrates the CV *outputs* from the module's
EEPROM, so the quantised pitch out is in tune for free — but there is **no calibration for the
CV inputs**, so 1V/oct tracking on CV In 2 depends on a scale constant that has to be trimmed.
Doing that on a screen with a live note readout beside it, rather than by recompiling, is
exactly what having a display is for. It also shows the link's traffic counters, which separate
"the host is not sending" from "the values are not being updated".

**Triggering is a grid, not a switch.** Channel 1 can fire from the Workshop's momentary switch
*and* Pulse In 2 while channel 2 fires from the switch only. A channel wired to nothing is
deliberately silent, and HOLD does not override that.

**Drums live on the wavetable and noise channels, so both squares stay melodic.** Pitched drums
play on channel 3 because it can hold an arbitrary waveform — a kick has a body instead of being
a square with a fast decay — and its period sweeps like the squares do. Its one weakness, four
volume steps, is sidestepped by scaling the wavetable itself rather than using the volume
register. No Direct Sound and no DMA: real sample playback would need a timer, the FIFOs, and
sample data in an already six-second payload.

**The noise channel tracks pitch, but only in octaves.** Its frequency is
`524288 / r / 2^(s+1)`, so the shift field steps by a factor of two and nothing finer — the
eight divider ratios do subdivide an octave, but unevenly, so there is no honest chromatic
mapping to be had. Tick channel 4 on a PITCH mapping and it follows an octave at a time, which
is what tuned noise percussion has always meant on this hardware. `N PITCH` on the CHAN page and
the `N PITCH` destination still give you direct control.

**Ornament mappings are switches, not depths.** When a MAP destination is `ORNMNT` the amount
column names the *slot*; the source is on above halfway. There is no forty per cent of an
arpeggio.

## Patches

Sixteen slots live in the last flash sector of the RP2040 — 256 bytes each, one sector, because
the sector is the erase unit. The GBA's patch is in EWRAM and is gone the moment the console is
switched off, so the Workshop is where a patch has to persist.

A transfer is one byte per link word, each carrying its own index, so a dropped word leaves a
hole the receiver can see rather than silently shifting everything after it. A save repeats the
whole block until the host acknowledges; the host only ever commits a complete one.

**Saving clicks the audio.** Erasing flash stops XIP, so core 0 is parked in a RAM handler with
interrupts off for the few milliseconds the write takes, and the 48 kHz callback does not run.
That is the price of a deliberate action, and the reason patch storage is not something the
audio path does.

## How it works

- **Multiboot upload.** The RP2040 runs the documented single-cartridge handshake (sync → header
  → palette/handshake → encrypted payload → CRC) and streams the program into the GBA's EWRAM.
  See `gba_multiboot.cpp`. **`0xC0` must hold a BRANCH, not code** — the BIOS overwrites bytes
  `0xC4` and `0xC5` of the loaded image.
- **Live link.** After boot the RP2040 polls at **1 kHz**, sending the inputs and receiving
  buttons, note and status framed by a 16-bit tag. See `gba_proto.h` and `gba_link.cpp`. The
  inter-word gap is what matters, not the clock: the slave holds one pending transfer and re-arms
  with a read-modify-write, so a poll landing early corrupts the word rather than merely wasting
  it. 2000 words/s is measured clean, so 1 kHz runs at half the proven rate.
- **Two cores.** ComputerCard's 48 kHz audio/CV loop runs on **core 0**; the entire link engine
  runs on **core 1** and talks to core 0 only through the lock-free `GbaShared` struct.
- **Serial IRQ.** The GBA re-arms its slave from the serial interrupt, so it is not deaf while it
  redraws — with the polled path kept as a live fallback. The CAL page reports which is running.
- **PIO SPI with baked-in inversion.** The pulse jacks are hardware-inverted and GPIO 8/9/2
  aren't hardware-SPI pins, so the transfer is a PIO state machine (`gba_spi.pio`, mode 3,
  MSB-first, 32-bit). **Pulse In 1 inverts** — established by the GBA's own reply, not by
  inference; see the post-mortem.

## Building

RP2040 firmware (SDK 2.2.0, toolchain 14_2_Rel1):

```sh
cmake -G Ninja -B build -S .
cmake --build build          # -> build/gba_link.uf2
```

GBA payload → `gba_payload.h`:

```sh
cd payload && ./build.sh     # then rebuild the firmware
```

`build.sh` needs no devkitPro: it uses the Pico SDK's own `arm-none-eabi-gcc` and a bundled
dependency-free `gbafix.py` to insert the Nintendo logo and fix the header complement, both of
which the GBA BIOS validates. See [`payload/README.md`](payload/README.md).

## Files

| File | Role |
|------|------|
| `main.cpp` | ComputerCard subclass (core 0): reads the inputs, drives CV/gate out, LEDs |
| `gba_proto.h` | **The wire protocol, shared verbatim by both sides** |
| `gba_link.h` / `.cpp` | Core-1 link engine + `GbaShared` cross-core state |
| `gba_multiboot.h` / `.cpp` | Multiboot uploader + progress |
| `gba_spi.pio` / `gba_spi.h` | PIO SPI master transport (GPIO 8/9/2) |
| `patch_store.h` / `.cpp` | Sixteen patch slots in the RP2040's flash |
| `payload/link.c` | GBA serial slave, serial IRQ, protocol decode, patch transfer |
| `payload/psg.c` | PSG registers, twelve wavetables |
| `payload/synth.c` | Voice model: pitch, modulation, envelopes, ornaments, drums |
| `payload/ui.c` | Performance screen + eleven-page editor |
| `payload/diag.c` | Link-characterisation screens (linkrate / bandwidth) |
| `ComputerCard.h` | Vendored HAL (Chris Johnson's library, with Andy's fixes) |

## Credits & references

By **Andy Jenkinson**, 2026. Built on Chris Johnson's ComputerCard HAL for Tom Whitwell's
Music Thing Workshop Computer.

Multiboot protocol and constants transcribed from the public open-source references
[akkera102/gba_01_multiboot](https://github.com/akkera102/gba_01_multiboot) and the RP2040 port
in [copyrat90/gba-pico-gamepad](https://github.com/copyrat90/gba-pico-gamepad); the PIO SPI core
follows the Raspberry Pi
[pico-examples SPI](https://github.com/raspberrypi/pico-examples/tree/master/pio/spi) CPHA=1
program. Register details throughout from [GBATEK](https://problemkaputt.de/gbatek.htm).
