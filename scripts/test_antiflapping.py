#!/usr/bin/env python3
"""Observe burner anti-flapping behaviour from the live relay bitmap.

Rewritten 2026-10-16. The previous version had never worked: every topic was
written with the prefix inverted ("status/boiler/state/burner" instead of
"boiler/status/state/burner"), so it neither received nor commanded anything,
and it read a "power_level" field from a topic the firmware does not publish.
It also rapid-toggled a real gas boiler under its default --test all.

What the firmware actually publishes is boiler/status/sensors every 10 s, with
the relays packed into one byte (MQTTPublisher.cpp:168):

    bit 0 (0x01)  burner enable      bit 3 (0x08)  power boost (FULL)
    bit 1 (0x02)  heating pump       bit 4 (0x10)  water mode
    bit 2 (0x04)  water pump

The burner fires in two distinct relay modes (BurnerSystemController truth
table): HEATING is bit0 set with bit4 clear, WATER is bit0 *clear* with bit4
set. So burner-on is bit0 OR bit4 - watching bit0 alone silently misses every
hot-water charge.

Default mode is observe-only: it commands nothing and simply reports whether
the anti-flapping floors are respected over the sampling window. The 10 s
publish interval bounds timing resolution, so only violations larger than one
sample are trustworthy; a reported on-time of 20 s may be anywhere in 10-30 s.

Credentials come from MQTT_USERNAME / MQTT_PASSWORD in the environment.
"""

import argparse
import json
import os
import sys
import time
from datetime import datetime

import paho.mqtt.client as mqtt

TOPIC_SENSORS = "boiler/status/sensors"
TOPIC_HEATING_CMD = "boiler/cmd/heating"

# Mirrors SystemConstants.h:373-375. Kept in sync by hand - the firmware does
# not publish these, so a mismatch here reports phantom violations.
MIN_ON_TIME_S = 120.0
MIN_OFF_TIME_S = 20.0
MIN_POWER_CHANGE_INTERVAL_S = 15.0

RELAY_BURNER_ENABLE = 0x01
RELAY_POWER_BOOST = 0x08
RELAY_WATER_MODE = 0x10


def log(message):
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {message}", flush=True)


class FlapMonitor:
    """Tracks burner on/off and power-level dwell times against the floors."""

    def __init__(self):
        self.burner_on = None
        self.full_power = None
        self.last_burner_change = None
        self.last_power_change = None
        self.violations = []
        self.samples = 0
        self.transitions = 0

    def observe(self, relay_bits, now):
        self.samples += 1
        burner_on = bool(relay_bits & (RELAY_BURNER_ENABLE | RELAY_WATER_MODE))
        mode = "WATER" if relay_bits & RELAY_WATER_MODE else "HEATING"
        full_power = bool(relay_bits & RELAY_POWER_BOOST)

        if self.burner_on is None:
            self.burner_on = burner_on
            self.full_power = full_power
            self.last_burner_change = now
            self.last_power_change = now
            log(f"Initial state: burner {'ON' if burner_on else 'OFF'}"
                f"{' (' + mode + ')' if burner_on else ''}"
                f", power {'FULL' if full_power else 'HALF'}, r=0x{relay_bits:02x}")
            return

        if burner_on != self.burner_on:
            self.transitions += 1
            dwell = now - self.last_burner_change
            # The floor that applies is the one for the state being LEFT.
            floor = MIN_ON_TIME_S if self.burner_on else MIN_OFF_TIME_S
            leaving = "ON" if self.burner_on else "OFF"
            detail = (f"Burner {leaving} -> {'ON' if burner_on else 'OFF'} "
                      f"after {dwell:.0f}s (floor {floor:.0f}s)")
            if dwell + 10.0 < floor:
                # +10 s: one publish interval of measurement slack.
                self.violations.append(f"min {leaving.lower()} time: {detail}")
                log(f"VIOLATION {detail}")
            else:
                log(detail + (f" [{mode}]" if burner_on else ""))
            self.burner_on = burner_on
            self.last_burner_change = now

        if full_power != self.full_power:
            dwell = now - self.last_power_change
            detail = (f"Power {'HALF' if self.full_power else 'FULL'} -> "
                      f"{'FULL' if full_power else 'HALF'} after {dwell:.0f}s "
                      f"(floor {MIN_POWER_CHANGE_INTERVAL_S:.0f}s)")
            if dwell + 10.0 < MIN_POWER_CHANGE_INTERVAL_S:
                self.violations.append(f"min power interval: {detail}")
                log(f"VIOLATION {detail}")
            else:
                log(detail)
            self.full_power = full_power
            self.last_power_change = now

    def report(self):
        print("=" * 60)
        print(f"Samples: {self.samples}   burner transitions: {self.transitions}")
        if not self.samples:
            print("No samples received - is boiler/status/sensors publishing?")
        elif self.violations:
            print(f"{len(self.violations)} violation(s):")
            for v in self.violations:
                print(f"  - {v}")
        else:
            print("No anti-flapping violations observed.")
        print("=" * 60)
        return 1 if self.violations else 0


