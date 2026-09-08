# 89_GBA — Claude working notes

GBA Multiboot link for the Music Thing Modular **Workshop Computer** (RP2040-based Eurorack
module). Boots a cartridge-less Game Boy Advance over the pulse jacks via BIOS Multiboot,
then runs a live SPI link so the GBA is a **PSG synth voice played by the modular** — sound
out of its headphone jack, sound editor on its own screen.

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

## The division of labour (the decision everything else follows from)

**The Workshop senses. The GBA is the instrument.** The Workshop reads its five inputs and
streams them down raw; the GBA owns pitch tracking, the modulation matrix, the envelope, the
sound and the whole UI. It sends back only what the rack needs: buttons, note, gate, status.

This is why the on-screen editor costs no protocol. Re-mapping Audio In 1 from detune to
vibrato is a change to a table in GBA RAM. Had the mapping lived on the Workshop side, every
edit page would have needed its own downstream opcode. **Do not move musical decisions to the
RP2040** — it is not where they belong and it makes the protocol grow without limit.

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

## Protocol v1 (`gba_proto.h` — ONE header, both sides)

C-compatible (payload is C, firmware is C++): plain `#define` and `static inline` only.

- **Downstream** tag in bits [31:29]: `0b100` STREAM (gate + edge + one 12-bit input pair +
  2 check bits), `0b000` CONTROL (`[28:24]` opcode, `[23:0]` arg), **`0b101` RESERVED**.
- **`0b101` is fenced off deliberately.** The three diagnostic magics (`0xA5A5`, `0xA6A6`,
  `0xBE7C`) all begin `101`. Giving STREAM `100` makes collision *structurally impossible*.
  Under a weaker tag a live stream word would impersonate the ramp magic about every 30 s at
  1 kHz and throw the instrument into link-test mode mid-performance.
- **Upstream: the 16-bit tag IS the kind.** `0x600D` keeps its exact old meaning (buttons), so
  every existing diagnostic still parses replies unchanged. `0x601E` status, `0x602A` note,
  `0x6033` param. Low bytes form a distance-3 code, so one flipped bit cannot turn one kind
  into another.
- **Gate edges are a COUNTER, not a sticky flag, on both sides.** A flag set by one context and
  cleared by another has two writers, and the losing interleaving is exactly the one that
  matters — an edge arriving between the read and the clear vanishes. Each side keeps its own
  tally and advances by one per edge, so a sub-millisecond trigger is never lost or merged.

## GBA-side gotchas that cost silence, not errors

1. **`SOUNDCNT_X` bit 7 (master enable) must be set BEFORE any other sound register write.**
   With the master off the sound registers are not writable at all, and clearing bit 7 resets
   them. `psg_init()` does this first.
2. **NEVER WRITE A BARE ZERO TO AN `NRx2` ENVELOPE REGISTER.** A channel's DAC is live only
   while the top five bits — volume *and* direction together — are non-zero, so a plain zero
   silences the channel *and* switches its DAC off, which disables the channel outright.
   Turning the DAC back on does not re-enable it; only a trigger does. `psg_sq_voice()` and
   `psg_noise_voice()` therefore set the direction bit whenever the volume is 0: equally
   silent, but the channel stays armed, so a retrigger from silence is instant rather than a
   recovery. This single fact was behind three separate faults — sustain 0 refusing to
   retrigger, the volume floor that existed to work around it, and the "first note sounds,
   every note after it is silent" bug before that.
3. **A trigger is a NOTE START, never a volume update.** The DMG only loads `NRx2`'s volume on
   a trigger, but the **AGB applies the write immediately**, which is what every GBA chip
   engine relies on. Retriggering each envelope step therefore bought nothing and cost:
   a duty-phase discontinuity every step (a quiet channel clicks its way through a fade),
   channel 1's sweep re-armed and its overflow check re-run once per step, and channel 4's
   LFSR reloaded so hiss became a tone at the step rate. `synth.c` keeps the switch as
   `PSG_VOL_NEEDS_RETRIGGER 0` — one line to put back if an envelope ever goes flat.
4. **Envelopes are software**, updated at ~1 kHz off Timer 0. The hardware envelope runs once
   per trigger and cannot sustain-then-release, which is the exact shape a gate input needs.
