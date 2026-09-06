# Bring-up post-mortem — how the GBA link was actually made to work

**Status: WORKING (2026-09-06).** A cartridge-less GBA boots from the Workshop Computer over
the pulse jacks, runs the uploaded payload, displays *"MTM - Workshop Computer Link"*, and
reports its buttons back — A/B/L/R light LEDs 2–5 on the module.

This is the record of what actually went wrong and what the debugging cost, because almost
none of it was the thing we thought it was. Four separate faults were stacked on top of each
other, and several of our own diagnostics were among them.

---

## The four real faults

| # | Fault | Cost | Found by |
|---|---|---|---|
| 1 | **The link cable crosses SO/SI.** We were driving the GBA's *output* pin and listening on its *input*. | Days | `cablecheck` MODE 0, listening on both pulse inputs at once |
| 2 | **MISO polarity was inverted.** Pulse In 1's transistor stage *does* invert; we had it as NORMAL. | Days | The GBA's own reply word |
| 3 | **The clock divisor was wrong.** Both PIO programs are 4 cycles/bit; the code divided by 3, so every rate ran 4/3 fast. | Minor | Code audit against the PIO source |
| 4 | **The payload went deaf mid-frame, and the host gave up too fast.** The GBA slave holds one pending transfer; the payload blocked and redrew while unarmed, and the host tore the link down after ~32 ms of silence. | Hours | Reading the payload loop after multiboot started succeeding |

---

## Lesson 1 — a measurement that doesn't probe what you think is worse than no measurement

The single most expensive error in the project.

To settle whether Pulse In 1 inverts, we drove Pulse Out 2 into Pulse In 1 **through a patch
cable** and put a scope probe on **each jack**. They tracked, so we concluded the input stage
does not invert, wrote `GBA_MISO_INOVER = NORMAL`, and recorded it as *scope-measured* — the
highest-confidence label available.

**Both probes were on the same net.** A patch cable is a wire. The measurement could only ever
have produced "they track"; it never observed the input stage at all. The conclusion was
recorded as fact, propagated into a shared constant, and became the foundation for days of
work in the wrong direction.

The truth came from the GBA: the reply read `0x8DFD9DFD`, the exact bit-complement of
`0x72026202` — `0x7202` recognition, `0x6202` echo. Both halves complement exactly.

**Rule:** before trusting a measurement, state which component sits *between* the two probes.
If the answer is "a piece of copper", the measurement is vacuous.

## Lesson 2 — know your test's structural blind spot, and don't let it vote

The loopback (drive Pulse Out 2 → Pulse In 1, expect the word back) passed bit-exact
throughout, and was repeatedly cited as evidence the transport was correct.

It only ever proved **MOSI-inversion XOR MISO-inversion == 0**. A *matched pair* of errors
passes perfectly — which is exactly what we had. This blind spot was documented in
`BRINGUP.md` from the very first session, and we still let the passing result reassure us.

Worse, the loopback and the live link genuinely disagree here: our ~6 V output driving the
transistor input is a different operating point from the GBA driving it at 3.3 V. After
fixing MISO the loopback is *expected to fail*. A test that cannot fail in the presence of the
bug is not evidence of correctness.

## Lesson 3 — the diagnostic is part of the system under test

Four separate bugs in our own tools produced confident, wrong readings:

- **Blocking transfers in `ProcessSample`.** `put_blocking`/`get_blocking` on a 48 kHz audio
  callback with a 20.8 µs budget, while a 32-bit word takes ~854 µs. The DMA IRQ re-entered
  mid-transfer and left the state machine half-fed — producing "SC idles high with only 2–3
  clock pulses", which looked exactly like a hardware fault. (The applet was never affected:
  it runs the link on core 1, where blocking is correct.)
- **A wedge that faked a dead console.** `loadPio()` reset the state machine without clearing
  the in-flight flag, so a knob parked near a threshold thrashed the reload and left the tool
  waiting forever for a reply that could never arrive. Four dark LEDs, indistinguishable from
  "the GBA is silent".
- **Latches with no visible reset.** `cablecheck`'s activity flags were sticky, so plugging
  and power-on transients stayed on the display forever — it reported activity on both inputs
  **with the GBA switched off**. Andy caught this one.
- **A detector too loose to be useful.** "Structured reply" was `r != 0 && r != 0xFFFFFFFF`,
  which cheerfully classified `0xFFFF0000` — a line sitting high and falling once, i.e. *not
  being driven at all* — as real data.