def main():
    parser = argparse.ArgumentParser(
        description="Observe burner anti-flapping from the relay bitmap")
    parser.add_argument("--broker", default="192.168.20.27")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--duration", type=int, default=1800,
                        help="observation window in seconds (default 1800)")
    parser.add_argument("--active", action="store_true",
                        help="also COMMAND the boiler: toggle the room target to "
                             "provoke transitions. Fires a real gas boiler - "
                             "requires --i-know-this-runs-the-boiler.")
    parser.add_argument("--i-know-this-runs-the-boiler", action="store_true",
                        dest="confirmed")
    parser.add_argument("--cycles", type=int, default=3,
                        help="toggle cycles when --active (default 3)")
    args = parser.parse_args()

    if args.active and not args.confirmed:
        parser.error("--active needs --i-know-this-runs-the-boiler")

    user = os.environ.get("MQTT_USERNAME")
    password = os.environ.get("MQTT_PASSWORD")
    if not user or not password:
        print("Set MQTT_USERNAME and MQTT_PASSWORD in the environment.",
              file=sys.stderr)
        return 2

    monitor = FlapMonitor()

    def on_connect(client, userdata, flags, rc):
        if rc != 0:
            log(f"Connect failed, rc={rc}")
            return
        client.subscribe(TOPIC_SENSORS)
        log(f"Connected, subscribed to {TOPIC_SENSORS}")

    def on_message(client, userdata, msg):
        try:
            data = json.loads(msg.payload.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return
        relay_bits = data.get("r")
        if isinstance(relay_bits, int):
            monitor.observe(relay_bits, time.monotonic())

    client = mqtt.Client()
    client.username_pw_set(user, password)
    client.on_connect = on_connect
    client.on_message = on_message
    try:
        client.connect(args.broker, args.port, 60)
    except OSError as exc:
        log(f"Failed to connect: {exc}")
        return 2
    client.loop_start()

    try:
        if args.active:
            # Provoke transitions through the normal demand path rather than
            # driving relays directly: the controller's own anti-flapping logic
            # is what is under test, so it must stay in the loop.
            log(f"Active mode: {args.cycles} room-target toggle cycles")
            for cycle in range(args.cycles):
                log(f"Cycle {cycle + 1}: target 23.0 (demand ON)")
                client.publish(TOPIC_HEATING_CMD, "23.0")
                time.sleep(180)
                log(f"Cycle {cycle + 1}: target 15.0 (demand OFF)")
                client.publish(TOPIC_HEATING_CMD, "15.0")
                time.sleep(180)
            log("Active cycles done - restore the room target yourself; this "
                "script does not know what it was before.")
        else:
            log(f"Observing for {args.duration}s (Ctrl-C to stop early)")
            time.sleep(args.duration)
    except KeyboardInterrupt:
        log("Interrupted")
    finally:
        client.loop_stop()
        client.disconnect()

    return monitor.report()


if __name__ == "__main__":
    sys.exit(main())
