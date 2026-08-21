# Hardware bring-up notes (89_GBA)

Status: **PAUSED** waiting on a better link cable. The firmware transport is proven good; the
blocker is electrical (bad cheap crossover cable / ground). Pick up here.

## FIRST, before touching firmware — verify the electrical layer

On the last attempt, a multimeter (reference = Computer GND) on the chopped cheap cable read:

| Line | Measured | Should be | Verdict |
|------|----------|-----------|---------|
| SC (clock) | **−2 V** | 0–3.3 V | ❌ negative = invalid/harmful |
| SI (MOSI)  | **+6 V** | 0–3.3 V at the GBA | ⚠️ Workshop pulse out swings ~5–6V; needs series R |
| SO (MISO)  | **+6 V** | ≤3.3 V (GBA drives it) | ❌ 6V here = back-drive, not from GBA |

These are out of the GBA's 0–3.3 V spec (−2 V is actively bad). **Do not run the GBA connected
until these are sane** — risk of damaging the console's serial pins.

Checklist to clear before any firmware work:

1. **Common ground.** Measure DC from Computer-GND to GBA-GND (e.g. headphone-jack sleeve).
   It MUST read ~0 V. Anything else (esp. a volt or two) = grounds aren't common, and *every*
   other reading is garbage. This is the #1 suspect. A proper GND wire through the cable
   (pin 6) is far better than borrowing the headphone sleeve.
2. **Series resistors.** Put ~1 kΩ in series on **SC and SI** (Workshop pulse outputs swing
   ~5–6 V; the GBA inputs are 3.3 V). The GBA's clamp diode + the resistor limits current.
   Do NOT put a resistor on SO (GBA drives that at 3.3 V into us — fine as-is).
3. With power off, continuity-check the cable end you kept: GBA socket **pin 2→SO, pin 3→SI,
   pin 5→SC, pin 6→GND**. Beware the link-cable crossover (the two ends swap pins 2/3). Trust
   the GBA-socket pin numbers, not wire colour.
4. Re-measure SC/SI/SO with the probe running: all should sit within 0–3.3 V (SI/SC after
   their series resistors), none negative.

## Operational gotcha (documented, real)

**Power the Computer (master) BEFORE turning on the GBA.** The GBA only syncs if the master is
already clocking when it powers up. Power-cycle the *GBA* (not the Computer) to retry.

## Firmware state (all PROVEN, don't re-derive)

- **Loopback passes** (jumper Pulse Out 2 → Pulse In 1, no GBA): flash `gba_loopback.uf2`,
  switch DOWN = PIO mode → LED0 solid = transport OK. (Switch UP "raw GPIO" mode races the
  ComputerCard HAL and gives false failures — ignore it; use PIO/DOWN.) Loopback blind spot:
  it only checks MOSI-invert XOR MISO-invert, so it can't catch each line's polarity alone.
- **Pull-up fix is in** `gba_multiboot.cpp`: after `pio_gpio_init` on the MISO pin, we
  re-`gpio_pull_up(GBA_MISO_PIN)` — the Pulse In 1 transistor input is dead without it.
- **Sample edge:** the reference uploaders (tangrs, jojolebarjos) sample MISO on the
  **leading (rising)** SCK edge. Our original `gba_spi` samples on the trailing edge (likely
  wrong); `gba_spi_cpha0` samples on the leading edge and was the only config returning
  structured bits from a real GBA. Once the cable is sane, sweep with `gba_probe.uf2` and if
  `cpha0` gives the clean 0x6202 echo (LED "echo good"), switch the applet's transport to the
  `gba_spi_cpha0` program + the winning SCK polarity.
- **Best sync test:** GBATEK — in normal mode the reply's LOW 16 bits echo the master's sent
  low-16, so sending 0x00006202 returns 0x????6202. That echo is a loopback *through the GBA*
  and confirms bit timing independent of recognition (upper 16 = 0x7202).

## UNRESOLVED: the applet and the diagnostics disagree about MISO polarity

Not a style difference — one of them is wrong, and it is on the axis loopback cannot see
(loopback only proves MOSI-invert XOR MISO-invert, so a matched pair of errors still passes):

| | `gpio_set_inover(GBA_MISO_PIN, …)` |
|---|---|
| `gba_multiboot.cpp:73` (the applet) | `GPIO_OVERRIDE_INVERT` |
| `gba_spi.pio` header derivation | `INVERT` — "Workshop inverts Pulse In 1 once" |
| `diagnostics/bringup.cpp:179` | **`GPIO_OVERRIDE_NORMAL`** |
| `diagnostics/gba_probe.cpp:97` | **`GPIO_OVERRIDE_NORMAL`** |

The diagnostics were the ones returning structured bits from a real GBA, so `NORMAL` is the
better-evidenced value — but that evidence was gathered over the bad cable, so it proves
little. **Resolve this on the scope before trusting either**: with the GBA connected and
idle, Pulse In 1's pad should read the GBA's actual SO line; compare the raw pad level
against what the PIO samples. Whichever value wins, make all four sites agree — a silent
disagreement here is exactly the shape of bug that survives a passing loopback.

## Diagnostics in this folder

- `bringup.cpp`   → `bringup.uf2` — **start here.** The staged tool: switch UP = signal
  generator (scope the raw output swing, GBA disconnected), MIDDLE = loopback, DOWN = live
  GBA handshake. Its file header documents the LED meaning per stage. Written 2026-07-28,
  builds clean, **never yet run on hardware**.
- `loopback.cpp`  → `gba_loopback.uf2` — transport self-test (no GBA). Superseded by
  `bringup` stage 2.
- `gba_probe.cpp` → `gba_probe.uf2` — live GBA handshake probe; sweeps edge/polarity/clock,
  can report the raw reply as CV1/CV2 voltages. Rebuild: configure once, then
  `cmake --build build`.

Build them all (paths as per the root `CLAUDE.md`):

```sh
cd diagnostics
cmake -G Ninja -B build -S . && cmake --build build
```

## Next session: an oscilloscope is now available

The blocker above was diagnosed with a multimeter, which cannot see edge quality, ringing or
a clock that only misbehaves at speed. With a scope, `bringup` stage 1 becomes the real first
test: it emits a slow anti-phase square on SC/SI plus a ~10 kHz burst so both the DC levels
and the edges can be judged directly.

A **browser diagnostic UI** (USB-MIDI SysEx + a `web/index.html`, as in `../WorkshopNibbleDrum`
and `../WorkshopZX`) is the natural next tool once the electrical layer is sane — it would
report the raw 32-bit reply word per attempt instead of encoding it into six LEDs, which is
the current bottleneck when reading stage 3. Not started; no USB code exists in this repo yet.
