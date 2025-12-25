#!/usr/bin/env python3
"""
Test script for burner anti-flapping functionality

This script tests the anti-flapping mechanisms by:
1. Rapidly toggling heating on/off
2. Monitoring burner state transitions
3. Verifying minimum on/off times are enforced
"""

import paho.mqtt.client as mqtt
import json
import time
import sys
import argparse
from datetime import datetime

# MQTT broker settings
MQTT_BROKER = "localhost"
MQTT_PORT = 1883
MQTT_KEEPALIVE = 60

# Topics
TOPIC_HEATING_CMD = "cmd/boiler/heating"
TOPIC_WATER_CMD = "cmd/boiler/water"
TOPIC_BURNER_STATUS = "status/boiler/state/burner"
TOPIC_SYSTEM_STATUS = "status/boiler/state/system"
TOPIC_DIAGNOSTICS = "status/boiler/diagnostics/burner"

# State tracking
burner_state = "OFF"
burner_on_time = None
burner_off_time = None
power_level = "OFF"
last_power_change = None
test_results = []

def log_event(message):
    """Log event with timestamp"""
    timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    print(f"[{timestamp}] {message}")
    test_results.append(f"[{timestamp}] {message}")

def on_connect(client, userdata, flags, rc):
    """Callback for when the client receives a CONNACK response from the server."""
    log_event(f"Connected with result code {rc}")
    
    # Subscribe to status topics
    client.subscribe(TOPIC_BURNER_STATUS)
    client.subscribe(TOPIC_SYSTEM_STATUS)
    client.subscribe(TOPIC_DIAGNOSTICS)
    
    log_event("Subscribed to status topics")

def on_message(client, userdata, msg):
    """Callback for when a PUBLISH message is received from the server."""
    global burner_state, burner_on_time, burner_off_time, power_level, last_power_change
    
    topic = msg.topic
    payload = msg.payload.decode('utf-8')
    
    if topic == TOPIC_BURNER_STATUS:
        try:
            data = json.loads(payload)
            new_state = data.get('state', 'UNKNOWN')
            new_power = data.get('power_level', 'OFF')
            
            # Check for state changes
            if new_state != burner_state:
                if burner_state == "OFF" and new_state != "OFF":
                    # Burner turned on
                    burner_on_time = time.time()
                    if burner_off_time:
                        off_duration = burner_on_time - burner_off_time
                        log_event(f"Burner ON after {off_duration:.1f}s off (min: 60s)")
                        if off_duration < 60:
                            log_event("⚠️  VIOLATION: Burner turned on too quickly!")
                elif burner_state != "OFF" and new_state == "OFF":
                    # Burner turned off
                    burner_off_time = time.time()
                    if burner_on_time:
                        on_duration = burner_off_time - burner_on_time
                        log_event(f"Burner OFF after {on_duration:.1f}s on (min: 120s)")
                        if on_duration < 120:
                            log_event("⚠️  VIOLATION: Burner turned off too quickly!")
                
                log_event(f"State: {burner_state} -> {new_state}")
                burner_state = new_state
            
            # Check for power level changes
            if new_power != power_level and new_power != "OFF" and power_level != "OFF":
                now = time.time()
                if last_power_change:
                    change_interval = now - last_power_change
                    log_event(f"Power: {power_level} -> {new_power} after {change_interval:.1f}s (min: 30s)")
                    if change_interval < 30:
                        log_event("⚠️  VIOLATION: Power level changed too quickly!")
                else:
                    log_event(f"Power: {power_level} -> {new_power}")
                
                last_power_change = now
                power_level = new_power
            elif new_power != power_level:
                power_level = new_power
                
        except json.JSONDecodeError:
            pass
    
    elif topic == TOPIC_DIAGNOSTICS:
        # Can parse additional diagnostics if needed
        pass

def rapid_toggle_test(client, cycles=5, interval=10):
    """Test rapid on/off toggling"""
    log_event(f"\n=== RAPID TOGGLE TEST ({cycles} cycles, {interval}s interval) ===")
    
    for i in range(cycles):
        # Turn heating on
        log_event(f"\nCycle {i+1}: Requesting heating ON")
        client.publish(TOPIC_HEATING_CMD, "on")
        time.sleep(interval)
        
        # Turn heating off
        log_event(f"Cycle {i+1}: Requesting heating OFF")
        client.publish(TOPIC_HEATING_CMD, "off")
        time.sleep(interval)
    
    log_event("\nRapid toggle test complete")

