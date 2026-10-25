# Roadmap

Planned work that needs hardware, a season, or a decision - not a bug list. Defects live in
`docs/TECHNICAL_DEBT.md`; day-to-day items live in the session notes.

Delete an entry when it ships.

---

## 1. Read the boiler's error outputs (hardware prepared)

**Status:** the user has prepared a chip to read the voltages on the boiler's error outputs
and decode them into an error identity. Not yet wired or specified here.

### Why this is the top entry

The firmware is **structurally blind to flame loss**. `BurnerSafetyChecks::isFlameDetected()`
(`src/modules/control/BurnerSafetyChecks.cpp:16`) returns
`BurnerSystemController::isActive()` - that is, "the relay I closed is closed". It is a
tautology, and it is the only flame input the control loop has. Consequences:

- `BurnerStateMachine`'s `Reason::FLAME_LOST` branch and
  `SystemConstants::BURNER_FLAME_DETECTION_TIMEOUT_MS` (5 s) can never fire.
- The real `FlameDetection` class (`src/modules/safety/FlameDetection.h`) is **not
  registered** - `SystemResourceProvider::getFlameDetection()` returns `nullptr` by design.
- `docs/EQUIPMENT_SPECS.md` lists flame detection under "Missing Physical Sensors" with
  "using temperature rise as proxy". No such proxy is implemented either.

### The event that proved it matters (2026-10-24 10:43 DHW charge)

The burner stopped producing heat **112 s before the controller commanded it off**, and
nothing in the system noticed:

```
10:43:21  relay ON, stage 1 -> stage 2 at 10:43:51
10:46:29  LAST GAS PULSE, flow 62.0      <- flame out here
10:46:31  flow 62.8, still rising - afterheat only
10:48:11  flow peaks 66.7  (+4.7 K of afterheat, the normal signature)
10:48:21  controller commands OFF, believing it had been firing throughout
```

Across **169 burns over ten days** the tail between the last gas pulse and the relay dropping
is 0-13 s, with two exceptions - 26 s and this 113 s. During those 112 s the PID kept
integrating and the hysteresis policy kept answering "keep firing", because
`BurnerHysteresisPolicy`'s `wasOn` is the *commanded* level, not the flame.

Note the trap this also exposes: **a rising flow temperature does not prove the burner is
firing.** Every water burn in the dataset overshoots the flow-at-last-pulse by 3-5 K on
afterheat alone. Any temperature-rise flame proxy must be written against that baseline.

### Candidate cause, not yet distinguished

Two hypotheses fit, and the error outputs are what separates them:

1. **A hard ceiling near 62 C - most likely the boiler's own adjustable thermostat.** Over
   169 burns the two highest flow temperatures recorded *with the flame confirmed burning* are
   **62.1** (10-23 09:04) and **62.0** (10-24 10:43). Every other burn stopped at or below
   **60.9**. Those same two burns are also the only two with an anomalous tail (26 s and
   113 s); the third-longest in the set is 13 s. Two independent outliers landing on the same
   two burns is not coincidence.

   **And both are after `tempChargeDelta` went 10 -> 15 on 2026-10-23 01:09.** That raised the
   DHW target from `tank + 10` to `tank + 15`, i.e. up to 65.0 at the top of a charge with an
   off-threshold of 69.0 - for the first time, above the ceiling. Delta-10 charges stopped
   firing at 59.6-60.9 and never tripped anything in 169 burns.

   The spec sheet allows 90 C on that dial, so if this is the thermostat it is set far down.
   **Reading the dial settles it in one minute with no electronics at all** - do that first.
2. **Ionisation / flame-sensor failure**, as a one-off. No burn in 169 has ever shown a
   mid-burn gas pause and relight, which is what a thermostat with a narrow differential would
   produce - though a wide differential would not, since the flow stayed above 62 for the whole
   112 s on afterheat. This hypothesis is not excluded.

Overheat is ruled out at the boiler output sensor: the flow never exceeded 66.7, against a
110 C software ceiling and a 115 C interlock.

### Interim, software only

Two things are worth doing before any hardware lands:

- **Consider reverting `tempChargeDelta` to 10.** The draw-free comparison of 2026-10-24 says
  delta 15 buys nothing measurable - same burst count, same burst lengths - while it is what
  pushes the commanded target above the ceiling. User decision.
- **A gas-free-burn detector is possible without new hardware.** The boiler cannot be firing if
  the flow temperature stops accelerating while stage 2 is commanded. It must be written against
  the afterheat baseline (+3-5 K after flame-out, decelerating to a peak in 80-110 s), so the
  signal is the *second* derivative, not the first. Cheaper than the chip, and strictly worse -
  it cannot name the error.

### What the readout has to deliver

- Which error the boiler is reporting, as a value the firmware can publish and HA can record.
- Enough resolution to separate "operating thermostat opened" (normal, auto-reset) from
  "flame failure / lockout" (needs attention).
- A fast "is there a flame right now" signal, if the outputs carry one - that is what closes
  the `isFlameDetected()` tautology.

### Open questions for the hardware owner

- Which chip, and which boiler terminals - the error outputs are not in
  `docs/HARDWARE_SETUP.md` yet.
- Voltage ranges per error code, and whether the output is galvanically isolated from the
  240 V side.
- Whether it rides the existing RS485 bus (mind the contention - see the ANDRTF3 notes) or
  takes a spare ESP32 ADC channel.
- Where the boiler's adjustable thermostat dial actually sits. Reading it off the boiler
  would confirm or kill hypothesis 1 in one minute, with no electronics at all.

---

## 2. Mechanical frost thermostat around CH1

`FrostProtectionPolicy` (2026-10-23) covers "controller healthy but not heating". It cannot
cover a **dead controller**: `CH1 BURNER_ENABLE` is a normally-OPEN relay contact
(`docs/HARDWARE_SETUP.md:207,230`), so with the ESP32 unpowered, crashed or in emergency stop
the burner circuit is open and the boiler cannot fire at all. The original Vaillant controller
is not in the loop, and the STB and tandem thermostat can only ever *stop* the burner.

Closing this needs a mechanical frost thermostat wired around the relay. Not yet decided.

---

## 3. DHW flow meter

Without one, a hot-water draw during a tank charge cannot be quantified, which has already
voided one `tempChargeDelta` comparison (2026-10-23) and makes any gas-per-kelvin figure
suspect. The heuristic substitute - "the tank rises faster with the burner off, so water is
leaving" - was tested on a known draw-free charge and **false-positives on every charge this
boiler does**, because the boiler mass and the slow tank coil produce the same signature.
