# Manual test mode — design note

Status: **SHIPPED 2026-11-12.** The three open decisions were answered by the owner on that
date: *no interlocks, low-level relays; one tier; panel yes.* Written 2026-11-07, rewritten on
delivery. Operator-facing documentation is in `docs/MQTT_API.md`; this note keeps the reasoning
and the things that are only visible if you go looking.

## What this is for

Driving individual relays by hand on a live controller — to listen to a pump, check a valve
actually diverts, confirm wiring after work, or chase a fault that only shows under load.

## What existed already, and what it told us

**A manual override did exist once.** `relays/+/control` in `src/main.cpp:530`, live in five
commits on 2025-06-13 in the `~/Documents/PlatformIO/git/esp32-boiler-controller` clone, then
commented out. It parsed a relay number 1–8 from the topic and set open/close event bits
directly, with **no interlocks in the handler at all**. The add-on's "Manual Relay Control"
card was built six months later against `cmd/relay/<name>` and never matched it, so those
buttons have never worked. Two lessons went into the shipped version: names rather than
numbers, and a reply on every command so a refusal is visible.

**Two bounded-window precedents to copy, not invent.** `bus_release` (clamp 30–900 s, expires
on-device, named refusal enum, the automatic path stands down) and `syslog_window` (same shape,
plus a re-arming always-on mode). An operator who knows one should not have to learn a third
idiom, so `TestModePolicy`/`TestMode` are deliberately the same two-file split with the same
function names.

**The DELAY watchdog is an asset, not an obstacle.** Every energised relay auto-OFFs in 10 s
unless renewed, and the firmware already renews the desired mask every 2.5 s. So a manual relay
is renewed by machinery that already exists — and if the controller dies mid-test, the hardware
de-energises everything within 10 s on its own. A free dead-man's switch, and it is the reason
a *bounded* manual mode is defensible at all.

## The finding that shaped the design

**`RelayControlTask::setRelayState()` is close to a raw write.** It validates the index, skips
redundant commands, and applies pump-motor protection — and that is all. The real interlocks
(`checkEmergencyStop`, `checkTemperatureLimits`, `checkSystemErrors`) live in
`BurnerSystemController`, *above* the relay layer. So a manual command routed through the relay
path inherits no safety interlocks whatsoever; it reproduces the 2025 behaviour exactly. This
is invisible unless you go looking — `RelayCommandProcessor`'s doc comment used to read as
though it were a fully-arbitrated control point.

Which made decision 1 real rather than rhetorical: interlocks had to be *added* to get them,
not *removed* to lose them. The owner chose not to add them.

## What shipped

```
boiler/cmd/system  test_mode [seconds]     enter (30–900, default 300, extends on re-issue)
boiler/cmd/system  test_mode_end           leave now
boiler/cmd/relay/<name>  on|off            accepted only while active
boiler/status/testmode                     state, and a reply per relay command
```

- `include/modules/diagnostics/TestModePolicy.h` — pure, 20 native tests.
- `src/modules/diagnostics/TestMode.{h,cpp}` — runtime state, modelled on `ModbusBusRelease`.
- `RelayControlTask::setRelayStateManual()` — the operator's write path.
- A **third** MQTT subscription, `boiler/cmd/relay/+`, because `boiler/cmd/+` matches one level
  only. That is the entire reason the add-on card never worked, and it is now in
  `failedSubscriptions` so a failure is retried rather than silently leaving the feature dead.

### The parts that were easy to get wrong

**The stand-down had to be three changes, not one.** A manual command that is not defended gets
reverted, which looks exactly like broken hardware:

1. `BurnerStateMachine::setHeatDemand` — one more `&&`, same gate `bus_release` uses.
2. `PumpControlModule` skips its reconcile block. This is the one that matters. The module is
   **not** edge-driven: `RelayCommandPolicy::pumpRequestResendDue()` re-sends its request
   whenever the relay bit disagrees with its own state, every 2000 ms. A manual pump command —
   in *either* direction — would be undone two seconds later, and no amount of blocking at the
   relay layer fixes it, because the module would just keep asking.
3. The relay layer refuses non-emergency **ON** commands, as belt and braces for any writer not
   enumerated above.

**Only ON is blocked, and that is load-bearing.** `CentralizedFailsafe` stops the burner with
*ordinary* `setRelayState()` calls — burner enable, power boost, water mode — not the emergency
setter. A blanket block at the relay layer would have disabled the overheat shutdown, turning
"bypass the control logic" into "bypass the safety system". An OFF cannot create a hazard, so
every OFF goes through. `ALARM` is exempt in both directions, because
`BurnerStateMachine` raises it on lockout *entry*, an edge that would never be re-raised.

**The exit mask is not zero.** Nothing in this firmware ever commands `VALVE`
(`SystemEventsGenerated` declares `VALVE_OPEN/CLOSE` and no handler exists), and `ALARM` is
written only on lockout edges. Zeroing them would leave a diverter closed or an alarm silent
until the next edge — a test mode leaving the boiler in a different state than it found it,
which is the one thing it must not do. So: restore the entry snapshot, force the firing bits
off regardless (`mayEnter` checks `actual`, and `desired` can legitimately lead it), and take
the pump bits **live**, because `PumpControlModule` reconciles those within 2 s anyway.

**Expiry runs on the 500 ms Modbus tick, not the publisher.** The MQTT publisher fires on the
60 s health cadence, so a 300 s window relying on it could hold live relays for 360. The
publisher is kept as a backstop for "the coordinator is stopped", the same gap the bus-release
backstop covers.

**The `test_mode_end` exclusion in the command parser.** `strncmp(payload, "test_mode", 9)`
matches `test_mode_end` too, so without the explicit exclusion the end command would be parsed
as an enter with the argument `"end"`, fail the `strtol`, and answer `bad_duration` instead of
ending the window.

## Deliberately NOT built

- **No permanent manual mode.** Bounded window only. A manual override with no deadline on a
  heating controller is the thing the DELAY watchdog exists to survive.
- **No raw relay-index addressing** (`relays/3/control`). See above.
- **No bypass of the DELAY watchdog renewal.** If the controller stops renewing, the relay
  drops. That property survives test mode untouched.
- **No persistence.** A reboot mid-window returns to full automatic control, and an OTA ends
  the window and restores the relays before the flash.

## Open

**The panel (decision 3 was "yes").** The add-on's relay card was removed in `ba8b675` because
it published to a topic nothing subscribed to. It can now come back **unchanged** — the topic
shape is identical, including the legacy `half_power` name. It should show the window state and
refuse to send while inactive, rather than offering buttons that do nothing outside a window.
That is the HA peer's call, not firmware's. The per-command reply on `boiler/status/testmode`
is there for exactly this: the card can show whether a click landed.

**Untested on hardware.** Everything above is reasoned from the code plus 20 native tests. The
first live window is the real test, and the thing to watch is whether the pump stand-down
behaves on handback — a manual pump command, then `test_mode_end`, then confirm
`PumpControlModule` puts the pump back within ~2 s without a flap.
