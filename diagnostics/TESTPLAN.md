# Bench test plan — 2026-09-04

Written after a full audit against [GBATEK](https://problemkaputt.de/gbatek.htm). Two new
things came out of it: **a firmware timing bug** (we have probably been sampling on the wrong
clock phase all along) and **a cable ambiguity that firmware cannot fix but can now identify**.

Work top to bottom. Each step says what to do, what each outcome means, and where to go next.

---

## What the audit found

### 1. The sample edge was probably wrong — and the old notes pointed the wrong way

GBATEK, *SIO Normal Mode*, is unambiguous:

> "During inactive transfer, the shift clock (SC) is high."
>
> "When master sends SC=LOW, each master and slave must output the next outgoing data bit to
> SO. When master sends SC=HIGH, each master and slave must read out the opponents data bit
> from SI."

So the link is **SPI mode 3**: SC idles HIGH, both ends **drive on the falling edge** and
**read on the rising edge**.

Our scope session already confirmed the polarity half of this by accident: with the state
machine stalled on `side 0`, the SC jack sits **HIGH at 6 V** — because Pulse Out 1 inverts.
That is exactly GBATEK's idle state, so `GBA_SCK_OUTOVER = NORMAL` is right.

**But `BRINGUP.md` has been telling us to use `gba_spi_cpha0`**, which samples while the jack
is LOW — during the *drive* phase, the opposite of what GBATEK specifies. `linkcheck` defaulted
to it too. That guidance came from reasoning about what the reference uploaders do, never from
a measurement, and it looks wrong.

The applet itself (`gba_multiboot.cpp`, using `gba_spi`) has **always been on the correct
timing**. It is the diagnostics that have been testing the wrong thing.

### 2. The cable crossover is a real, unresolved fork

A GBA↔GBA link cable **crosses SO/SI between its two ends**; a GameCube→GBA cable does not.
Sources disagree in their wording about exactly how GBA cables are wired, and your cable has
already surprised us once. So this is settled **by measurement, not by reading**.

If your cable crosses and the socket is wired by the GBA pinout (pin 2 = read, pin 3 = drive):

* we **drive our data into the GBA's SO pin** — one of its *outputs* (contention), and
* we **listen on the GBA's SI pin** — an *input*, so we only ever read our own pull-up.

Everything looks wired, nothing works, and no amount of edge/polarity sweeping helps. That
matches this session's symptoms exactly.

### 3. GBATEK gives a readiness signal we were ignoring

The master init sequence says: **"Wait for SI to become LOW (slave ready). (Check timeout
here!)"** A GBA waiting for multiboot pulls its SO low. We never checked for it — and it can
be detected with **no transfer at all**, so it is independent of every timing question.
`cablecheck` MODE 2 watches for it.

### 4. Hardware compatibility (asked, and checked)

* **GBA SP (AGS-001 / AGS-101) is pin- and electrically compatible** with the original
  AGB-001 link port. Either console is fine.
* **Game Boy Micro is NOT** — different, smaller connector. Do not use one.
* Pinout is confirmed identical across sources, including GBATEK's Xboo PC→GBA multiboot
  cable, which is the closest published analogue to what we are building:
  **pin 2 = SO (we read), pin 3 = SI (we drive), pin 5 = SC (we drive), pin 6 = GND.**
* GBATEK notes the Xboo cable's PC side runs 5 V against the GBA's 3 V logic — the same
  mismatch as our 6 V pulse outputs, and the reason for the series resistors.

---

## STEP 1 — `cablecheck.uf2`, MODE 0. Which wire carries the GBA's data?

**Do this first.** It is the only step that resolves a hardware unknown, and every later step
depends on the answer. It drives no data, so it is safe whichever way the cable is wired.

### Rewire (one wire moves, temporarily)

| Socket pin | Goes to | Note |
|---|---|---|
| 5 (SC) | Pulse Out 1 | keep the 1 kΩ |
| 6 (GND) | Computer GND | |
| 2 | **Pulse In 1** | |
| 3 | **Pulse In 2** | ← move this off Pulse Out 2 |
| — | Pulse Out 2 | leave disconnected |

