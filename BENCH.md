# Bench checklist — GBA PSG Voice, first power-on

Work down this list. Each step isolates one thing, so a failure tells you *where* rather than
just *that*. Stop at the first step that fails and read its "if it fails" note — most of them
point at a specific fault this project has already had once.

Flash `build/gba_link.uf2`. Power the **Computer first, then the GBA**, cartridge-less, on the
logo screen.

---

### 1. It boots

**Expect:** after ~2.5 s the GBA shows *"MTM - Workshop Computer Link"* and *"GBA PSG VOICE"*,
then switches to the play screen. Workshop **LED 0 goes solid**.

*If the screen stays blank:* the image never ran — a wiring or multiboot fault, not a synth
fault. Fall back to `diagnostics/mbrate.uf2` (LED 3 = 100 kHz is the proven rung).

*If the title appears but the play screen never does:* the host is not talking. LED 0 will be
blinking. The splash holds for about 3 s and then gives up, so this is visible.

*If it boots and then the screen goes dark or freezes:* suspect the **serial IRQ**. Set
`LINK_USE_IRQ` to `0` at the top of [`payload/link.h`](payload/link.h), re-run
`payload/build.sh`, rebuild the firmware. That reverts to the polled servicing that has worked
since bring-up, and the CAL page will then read `POLLED`.

### 2. It makes a sound

**Expect:** press **A** on the GBA. A note sounds from the headphone jack. LED 2 lights.

*If silent but the screen responds:* check the headphone jack actually carries audio (the
**GBA SP has no headphone socket** — it needs the SP adapter). If the jack is fine, this is
almost certainly PSG register ordering: `SOUNDCNT_X` bit 7 must be set before any other sound
register write, and the envelope volume must be written before the trigger. Both are done in
that order deliberately — see `CLAUDE.md`.

*If the FIRST note sounds and later ones do not:* that is exactly the DAC-off failure the
trigger ordering exists to prevent. Worth reporting precisely, because it means the ordering
fix did not take.

### 3. The envelope shapes

**Expect:** hold **A**: the note attacks, decays to a sustain, and stops when released. The
envelope bar on screen follows it.

*If the volume never changes:* the software envelope writes `NRx2` every control tick. If this
hardware needs a re-trigger to apply a volume write ("zombie mode"), the note will play at a
fixed level. Report it — the fix is a different envelope strategy, not a tweak.

### 4. Pitch tracks CV

**Expect:** patch a keyboard or sequencer into **CV In 2**. The big note name follows it, and
**CV Out 2** into a tuner reads the same note.

**Then trim it.** `START` → `SELECT` to the **CAL** page. `CV SCALE` is counts per semitone in
sixteenths; it starts at **455** (28.4 counts, the arithmetic estimate). Play an octave apart
and adjust until the two notes really are twelve semitones apart. `CV OFFSET` shifts the whole
range. There is a live `READS` note and `RAW` count on that page to trim against.

*Why this needs doing at all:* ComputerCard calibrates the CV **outputs** from EEPROM, but
there is **no factory calibration for the CV inputs**. The starting number is arithmetic, not
measurement.

### 5. The gate works, including short triggers

**Expect:** a gate into **Pulse In 2** plays notes. LED 1 follows the gate.

Then try a **very short trigger** — well under 1 ms. It should still fire a note. That is what
the sticky edge bit and the edge counters on both sides are for; if short triggers are missed
while long ones work, that mechanism is broken.

### 6. The default mapping does something

**Expect:** with a note sounding —

- **CV In 1** sweeps the pulse duty (timbre changes)
- **Audio In 1** detunes the second square — *turn CH2 on first*, VOICE page row 2
- **Audio In 2** brings in noise — *turn CH4 on first*, VOICE page row 4

*If Audio In 1/2 respond to movement but drift back to centre when you hold a steady voltage,*
those inputs are AC-coupled on this hardware and are modulation sources rather than CV sources.
Nothing to fix in firmware — re-point them at something where that suits (vibrato depth, wave
scan) on the MAP page, and keep the two CV jacks for anything that must hold still. The play
screen's input meters show this immediately.

### 7. The editor

**Expect:** `START` toggles PLAY ⇄ EDIT (LED 3 follows). `SELECT` cycles five pages, and
**LEDs 4+5 show the page number in binary**. D-pad moves and changes; L/R change by 8.

On the **MAP** page, re-point one input at a different destination and confirm it takes effect
immediately, with no rebuild. That is the whole architectural claim in one test.

### 8. Soak

Play for ten minutes. **LED 0 must never drop back to blinking**, and a note must never stick
on after the gate releases. The play screen has a `REJ` counter — words rejected by the tag or
check bits. On a healthy link **it should not move at all**; any steady climb is worth
reporting with the rate.

---

## The link diagnostics still work

The same payload still carries the characterisation screens, so nothing here has cost you the
ability to re-measure:

| Build | Question it answers |
|---|---|
| `diagnostics/mbrate.uf2` | which multiboot speed uploads *repeatably* (LED 3 = 100 kHz) |
| `diagnostics/linkrate.uf2` | how many words/s the live link sustains cleanly |
| `diagnostics/cablecheck.uf2` | which wire actually carries the GBA's SO |

## The one habit worth keeping

**When a measurement contradicts something already known to work, doubt the measurement
first.** Several days of this project went into faults that turned out to be in the
diagnostics rather than the hardware. `diagnostics/POSTMORTEM.md` records each one.
