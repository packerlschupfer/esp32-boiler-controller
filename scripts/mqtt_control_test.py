#!/usr/bin/env python3
"""
MQTT Control Test for ESPlan Boiler Controller
Interactive test script for verifying MQTT remote control functionality
"""

import paho.mqtt.client as mqtt
import json
import time
import sys
from datetime import datetime

# MQTT Configuration
MQTT_HOST = "192.168.20.27"
MQTT_PORT = 1883
MQTT_USER = "YOUR_MQTT_USER"
MQTT_PASS = "YOUR_MQTT_PASSWORD"
MQTT_CLIENT_ID = "boiler_test_client"

# Topic configuration
TOPIC_PREFIX = "cmd/boiler"
STATE_PREFIX = f"{TOPIC_PREFIX}/state"
DIAG_PREFIX = f"{TOPIC_PREFIX}/diagnostics"
PARAM_PREFIX = f"{TOPIC_PREFIX}/params"

class BoilerControlTest:
    def __init__(self):
        self.client = mqtt.Client(client_id=MQTT_CLIENT_ID)
        self.client.username_pw_set(MQTT_USER, MQTT_PASS)
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        self.client.on_disconnect = self.on_disconnect
        self.connected = False
        self.test_results = {}
        
    def on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            print(f"✓ Connected to MQTT broker at {MQTT_HOST}")
            self.connected = True
            # Subscribe to all boiler topics
            topics = [
                (f"{TOPIC_PREFIX}/#", 1),
                (f"{STATE_PREFIX}/#", 1),
                (f"{DIAG_PREFIX}/#", 1),
                (f"{PARAM_PREFIX}/#", 1)
            ]
            for topic, qos in topics:
                client.subscribe(topic, qos)
                print(f"  Subscribed to: {topic}")
        else:
            print(f"✗ Failed to connect, return code: {rc}")
            
    def on_disconnect(self, client, userdata, rc):
        print(f"Disconnected from MQTT broker (rc: {rc})")
        self.connected = False
        
    def on_message(self, client, userdata, msg):
        timestamp = datetime.now().strftime("%H:%M:%S")
        topic = msg.topic
        try:
            # Try to parse as JSON
            payload = json.loads(msg.payload.decode())
            print(f"[{timestamp}] {topic}: {json.dumps(payload, indent=2)}")
        except:
            # Plain text message
            payload = msg.payload.decode()
            print(f"[{timestamp}] {topic}: {payload}")
            
        # Store response for test validation
        if topic not in self.test_results:
            self.test_results[topic] = []
        self.test_results[topic].append(payload)
    
    def connect(self):
        print(f"Connecting to MQTT broker at {MQTT_HOST}:{MQTT_PORT}...")
        try:
            self.client.connect(MQTT_HOST, MQTT_PORT, 60)
            self.client.loop_start()
            # Wait for connection
            timeout = 5
            while not self.connected and timeout > 0:
                time.sleep(0.5)
                timeout -= 0.5
            return self.connected
        except Exception as e:
            print(f"Connection error: {e}")
            return False
    
    def send_command(self, topic, payload, description=""):
        if not self.connected:
            print("✗ Not connected to MQTT broker")
            return False
            
        full_topic = topic if topic.startswith(TOPIC_PREFIX) else f"{TOPIC_PREFIX}/{topic}"
        
        print(f"\n→ {description}" if description else f"\n→ Sending command")
        print(f"  Topic: {full_topic}")
        print(f"  Payload: {payload}")
        
        try:
            result = self.client.publish(full_topic, payload, qos=1, retain=False)
            if result.rc == mqtt.MQTT_ERR_SUCCESS:
                print("  ✓ Command sent successfully")
                return True
            else:
                print(f"  ✗ Failed to send command (rc: {result.rc})")
                return False
        except Exception as e:
            print(f"  ✗ Error sending command: {e}")
            return False
    
    def wait_for_response(self, timeout=3):
        """Wait for responses after sending a command"""
        print(f"  Waiting {timeout}s for responses...")
        time.sleep(timeout)
    
    def run_basic_tests(self):
        """Run basic control tests"""
        print("\n" + "="*60)
        print("BASIC CONTROL TESTS")
        print("="*60)
        
        tests = [
            # (topic, payload, description)
            ("enable", "on", "Enable boiler system"),
            ("heating", "on", "Enable space heating"),
            ("water", "on", "Enable water heating"),
            ("water/priority", "on", "Enable water heating priority"),
            ("water/priority", "off", "Disable water heating priority"),
            ("water", "off", "Disable water heating"),
            ("heating", "off", "Disable space heating"),
            ("enable", "off", "Disable boiler system"),
        ]
        
        for topic, payload, desc in tests:
            if self.send_command(topic, payload, desc):
                self.wait_for_response(2)
            else:
                print("✗ Test failed")
    
    def run_parameter_tests(self):
        """Test parameter management"""
        print("\n" + "="*60)
        print("PARAMETER MANAGEMENT TESTS")
        print("="*60)
        
        # Request parameter list
        self.send_command("params/list", "", "Request parameter list")
        self.wait_for_response(3)
        
        # Test setting parameters
        params = [
            ("targetTemperatureInside", "22.0", "Set target inside temperature to 22°C"),
            ("heating_hysteresis", "2.5", "Set heating hysteresis to 2.5°C"),
            ("wHeaterConfTempLimitLow", "45.0", "Set water heater low limit to 45°C"),
            ("wHeaterConfTempLimitHigh", "60.0", "Set water heater high limit to 60°C"),
        ]
        
        for param, value, desc in params:
            self.send_command(f"params/set/{param}", value, desc)
            self.wait_for_response(1)
        
        # Save parameters
        self.send_command("params/save", "", "Save parameters to NVS")
        self.wait_for_response(2)
    
    def run_diagnostic_tests(self):
        """Test diagnostic requests"""
        print("\n" + "="*60)
        print("DIAGNOSTIC TESTS")
        print("="*60)
        
        diag_types = [
            ("health", "Request health diagnostics"),
            ("memory", "Request memory diagnostics"),
            ("tasks", "Request task diagnostics"),
            ("sensors", "Request sensor diagnostics"),
            ("burner", "Request burner diagnostics"),
            ("errors", "Request error diagnostics"),
        ]
        
        for diag_type, desc in diag_types:
            self.send_command(f"diagnostics/{diag_type}", "", desc)
            self.wait_for_response(2)
    
    def run_stress_test(self):
        """Run rapid command stress test"""
        print("\n" + "="*60)
        print("STRESS TEST - Rapid Commands")
        print("="*60)
        
        print("Sending 10 rapid on/off commands...")
        for i in range(10):
            self.send_command("heating", "on" if i % 2 == 0 else "off", f"Rapid test {i+1}")
            time.sleep(0.5)
        
        print("\nWaiting for system to stabilize...")
        self.wait_for_response(5)
    
    def print_summary(self):
        """Print test summary"""
        print("\n" + "="*60)
        print("TEST SUMMARY")
        print("="*60)
        
        print(f"\nReceived responses on {len(self.test_results)} topics:")
        for topic in sorted(self.test_results.keys()):
            print(f"  • {topic}: {len(self.test_results[topic])} messages")
    
    def run_all_tests(self):
        """Run all tests"""
        if not self.connect():
            print("✗ Failed to connect to MQTT broker")
            return
        
        try:
            # Let subscriptions settle
            time.sleep(2)
            
            # Run test suites
            self.run_basic_tests()
            self.run_parameter_tests()
            self.run_diagnostic_tests()
            self.run_stress_test()
            
            # Final summary
            self.print_summary()
            
        except KeyboardInterrupt:
            print("\n\nTest interrupted by user")
        except Exception as e:
            print(f"\n✗ Test error: {e}")
        finally:
            print("\nDisconnecting...")
            self.client.loop_stop()
            self.client.disconnect()

def main():
    print("ESPlan Boiler Controller - MQTT Remote Control Test")
    print("="*60)
    
    tester = BoilerControlTest()
    tester.run_all_tests()
    
    print("\n✓ Test complete")

if __name__ == "__main__":
    main()