Flash `cablecheck.uf2`. **LED4 + LED5 always show the mode in binary** — MODE 0 is both off.
Power the Computer first, then the GBA. Cartridge-less, on the Nintendo logo screen.

### Read the result

LED0 = Pulse In 1 has shown activity. LED1 = Pulse In 2 has shown activity.

| What you see | Meaning | Go to |
|---|---|---|
| **LED0 on, LED1 off** | Pin 2 carries SO. **Cable is STRAIGHT.** Wiring was right all along. | **STEP 2A** |
| **LED1 on, LED0 off** | Pin 3 carries SO. **Cable is CROSSED.** | **STEP 2B** |
| **Both off** | GBA is not driving at all. | **STEP 3** |
| **Both on** | Suspect a short between the data lines. | MODE 1, then **STEP 3** |

---

## STEP 2A — Cable is STRAIGHT

Put the wiring back as it was:

| Socket pin | Goes to |
|---|---|
| 2 | Pulse In 1 |
| 3 | Pulse Out 2 (via 1 kΩ) |
| 5 | Pulse Out 1 (via 1 kΩ) |
| 6 | Computer GND |

Go to **STEP 4**.

## STEP 2B — Cable is CROSSED: the swap fix

**Swap the two data wires at the Workshop end.** SC and GND do not move.