5. **No runtime division on the GBA.** The payload links `-nostdlib`, so libgcc is absent and
   a divide by a *variable* is an undefined `__aeabi_uidiv`/`__aeabi_idivmod` at LINK time.
   This is a deliberate tripwire, and it has already caught one `% rows` in the editor. Divides
   by compile-time constants are fine (multiply-and-shift). `synth.c` carries a shift-subtract
   `udiv32` for the one place that needs it.
6. **Pitch comes from a baked table** (`payload/notes.h`, generated by `gen_notes.py`), and
   sub-semitone pitch is linear interpolation *between period register values*. That is exact
   rather than approximate: the register is an affine function of 1/f. The wave channel is one
   octave down for the same register value, so channel 3 indexes the table 12 entries higher.
7. **The user stack sits at `0x03007E00`, not the conventional `0x03007F00`.** The BIOS gives
   the serial IRQ handler the IRQ stack at `0x03007FA0`; at `0x03007F00` that is only 160 bytes
   before the two collide, and `link_pump()` nests deeper than a typical handler. Nothing else
   uses IWRAM — the whole image is linked for EWRAM — so the move costs nothing.
8. **The serial IRQ does NOT replace the polled path.** `link_service()` still runs the same
   body with interrupts masked, so the two are mutually exclusive rather than racing. If the
   IRQ never fires the payload degrades to exactly the polled behaviour that has always worked,
   instead of going deaf and looking like a failed multiboot. The CAL page shows which is live.

## The drum engine

Pitched drums are on **channel 3 (wave)**, noise drums on **channel 4**. Both squares stay
melodic, which is the better half to keep — two squares is a lead and a bass.

The wave channel is the right home for percussion: it plays an arbitrary waveform, so a kick has
a body rather than being a square, and its period register sweeps like the squares do. Its one
weakness is that `SOUND3CNT_H` gives only four volume steps, far too coarse for a decay — so the
amplitude is applied by **scaling the wavetable samples** (`psg_wave_load_scaled`), which gives
sixteen steps for eight halfword writes. That is the standard Game Boy tracker trick, and it also
means the wave channel needs no trigger at all on a volume change — and since the retrigger came
out of the squares and noise too, nothing on the instrument now triggers except a note start.

Leaving drum mode must restore the melodic waveform: the drum engine has been overwriting wave
RAM with scaled copies of a drum body.

**No Direct Sound, no DMA.** `SOUNDCNT_H` stays at PSG-100%/DMA-off. Sample playback would need
DMA1/2, a timer driving the FIFO, and sample data in a payload already taking six seconds to
upload.

## Levels that could not reach zero

All of them the same shape — a value that says zero and does not mean it. Four found so far,
and the family is worth checking against first whenever a control seems dead at one end.

1. **Sustain 0 was not silent.** The envelope volume was floored at 1 for every state that was
   not IDLE, so a sustain of 0 decayed to a true zero and was floored straight back up. Narrowed
   to `== ENV_ATK`, then removed outright once the DAC no longer switches off (gotcha 2) — the
   floor's whole purpose was working around that.
2. **Master volume 0 is not silence on this hardware.** `SOUNDCNT_L` scales by `(vol+1)/8`, so 0
   is one eighth. The per-channel enables are dropped for that side instead, which genuinely
   mutes it.
3. **The melodic wave channel had four amplitude steps, not sixteen.** `SOUND3CNT_H` offers only
   mute / 25 / 50 / 100 %, so a 0-15 envelope mapped onto it made most of the envelope invisible:
   a sustain of 13 never left the 100% band, 11 to 15 were identical, and the release moved in
   three coarse jumps that read as stopping rather than decaying. It now scales the wavetable, as
   the drum engine already did. **If a control on channel 3 seems to do nothing, suspect this
   first** — its hardware volume register is far coarser than anything else on the instrument.
4. **A faded wavetable was a DC offset, not silence.** The wave DAC's centre lies *between*
   samples 7 and 8, so scaling a table toward 8 left every quiet table half a step above centre
   — a fixed offset that did not shrink with the envelope. The channel went inaudible well
   before it went quiet, and the mute at the bottom removed the offset in one step: a click
   arriving a second or so after the note had seemingly already stopped. `scale_nib()` now scales
   about 7.5 and splits the half step between neighbouring samples, so a faded table is
   7,8,7,8... — mean exactly at centre, its only content at half the wave clock, far above
   hearing and filtered by `SOUNDBIAS`.