def power_change_test(client):
    """Test power level changes"""
    log_event("\n=== POWER CHANGE TEST ===")
    
    # Ensure heating is on
    log_event("Ensuring heating is ON")
    client.publish(TOPIC_HEATING_CMD, "on")
    time.sleep(5)
    
    # Wait for burner to stabilize
    log_event("Waiting for burner to start...")
    time.sleep(30)
    
    # Try to force power changes by toggling water heating
    log_event("\nToggling water heating to force power changes")
    for i in range(3):
        log_event(f"\nCycle {i+1}: Water heating ON (should increase power)")
        client.publish(TOPIC_WATER_CMD, "on")
        time.sleep(15)
        
        log_event(f"Cycle {i+1}: Water heating OFF (should decrease power)")
        client.publish(TOPIC_WATER_CMD, "off")
        time.sleep(15)
    
    # Turn everything off
    log_event("\nTurning everything OFF")
    client.publish(TOPIC_HEATING_CMD, "off")
    client.publish(TOPIC_WATER_CMD, "off")
    
    log_event("\nPower change test complete")

def minimum_runtime_test(client):
    """Test minimum runtime enforcement"""
    log_event("\n=== MINIMUM RUNTIME TEST ===")
    
    # Turn on and try to turn off quickly
    log_event("Turning heating ON")
    client.publish(TOPIC_HEATING_CMD, "on")
    
    # Try to turn off after 30 seconds (should be blocked)
    time.sleep(30)
    log_event("Attempting to turn OFF after 30s (should be delayed)")
    client.publish(TOPIC_HEATING_CMD, "off")
    
    # Wait and observe
    time.sleep(100)
    
    log_event("\nMinimum runtime test complete")

def generate_report():
    """Generate test report"""
    log_event("\n" + "="*50)
    log_event("TEST REPORT")
    log_event("="*50)
    
    violations = [r for r in test_results if "VIOLATION" in r]
    if violations:
        log_event(f"\n⚠️  Found {len(violations)} violations:")
        for v in violations:
            print(v)
    else:
        log_event("\n✅ No violations detected - anti-flapping working correctly")
    
    log_event("\nTest complete!")

def main():
    parser = argparse.ArgumentParser(description='Test burner anti-flapping mechanisms')
    parser.add_argument('--broker', default=MQTT_BROKER, help='MQTT broker address')
    parser.add_argument('--port', type=int, default=MQTT_PORT, help='MQTT broker port')
    parser.add_argument('--test', choices=['rapid', 'power', 'runtime', 'all'], 
                        default='all', help='Test to run')
    
    args = parser.parse_args()
    
    # Create MQTT client
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    
    # Connect to broker
    log_event(f"Connecting to MQTT broker at {args.broker}:{args.port}...")
    try:
        client.connect(args.broker, args.port, MQTT_KEEPALIVE)
    except Exception as e:
        log_event(f"Failed to connect: {e}")
        sys.exit(1)
    
    # Start the network loop
    client.loop_start()
    
    # Wait for connection
    time.sleep(2)
    
    # Ensure system is enabled
    log_event("Enabling boiler system")
    client.publish("cmd/boiler/enable", "on")
    time.sleep(2)
    
    # Run selected tests
    try:
        if args.test == 'rapid' or args.test == 'all':
            rapid_toggle_test(client)
            if args.test == 'all':
                time.sleep(120)  # Wait for system to stabilize
        
        if args.test == 'power' or args.test == 'all':
            power_change_test(client)
            if args.test == 'all':
                time.sleep(120)  # Wait for system to stabilize
        
        if args.test == 'runtime' or args.test == 'all':
            minimum_runtime_test(client)
        
        # Wait for final messages
        time.sleep(10)
        
    except KeyboardInterrupt:
        log_event("\nTest interrupted by user")
    
    # Generate report
    generate_report()
    
    # Cleanup
    client.loop_stop()
    client.disconnect()

if __name__ == "__main__":
    main()