**Rule:** when a diagnostic reports something surprising, suspect the diagnostic first. Every
one of these was found by asking "what would make the tool say that even if the hardware were
fine?"

## Lesson 4 — encode the data, not a verdict

The turning point was the **word readout**: clocking the raw 32-bit reply out as eight nibbles
in binary across four LEDs.

Before it, six LEDs carried six boolean flags, and the answer to "what did the GBA actually
say?" was unavailable. Flags can only answer questions you already thought to ask. `0x8DFD9DFD`
answered a question nobody had asked — *is it inverted?* — instantly and unambiguously.

Related: the readout initially showed the *last* word, and a sweep that ended on a dead slot
overwrote the evidence with zeros. Showing the *richest* word (most bit transitions) fixed
it. **Keep the most informative sample, not the most recent one.**

## Lesson 5 — the device under test outranks your inferences

Two independent chains of reasoning said MISO was NORMAL. Both were wrong, and both were
*ours*. The GBA said INVERT in a way that admitted no other reading — the precise
bit-complement of the expected word is not something noise produces.

When the DUT contradicts your model, the model loses. This is also why `cablecheck` MODE 0 was
worth building: it stopped the crossover being argued from datasheets (which genuinely
disagree about GBA cable wiring) and made the console answer it.

## Lesson 6 — read the right section of the spec

GBATEK's multiboot table shows `6200`/`FFFF`/`0000`/`610y`/`720x` exchanges, and we briefly
built a whole diagnostic around the theory that `0x0000` was a documented success reply we
were discarding.

Those are **16-bit** words. SIO *normal* mode only has 8-bit and 32-bit. That table describes
the **multi-play** multiboot variant — a different protocol on a different topology. The
applet's `0x6202` → `0x7202` in normal 32-bit mode, matching the reference uploaders, was
right all along.

The tool still earned its place: it *disproved* the theory in one run.

## Lesson 7 — a half-duplex slave needs continuous service

Once multiboot succeeded, the link connected and immediately dropped. Neither side was wrong
about the protocol; both were wrong about *timing*.

The GBA slave can hold exactly **one** pending transfer. Hardware clears `SIO_START` when the
host finishes clocking a word, and the console is deaf until re-armed. The payload blocked in
a 200,000-iteration spin (~180 ms out of EWRAM) and then redrew the whole screen while
unarmed. Meanwhile the host polled every 1 ms and declared the link dead after 32 misses —
**32 ms of silence tore it down and restarted multiboot.**

Fix on both sides: the payload never blocks and re-arms the instant a word lands, calling
`service()` between every drawing step; the host tolerates ~5 s and polls at 200 Hz instead of
1 kHz, because polling faster than the slave can re-arm only manufactures bad words.

## Lesson 8 — make failure modes visually distinct

The payload originally filled the screen with `rgb15(0, 8, 16)` — a dark blue that reads as
**black on an unlit GBA**. So "the image never ran" and "ran but the link is quiet" looked
identical.

The title is now drawn **first, before any serial setup**, precisely so the screen alone
separates those two cases. It was asked for as a feature; it is just as valuable as
instrumentation.

---

## What survived unchanged

Worth recording, because it was the part most feared:

- **The multiboot protocol implementation was correct from the first draft.** Sync, header,
  palette/handshake, length/seed, the encrypted transfer with its per-word offset check, and
  the final CRC — all of it worked the moment the wiring and polarity were right. Transcribing
  the magic constants from two agreeing references paid off exactly as intended.
- **The two-core split held.** The 48 kHz audio callback was never the problem; the one time
  blocking hurt, it was a diagnostic on core 0, not the applet on core 1.
- **`Pulse In 1` was never the bottleneck.** The project's one flagged unproven assumption —
  "can a slow transistor gate input clock this?" — turned out fine. It inverts, which we
  missed, but its bandwidth was never the issue.

## The tools, in the order they earned their keep

| Tool | What it settled |
|---|---|
| `bringup.uf2` | Output swing, edges, the electrical layer |
| `cablecheck.uf2` | **Which wire carries the GBA's SO** — the crossover, by measurement |
| `linkcheck.uf2` | Sampling variants, and the **word readout** that exposed the inversion |
| `handshake.uf2` | Disproved the `0x0000` theory in one run |

`cablecheck` exists only because **Pulse In 2 (GPIO 3) was unused** by the project. Having a
spare input to listen on turned an unresolvable argument into a two-minute measurement. Worth
remembering when planning pin budgets: keep one input free for debugging.
