# Manual test mode — design note

Status: **DESIGN ONLY, nothing implemented.** Three decisions are open and marked below.
Written 2026-11-07.

## What this is for

Driving individual relays by hand on a live controller — to listen to a pump, check a valve
actually diverts, confirm wiring after work, or chase a fault that only shows under load.

## What exists already, and what it tells us

**A manual override did exist once.** `relays/+/control` in `src/main.cpp:530`, live in five
commits on 2025-06-13 in the `~/Documents/PlatformIO/git/esp32-boiler-controller` clone, then
commented out. It parsed a relay number 1–8 from the topic and set open/close event bits
directly, with **no interlocks in the handler at all**. The add-on's "Manual Relay Control"
card was built six months later against `cmd/relay/<name>` and never matched it, so those
buttons have never worked.

**The plumbing for a better version is already in the tree and dormant.**
`RelayCommandProcessor` has working branches for `BURNER_ENABLE/DISABLE`, `POWER_HALF/FULL`
and `WATER_MODE_ON/OFF` that nothing has ever set. `SystemEventsGenerated.h` additionally
declares `VALVE_OPEN/CLOSE` and `ALARM_ON/OFF`, handled nowhere. These were half of a uniform
request-bit protocol; only the pump half was adopted.

**Two bounded-window precedents to copy, not invent.** `bus_release` (clamp 30–900 s, expires
on-device, named refusal enum, the automatic path stands down) and `syslog_window` (same
shape, plus a re-arming always-on mode). An operator who knows one should not have to learn a
third idiom.

**The DELAY watchdog is an asset, not an obstacle.** Every energised relay auto-OFFs in 10 s
unless renewed, and the firmware already renews the desired mask every 2.5 s
(`RYN4_SET_TICKS = {1,6}`). So a manual relay set through the existing request bits is
renewed by machinery that already exists — and if the controller dies mid-test, the hardware
de-energises everything within 10 s on its own. That is a free dead-man's switch.

## The finding that shapes the design

**`RelayControlTask::setRelayState()` is close to a raw write.** It validates the index,
skips redundant commands, and applies pump-motor protection — and that is all. The real
interlocks (`checkEmergencyStop`, `checkTemperatureLimits`, `checkSystemErrors`) live in
`BurnerSystemController`, *above* the relay layer.

So a manual command routed through `RelayCommandProcessor → setRelayState` **inherits no
safety interlocks whatsoever**. It would reproduce the 2025 behaviour exactly. Any interlock
we want must be called explicitly, or the command must be routed through
`BurnerSystemController` instead.

This is the single most important thing to get right, and it is invisible unless you go
looking — `RelayCommandProcessor`'s own doc comment used to read as though it were a
fully-arbitrated control point.

## Proposed shape

```
boiler/cmd/system  test_mode [seconds]     enter (clamped, self-expiring, extends on re-issue)
boiler/cmd/system  test_mode_end           leave now
boiler/cmd/relay/<name>  on|off            only accepted while test mode is active
boiler/status/testmode                     {"active":true,"left":N,"tier":"safe"}
```

`TestModePolicy` as a pure header with native tests (like `BusReleasePolicy` and
`SyslogWindowPolicy`), `TestMode` as the runtime half.

### Entry refusals

Reuse `BusReleasePolicy`'s list almost verbatim, because the question is the same — is the
boiler quiescent enough to hand control over:

| refusal | why |
|---|---|
| `burner_active` | a firing relay is closed |
| `demand_present` | demand armed, ignition imminent |
| `pump_active` | heat is being dissipated; cutting flow traps it |
| `burner_sequencing` | mid purge/ignition/mode-switch |
| `emergency_latched` | operator is mid-diagnosis |
| `ota_active` | an update is running |
| `bus_released` | the RS485 bus belongs to another master, so relays are unreachable |

### While active

The automatic path must **stand down**, not compete. `BurnerStateMachine::setHeatDemand`
already takes `demandPermitted && !busReleased`; this adds `&& !testModeActive` at the same
point. Pumps need the equivalent in `PumpControlModule`, which is the part with no precedent
and the most likely place to get it wrong.

### Exit

On expiry or command: restore a safe desired mask (firing bits cleared, as
`BusReleasePolicy::safeDesiredMask` does), then release the stand-down so the control logic
re-decides from fresh readings. Never hand back with stale manual bits still set.

## OPEN DECISIONS — these are yours

**1. Do the safety interlocks apply?**
Not automatically — see the finding above. My recommendation: **yes, for the burner tier**,
by routing those commands through `BurnerSystemController` rather than the raw relay path, so
overtemp, emergency-stop and sensor-fault vetoes still bite. Test mode should make the
*controller* stand down, not the *safety system*. A test mode that can light the burner over
a failed sensor is a worse tool than no test mode.

**2. Which relays — one tier or two?**

- *Safe tier*: `HEATING_PUMP`, `WATER_PUMP`, `VALVE`, `ALARM`. Covers most commissioning and
  fault-finding, and nothing here can light a flame.
- *Hazardous tier*: `BURNER_ENABLE`, `POWER_BOOST`, `WATER_MODE`.

My recommendation: **build the safe tier first and ship it**, then decide on the hazardous
tier with the safe one in hand. The safe tier needs no interlock argument at all, which makes
it a much smaller thing to get right.

**3. What does the panel do?**
The add-on's relay card was removed today because it published to a topic nothing subscribed
to. If test mode ships, the card could come back — but it should then show the mode state and
refuse to send while inactive, rather than offering buttons that silently do nothing outside
a window. That is the HA peer's call, not firmware's.

## Deliberately NOT proposed

- **No permanent manual mode.** Bounded window only. A manual override with no deadline on a
  heating controller is the thing the DELAY watchdog exists to survive.
- **No raw relay-index addressing** (`relays/3/control`). Names, mapped through `RelayIndex`,
  so a hardware re-map cannot silently redirect a command. The 2025 version addressed by
  physical number.
- **No bypass of the DELAY watchdog renewal.** If the controller stops renewing, the relay
  drops. That property should survive test mode untouched.
