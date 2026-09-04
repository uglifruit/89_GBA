# Link cables and continuity checks (89_GBA)

Bench reference. **Do every check with everything powered off**, before a console is
connected to the Workshop Computer. Write your findings in the blank column — wire colours
are not standardised, not even between official Nintendo production runs, so the table you
fill in for *your* cable is the only trustworthy record.

Related: `TESTPLAN.md` (**the current bench procedure — start there**), `BRINGUP.md` (the
accumulated background), and the pin map in `../gba_spi.h`.

> **The crossover no longer has to be reasoned about — it can be measured.** Flash
> `cablecheck.uf2` and run MODE 0: it listens on *both* Pulse In 1 and Pulse In 2 at once and
> tells you which socket pin actually carries the GBA's SO. Sources genuinely disagree about
> how GBA link cables are wired internally, so this is the reliable way to settle it. See
> `TESTPLAN.md` STEP 1, and STEP 2B for the two-wire swap if it turns out crossed.

---

## The GBA link socket

Six pins. Pin 1 is at the **narrow** end of the trapezoid, pin 6 at the wide end; numbering
runs 1→6 across the socket. All directions below are **from the GBA's point of view** while
it is a multiboot slave (which is the only role we drive it in).

| Pin | Name | Dir | Connects to | Workshop GPIO |
|-----|------|-----|-------------|---------------|
| 1 | VCC +3.3V | out | **nothing — tape it off** | — |
| 2 | SO | **out** | Pulse In 1 | GPIO 2 |
| 3 | SI | **in** | Pulse Out 2 | GPIO 9 |
| 4 | SD | — | nothing | — |
| 5 | SC | **in** | Pulse Out 1 | GPIO 8 |
| 6 | GND | — | Computer GND | — |

Pins 2/3/5/6 are the only four you need. Pin 1 is a rail the **GBA sources** — it is an
output, not a supply input, and must be left floating. Pin 4 (SD) is unused in normal/SIO32
mode.

Verified against `../gba_spi.h:15-17`, `../main.cpp` and `../gba_spi.pio` — all three agree.

---

## Which cable is which

### A. GameCube → GBA cable (DOL-011) — **preferred**

**Straight-through on the GBA side.** No crossover: the GameCube is unambiguously the
master, so the GBA plug's pins run to the GC plug without a swap. Cut the GameCube end off
and you have an un-swapped breakout of the GBA connector. This is the cable that removes the
whole crossover problem, which is why it is worth using over a GBA-to-GBA cable.

Check continuity from each stripped wire to the GBA plug's pins:

| GBA plug pin | Signal | Expect | Your wire colour |
|---|---|---|---|
| 2 | SO | one wire, continuous | |
| 3 | SI | one wire, continuous | |
| 5 | SC | one wire, continuous | |
| 6 | GND | one wire, continuous | |
| 1 | VCC | **identify and isolate** | |
| 4 | SD | may be absent | |

Then confirm **no wire is shorted to any other** — buzz every pair. A short between SC and
GND, or SI and SO, will look exactly like "the handshake never syncs".

### B. GBA ↔ GBA link cable (AGB-005) — **the crossover trap**

**Pins 2 and 3 are SWAPPED between the two ends.** SO at one end arrives at SI on the other.
It also has a distinguishable "parent" and "child" plug, and you cannot reliably tell them
apart by eye.

If you use this cable, the pin that carries SO *at the end you kept* depends on which end you
kept. So:

1. Buzz pin 2 of your cut end against **both** pin 2 and pin 3 of the intact plug.
2. If your pin 2 → their pin 3, you are holding the crossover side. Your **pin 2 still is
   SO** at your connector — the swap is in the cable, not in the socket. Wire by the **socket
   pin numbers in the table at the top**, always.
3. Record which end you kept, physically label it, and don't mix it up later.

The failure this causes is silent: everything looks wired, and SI/SO are simply transposed,
so the GBA never sees a valid sync word.

### C. Bare/DIY connector or a chopped unknown cable

No assumptions available at all. Buzz all six pins to whatever conductors exist, fill in the
top table, and additionally verify the socket's **physical orientation** — confirm pin 1 by
finding the pin that reads ~3.3V to GND with the GBA powered on and nothing else connected
(this is the one check performed powered, and only with the Workshop side disconnected).

---

## Before connecting anything to the Workshop Computer

These three are not optional, and two of them are already-diagnosed faults from the last
bench session.

**1. Common ground.** With both units powered, measure DC from Computer GND to GBA GND. It
must read ~0V. Anything else means the grounds are not common and *every other reading you
take is meaningless*. A real GND wire through pin 6 is far better than borrowing the
headphone-jack sleeve.

**2. Series resistors on SC and SI — ~1kΩ each.** The Workshop pulse outputs swing to ~5–6V;
the GBA's inputs are 3.3V logic. The resistor plus the GBA's clamp diode limits the current.
**No resistor on SO** — the GBA drives that at 3.3V into us, which is fine as-is. Note that
using the GameCube cable does *not* fix this; the crossover and the voltage are separate
problems.

**3. Re-measure with the probe running.** SC, SI and SO should all sit within 0–3.3V (SC/SI
measured *after* their series resistors). Nothing should be negative. The last session
measured −2V on SC, which is actively harmful to the console — do not connect a GBA until
that is gone.

---

## Then

Power the **Computer first, then the GBA** — the GBA only syncs if the master is already
clocking when it powers up. Power-cycle the *GBA*, not the Computer, to retry.

Flash `bringup.uf2` and work the switch stages in order: UP (scope the output swing, GBA
disconnected) → MIDDLE (loopback) → DOWN (live handshake). See `BRINGUP.md`.