Per-channel mixer level is fine and always was: `lv == 0` forces the output to zero explicitly.

## Patch storage

Sixteen 256-byte slots in the Workshop's last flash sector, one byte per link word in either
direction, each byte carrying its own index so a lost word leaves a hole the receiver can see.

- **The WHOLE slot is sent, zero-padded past the end of the `Patch`.** The host cannot know how
  big a `Patch` is, so a complete transfer is "every byte of the slot seen" and nothing else.
  Sending only `sizeof(Patch)` left the tail permanently unseen, so the host never committed,
  never acknowledged, and the GBA resent for ever — which also monopolised the upstream channel
  and froze the button reports.
- **`gba_proto.h` is the authority on where a byte sits in the word, and the two directions do
  not use the same place.** Upstream `GBA_UP_PATCH` is index `[15:8]`, value `[7:0]`; downstream
  `GBA_OP_PATCH` is index `[23:16]`, value `[15:8]`. The host once packed the downstream value
  into `[7:0]`, so every LOAD delivered 256 zeroes; the GBA's magic check threw them away and the
  page still reported DONE, because `PATCH_DONE` had arrived exactly as expected. **A load that
  changes nothing and a load with nothing to change now look different** — `LINK_RESULT_BAD`
  prints "BAD DATA - NOT LOADED".
- A save is self-healing without a retransmit protocol: the GBA repeats the whole block until the
  host acks, and the host commits only a block whose seen-bitmap is complete.

## Payload UI rules (learned the hard way, all of them)

1. **NEVER ERASE THEN DRAW.** Every flickering region had the same shape: clear a box to the
   background, then paint into it, once per frame. Two rules fix it everywhere — paint
   continuously-changing regions in ONE overwriting pass (background band, then value band), and
   compare rendered text against what is on screen and skip when unchanged. Static legends are
   drawn once, never per frame.
2. **A cache key must include everything that changes the drawing.** The MAP column cursor was
   folded into every row's comparison string, so moving it repainted all seven rows; the VOICE
   page's labels changed with the channel while only values were compared, leaving stale labels.
3. **A clear must not reach past what it owns.** The list clear covered the full ten-row height
   regardless of the page, erasing legends drawn below it. It now clears the taller of the old
   and new layouts and nothing more.
4. **`dec32()` right-aligns into a FIXED width and drops leading digits when it does not fit.**
   `dec32(buf, 10, 1)` yields `"0"` — that printed every envelope time above 5 ms as `0 MS`. Use
   `dec_at()` for anything whose digit count varies.
5. **Watch the buffer sizes.** The CAL line composes to 29 characters and had a 24-byte buffer;
   the stack smash made its redraw guard never hold, so it drew, cleared and redrew for ever.
6. **The `-nostdlib` tripwire is doing real work.** It has caught a `% rows` with a variable
   divisor, a drum-sweep divide, and an implicit `memcpy` from a struct assignment — each a LINK
   error rather than a silent bug. Do not "fix" it by linking libgcc.

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

- **No splash screen on a normal boot.** The green screen `crt0.s` paints in assembly is the
  boot proof; a title card after it was on screen for about as long as it took to clock one word.
  A message is drawn ONLY if the host stays silent, which is the one case where it says something.
- `payload/` is a **self-contained GBA-side sub-project** (no libgba, no libc) built for
  ARM7TDMI (armv4t). Modules: `link` (serial slave + IRQ), `psg` (registers), `synth` (voice,
  mod matrix, envelope), `ui` (play screen + five editor pages), `gfx`, `diag`, `main`.
  `build.sh` compiles every `.c` in the directory, so adding a module needs no script edit.
- **`diag.c` keeps the link-characterisation screens and must not be deleted.**
  `linkrate.uf2` and `bandwidth.uf2` link this same generated payload image; they are the
  instruments that produced every number in `POSTMORTEM.md` and how we would re-measure after
  a regression. While a diagnostic mode is active EVERY word routes to its decoder, because
  its corruption counter works by counting words matching neither magic.
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
