#!/usr/bin/env python3
"""
Test script to verify operation mode fix for BurnerControlTask
Tests that the system doesn't go into emergency shutdown when transitioning from STARTUP to NORMAL mode
"""

import paho.mqtt.client as mqtt
import json
import time
import sys
from datetime import datetime

# MQTT configuration
BROKER = "truenas.local"
PORT = 1883
USERNAME = "YOUR_MQTT_USER"
PASSWORD = "YOUR_MQTT_PASSWORD"
CLIENT_ID = "operation_mode_test"

# Topics
CONTROL_PREFIX = "boiler/control"
STATE_PREFIX = "boiler/state"
DIAGNOSTICS_PREFIX = "boiler/diagnostics"

# Track received messages
messages = []
startup_complete = False
error_detected = False
operation_mode = None

def timestamp():
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]

def on_connect(client, userdata, flags, rc):
    print(f"[{timestamp()}] Connected with result code {rc}")
    
    # Subscribe to all relevant topics
    topics = [
        f"{STATE_PREFIX}/system",
        f"{STATE_PREFIX}/burner",
        f"{DIAGNOSTICS_PREFIX}/errors",
        f"{DIAGNOSTICS_PREFIX}/sensors",
        f"{DIAGNOSTICS_PREFIX}/burner",
        f"{DIAGNOSTICS_PREFIX}/health"
    ]
    
    for topic in topics:
        client.subscribe(topic)
        print(f"[{timestamp()}] Subscribed to {topic}")

def on_message(client, userdata, msg):
    global startup_complete, error_detected, operation_mode
    
    try:
        # Try to parse as JSON
        payload = json.loads(msg.payload.decode())
        
        # Check for system state
        if msg.topic == f"{STATE_PREFIX}/system":
            if payload.get("burner", {}).get("state") == "OFF" and not payload.get("boilerEnabled"):
                print(f"[{timestamp()}] System in standby mode (boiler disabled)")
        
        # Check for burner state
        elif msg.topic == f"{STATE_PREFIX}/burner":
            state = payload.get("state", "UNKNOWN")
            mode = payload.get("mode", "UNKNOWN")
            if operation_mode != mode:
                operation_mode = mode
                print(f"[{timestamp()}] Burner operation mode: {mode}")
        
        # Check for errors
        elif msg.topic == f"{DIAGNOSTICS_PREFIX}/errors":
            errors = payload.get("errors", [])
            if errors:
                error_detected = True
                print(f"[{timestamp()}] ❌ ERROR DETECTED: {errors}")
                for error in errors:
                    if "No operation mode set" in str(error):
                        print(f"[{timestamp()}] ⚠️  CRITICAL: Operation mode error found!")
                    if "emergency" in str(error).lower():
                        print(f"[{timestamp()}] ⚠️  CRITICAL: Emergency shutdown detected!")
        
        # Check diagnostics
        elif msg.topic == f"{DIAGNOSTICS_PREFIX}/sensors":
            # Check if we've transitioned past startup
            fallback_mode = payload.get("fallback", {}).get("mode", "")
            if fallback_mode and fallback_mode != "STARTUP":
                startup_complete = True
                print(f"[{timestamp()}] Sensor fallback mode: {fallback_mode}")
        
        elif msg.topic == f"{DIAGNOSTICS_PREFIX}/health":
            status = payload.get("status", "UNKNOWN")
            if status != "HEALTHY":
                print(f"[{timestamp()}] Health status: {status}")
                
    except json.JSONDecodeError:
        # Handle non-JSON messages
        pass
    except Exception as e:
        print(f"[{timestamp()}] Error processing message: {e}")

def main():
    print("=" * 80)
    print("Operation Mode Fix Test")
    print("=" * 80)
    print("This test verifies that the system doesn't go into emergency shutdown")
    print("when transitioning from STARTUP to NORMAL mode without heating requests.")
    print("")
    print("Expected behavior:")
    print("1. System starts in STARTUP mode")
    print("2. After 15 seconds, transitions to NORMAL mode")
    print("3. NO emergency shutdown or 'No operation mode set' errors")
    print("4. System remains stable waiting for heating requests")
    print("=" * 80)
    
    # Create MQTT client
    client = mqtt.Client(client_id=CLIENT_ID)
    client.username_pw_set(USERNAME, PASSWORD)
    client.on_connect = on_connect
    client.on_message = on_message
    
    try:
        print(f"[{timestamp()}] Connecting to MQTT broker...")
        client.connect(BROKER, PORT, 60)
        
        # Start MQTT loop
        client.loop_start()
        
        # Wait for connection
        time.sleep(2)
        
        print(f"[{timestamp()}] Monitoring system for 30 seconds...")
        print(f"[{timestamp()}] Watching for startup transition at ~15 seconds...")
        
        # Monitor for 30 seconds
        start_time = time.time()
        last_status_time = start_time
        
        while time.time() - start_time < 30:
            elapsed = int(time.time() - start_time)
            
            # Print status every 5 seconds
            if time.time() - last_status_time >= 5:
                status = "✅ No errors" if not error_detected else "❌ Errors detected"
                print(f"[{timestamp()}] [{elapsed}s] Status: {status}")
                last_status_time = time.time()
            
            time.sleep(0.1)
        
        print("\n" + "=" * 80)
        print("TEST RESULTS")
        print("=" * 80)
        
        if error_detected:
            print("❌ TEST FAILED: Errors detected during operation")
            print("   The system should not generate errors when idle")
        else:
            print("✅ TEST PASSED: No errors detected")
            print("   System correctly handles NONE operation mode")
        
        if startup_complete:
            print("✅ Startup transition completed successfully")
        else:
            print("⚠️  Warning: Startup transition not detected")
        
        print("=" * 80)
        
    except Exception as e:
        print(f"[{timestamp()}] Error: {e}")
    finally:
        client.loop_stop()
        client.disconnect()

if __name__ == "__main__":
    main()