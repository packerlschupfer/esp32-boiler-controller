#!/usr/bin/env python3
"""
Test script for PID auto-tuning feature via MQTT

This script tests the PID auto-tuning functionality by:
1. Starting auto-tuning
2. Monitoring progress
3. Displaying results
"""

import paho.mqtt.client as mqtt
import json
import time
import sys
import argparse

# MQTT broker settings
MQTT_BROKER = "localhost"
MQTT_PORT = 1883
MQTT_KEEPALIVE = 60

# Topics
TOPIC_COMMAND = "cmd/boiler/pid_autotune"
TOPIC_STATUS = "status/boiler/pid_autotune"
TOPIC_PROGRESS = "status/boiler/pid_autotune/progress"
TOPIC_RESULTS = "status/boiler/pid_autotune/results"

# Global state
autotune_state = "idle"
autotune_progress = 0
autotune_results = None


def on_connect(client, userdata, flags, rc):
    """Callback for when the client receives a CONNACK response from the server."""
    print(f"Connected with result code {rc}")
    
    # Subscribe to status topics
    client.subscribe(TOPIC_STATUS)
    client.subscribe(TOPIC_PROGRESS)
    client.subscribe(TOPIC_RESULTS)
    
    print("Subscribed to status topics")


def on_message(client, userdata, msg):
    """Callback for when a PUBLISH message is received from the server."""
    global autotune_state, autotune_progress, autotune_results
    
    topic = msg.topic
    payload = msg.payload.decode('utf-8')
    
    print(f"Received: {topic} -> {payload}")
    
    if topic == TOPIC_STATUS:
        autotune_state = payload
    elif topic == TOPIC_PROGRESS:
        try:
            data = json.loads(payload)
            autotune_progress = data.get('progress', 0)
            print(f"Progress: {autotune_progress}%")
        except json.JSONDecodeError:
            print(f"Failed to parse progress: {payload}")
    elif topic == TOPIC_RESULTS:
        try:
            autotune_results = json.loads(payload)
            print("\n=== AUTO-TUNING RESULTS ===")
            print(f"State: {autotune_results.get('state', 'unknown')}")
            print(f"Kp: {autotune_results.get('kp', 0):.3f}")
            print(f"Ki: {autotune_results.get('ki', 0):.6f}")
            print(f"Kd: {autotune_results.get('kd', 0):.3f}")
            print(f"Ultimate Gain (Ku): {autotune_results.get('ku', 0):.3f}")
            print(f"Ultimate Period (Tu): {autotune_results.get('tu', 0):.1f} seconds")
            print("===========================\n")
        except json.JSONDecodeError:
            print(f"Failed to parse results: {payload}")


def start_autotune(client):
    """Start PID auto-tuning"""
    print("\nStarting PID auto-tuning...")
    client.publish(TOPIC_COMMAND, "start")
    

def stop_autotune(client):
    """Stop PID auto-tuning"""
    print("\nStopping PID auto-tuning...")
    client.publish(TOPIC_COMMAND, "stop")


def check_status(client):
    """Check auto-tuning status"""
    print("\nChecking auto-tuning status...")
    client.publish(TOPIC_COMMAND, "status")


def monitor_autotune(client, timeout=600):
    """Monitor auto-tuning progress"""
    print(f"\nMonitoring auto-tuning (timeout: {timeout}s)...")
    print("Press Ctrl+C to stop monitoring\n")
    
    start_time = time.time()
    last_progress_time = time.time()
    
    try:
        while True:
            elapsed = time.time() - start_time
            
            # Check timeout
            if elapsed > timeout:
                print("\nAuto-tuning timeout reached!")
                stop_autotune(client)
                break
            
            # Request status update every 5 seconds
            if time.time() - last_progress_time > 5:
                check_status(client)
                last_progress_time = time.time()
            
            # Check if complete
            if autotune_state == "complete":
                print("\nAuto-tuning completed successfully!")
                break
            elif autotune_state == "failed":
                print("\nAuto-tuning failed!")
                break
            
            # Display status
            print(f"\rElapsed: {elapsed:.0f}s | State: {autotune_state} | Progress: {autotune_progress}%", 
                  end='', flush=True)
            
            time.sleep(1)
            
    except KeyboardInterrupt:
        print("\n\nMonitoring interrupted by user")
        return False
    
    return autotune_state == "complete"


def main():
    parser = argparse.ArgumentParser(description='Test PID auto-tuning via MQTT')
    parser.add_argument('--broker', default=MQTT_BROKER, help='MQTT broker address')
    parser.add_argument('--port', type=int, default=MQTT_PORT, help='MQTT broker port')
    parser.add_argument('--timeout', type=int, default=600, help='Auto-tuning timeout in seconds')
    parser.add_argument('--action', choices=['start', 'stop', 'status', 'monitor'], 
                        default='monitor', help='Action to perform')
    
    args = parser.parse_args()
    
    # Create MQTT client
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    
    # Connect to broker
    print(f"Connecting to MQTT broker at {args.broker}:{args.port}...")
    try:
        client.connect(args.broker, args.port, MQTT_KEEPALIVE)
    except Exception as e:
        print(f"Failed to connect: {e}")
        sys.exit(1)
    
    # Start the network loop
    client.loop_start()
    
    # Wait for connection
    time.sleep(2)
    
    # Perform action
    if args.action == 'start':
        start_autotune(client)
        time.sleep(2)
    elif args.action == 'stop':
        stop_autotune(client)
        time.sleep(2)
    elif args.action == 'status':
        check_status(client)
        time.sleep(2)
    elif args.action == 'monitor':
        # Start and monitor
        start_autotune(client)
        time.sleep(2)
        
        # Monitor progress
        success = monitor_autotune(client, args.timeout)
        
        if success and autotune_results:
            print("\nAuto-tuning completed successfully!")
            print("New PID parameters have been applied.")
        else:
            print("\nAuto-tuning did not complete successfully.")
    
    # Cleanup
    client.loop_stop()
    client.disconnect()
    
    print("\nTest completed.")


if __name__ == "__main__":
    main()