| Socket pin | Was | **Now** |
|---|---|---|
| 3 | Pulse Out 2 | **Pulse In 1** (carries the GBA's SO — we read it) |
| 2 | Pulse In 1 | **Pulse Out 2** via 1 kΩ (reaches the GBA's SI — we drive it) |
| 5 | Pulse Out 1 | unchanged, via 1 kΩ |
| 6 | GND | unchanged |

**No firmware change is needed** — the swap happens in copper, and the pin roles in
`gba_spi.h` stay exactly as they are. Move the 1 kΩ resistor with the driven wire, so the
series resistors always sit on the two lines we drive and never on the line we listen to.

Then go to **STEP 4**.

---

## STEP 3 — Nothing is driving. Rule these out in order

1. **Power-on order.** Computer first, *then* the GBA. This is real and documented: the
   console only syncs if the master is already clocking when it powers up. **Leave the
   Computer running and power-cycle the GBA only.**
2. **Cartridge-less, on the logo screen.** Not a game, not the menu.
3. **`cablecheck` MODE 2** (LED4 on, LED5 off) — the GBATEK slave-ready watch. LED0/LED1
   latch if either input has *ever* gone low. If one flickers low as the GBA boots, that line
   is the SO line and the console is alive — go back to STEP 2 with that knowledge.
4. **`cablecheck` MODE 3** (both on) — drives SC as a slow ~2 Hz square. Confirms the clock
   wire actually reaches the console; check it at the socket with a meter.
5. **`cablecheck` MODE 1** with the **GBA disconnected** — all four LEDs should be OFF. Any
   that light indicate a short in your own harness.
6. **Ground.** Re-buzz pin 6. Everything else is meaningless without it.

---

## STEP 4 — `linkcheck.uf2` TEST 2, on the GBATEK-correct timing

Flash the new `linkcheck.uf2`. It now carries **five sampling variants** instead of two, with
per-variant clock arithmetic (they have different cycles-per-bit, which the old single
constant got wrong).

**Set the knobs before you look:**

| Knob | Position | Selects |
|---|---|---|
| **X** | **fully CCW** | variant 0 — `m3-edge`, GBATEK canonical |
| **Y** | **fully CCW** | normal SCK polarity (the measured value) |
| **Main** | **fully CCW** | ~1 kHz, slowest and kindest to the transistor input |

Click to TEST 2 (LED4 on, LED5 off) and power-cycle the GBA while watching.

| LED | Meaning |
|---|---|
| **LED0** | **ECHO** — low-16 came back `0x6202`. The key light. |
| LED1 | **SYNC** — high-16 is `0x7202`. Full recognition. |
| LED2 | Reply looks like real data (≥3 bit transitions) |
| **LED3 flashing fast** | **Our own transport is stalling** — not the GBA's fault |

Solid = live now. Pulsing = latched earlier.

**If LED0 lights, that is the breakthrough** — bit timing is correct and the link works.

---

## STEP 5 — `linkcheck` TEST 3, the full sweep

If TEST 2 gives nothing, click to TEST 3 (LED4 on, LED5 on). It ignores the knobs and walks
**5 variants × 2 polarities × 5 rates = 50 slots**, ~0.7 s each.

**A full pass is ~35 seconds — let it run a full minute.**

LED0/LED1 latch and pulse if echo or sync is ever seen; LED3 lights once any best-slot has
been recorded.

Then **hold UP ~1 s** for the word readout. In TEST 3 the first nibble is the **winning slot
number** (mod 16), followed by the winning 32-bit word. Write down all 8 nibbles.

Decode the slot: `rate = slot % 5`, `polarity = (slot / 5) & 1`, `variant = (slot / 10) % 5`,
where rates are `{1k, 5k, 16k, 50k, 100k}` and variants are
`{m3-edge, m3-hold, m3-late, m3-wide, cpha0}`.

---

## STEP 6 — Read the word out and interpret it

The word readout is the tool that actually diagnoses. Hold UP ~1 s from any test; LED0–3 are
the nibble in binary, LED4 bright for the first four nibbles and dim for the last four, LED5
blinks between them. Click DOWN to leave.

| Word | Meaning |
|---|---|
| `0x00000000` | Line stuck low. Short to GND, or listening on the GBA's input (crossed cable). |
| `0xFFFFFFFF` | Line stuck high. Nothing driving it — we are reading our own pull-up. |
| `0xFFFF0000` or similar | One clean transition, no data. **Not** a reply — this is what fooled us before. |
| `0x????6202` | **ECHO — the link works.** Bit timing correct. |
| `0x7202????` | **Full sync.** |
| A rotation of `0x6202` | Off-by-N clock — the sample point is adjacent to correct; try the neighbouring variant. |
| Bit-reverse or inverse of `0x6202` | Polarity error — try the other Y-knob position. |
| Different every read | Noise, floating input, or a rate too fast for the transistor input. |

---

## Reference: LED conventions (both tools)

**LED4 + LED5 are always the mode/test number in binary** (LED4 = bit 1, LED5 = bit 0).
No test borrows them for data. If you are unsure where you are, read those two.

| LED4 | LED5 | `linkcheck` | `cablecheck` |
|---|---|---|---|
| off | off | 0 idle/wiring | 0 **listen (start here)** |
| off | on | 1 loopback | 1 short/crosstalk |
| on | off | 2 live GBA | 2 slave-ready watch |
| on | on | 3 sweep | 3 SC sanity |

Switch **clicks** — DOWN for next, UP for previous. Nothing needs holding except the ~1 s
UP-hold for the word readout.

---

## Still-open questions (honest list)

* **The sweep has never produced a positive result**, so no variant is yet proven on
  hardware. GBATEK says variant 0; that is a strong prior, not a measurement.
* **`cablecheck` and the five PIO variants have never been run on hardware.** They compile
  clean with `-Wall -Wextra`; that is all that is known.
* **Pulse In 2's pad inversion is assumed** to match Pulse In 1 (non-inverting, as measured).
  If MODE 0 gives a confusing result, that assumption is the first thing to doubt — invert
  the reading of LED1/LED3 mentally and see if it makes more sense.
* **Our sync loop sends `0x6202` and waits for `0x7202`**, following the two reference
  uploaders. GBATEK's own table shows a `6200` → `610y` → `720x` sequence instead. The
  references agree with each other so the code was left alone, but if the handshake starts
  responding and then stalls, this is the next thing to revisit.
