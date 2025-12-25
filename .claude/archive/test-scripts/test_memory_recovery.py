#!/usr/bin/env python3
"""
Test script for MQTT Diagnostics Memory Recovery
Simulates low memory conditions and verifies recovery behavior
"""

import paho.mqtt.client as mqtt
import time
import json
from datetime import datetime

# MQTT Configuration
MQTT_HOST = "192.168.16.16"
MQTT_PORT = 1883
MQTT_USER = "YOUR_MQTT_USER"
MQTT_PASS = "YOUR_MQTT_PASSWORD"
TOPIC_PREFIX = "cmd/boiler"

class MemoryRecoveryTest:
    def __init__(self):
        self.client = mqtt.Client(client_id="memory_test_client")
        self.client.username_pw_set(MQTT_USER, MQTT_PASS)
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        self.diagnostics_received = {}
        self.test_phase = "initial"
        
    def on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            print(f"✓ Connected to MQTT broker")
            # Subscribe to diagnostics topics
            client.subscribe(f"{TOPIC_PREFIX}/diagnostics/+", 1)
            client.subscribe(f"{TOPIC_PREFIX}/diagnostics/memory/response", 1)
            client.subscribe(f"{TOPIC_PREFIX}/diagnostics/health/response", 1)
        else:
            print(f"✗ Failed to connect: {rc}")
            
    def on_message(self, client, userdata, msg):
        topic = msg.topic
        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        
        # Track diagnostic message timing
        if "diagnostics" in topic:
            diag_type = topic.split("/")[-1].replace("/response", "")
            if diag_type not in self.diagnostics_received:
                self.diagnostics_received[diag_type] = []
            self.diagnostics_received[diag_type].append({
                "time": timestamp,
                "phase": self.test_phase
            })
            
            # Parse memory diagnostics
            if "memory" in topic:
                try:
                    data = json.loads(msg.payload.decode())
                    free_heap = data.get("free_heap", 0)
                    min_free = data.get("min_free_heap", 0)
                    largest_block = data.get("largest_free_block", 0)
                    fragmentation = data.get("fragmentation", 0)
                    
                    print(f"[{timestamp}] Memory Status:")
                    print(f"  Free: {free_heap/1024:.1f}KB")
                    print(f"  Min Free: {min_free/1024:.1f}KB") 
                    print(f"  Largest Block: {largest_block/1024:.1f}KB")
                    print(f"  Fragmentation: {fragmentation:.1f}%")
                except:
                    pass
    
    def run_test(self):
        print("\n=== MQTT Diagnostics Memory Recovery Test ===\n")
        
        if not self.client.connect(MQTT_HOST, MQTT_PORT, 60):
            print("Failed to connect")
            return
            
        self.client.loop_start()
        time.sleep(2)  # Let connection settle
        
        # Phase 1: Baseline - Normal diagnostic frequency
        print("\n--- Phase 1: Baseline Measurements (30s) ---")
        self.test_phase = "baseline"
        self.diagnostics_received.clear()
        
        # Request diagnostics every 5 seconds for 30 seconds
        for i in range(6):
            print(f"\n[{i*5}s] Requesting diagnostics...")
            self.client.publish(f"{TOPIC_PREFIX}/diagnostics/memory", "", qos=1)
            self.client.publish(f"{TOPIC_PREFIX}/diagnostics/health", "", qos=1)
            time.sleep(5)
        
        # Count baseline frequency
        baseline_memory_count = len(self.diagnostics_received.get("memory", []))
        baseline_health_count = len(self.diagnostics_received.get("health", []))
        print(f"\nBaseline: {baseline_memory_count} memory, {baseline_health_count} health messages")
        
        # Phase 2: Simulate memory pressure
        print("\n--- Phase 2: Simulating Memory Pressure ---")
        self.test_phase = "pressure"
        print("Note: In real deployment, MemoryGuard would trigger at <10% free heap")
        print("Diagnostics should be throttled or suspended...")
        
        # The actual memory pressure would be triggered internally
        # Here we just monitor the response
        time.sleep(5)
        
        # Phase 3: Recovery period - Check reduced frequency
        print("\n--- Phase 3: Recovery Period (40s) ---")
        self.test_phase = "recovery"
        self.diagnostics_received.clear()
        
        # Monitor for 40 seconds (should see reduced frequency)
        for i in range(8):
            print(f"\n[{i*5}s] Monitoring diagnostic frequency...")
            self.client.publish(f"{TOPIC_PREFIX}/diagnostics/memory", "", qos=1)
            time.sleep(5)
        
        recovery_memory_count = len(self.diagnostics_received.get("memory", []))
        print(f"\nRecovery: {recovery_memory_count} memory messages (should be less frequent)")
        
        # Phase 4: Wait for normal operation restore (30s timer)
        print("\n--- Phase 4: Waiting for Normal Operation Restore ---")
        self.test_phase = "restored"
        print("Diagnostics should resume normal frequency after 30s timer...")
        time.sleep(35)  # Wait for timer to expire
        
        # Phase 5: Verify normal operation restored
        print("\n--- Phase 5: Verifying Normal Operation (30s) ---")
        self.diagnostics_received.clear()
        
        for i in range(6):
            print(f"\n[{i*5}s] Checking restored frequency...")
            self.client.publish(f"{TOPIC_PREFIX}/diagnostics/memory", "", qos=1)
            self.client.publish(f"{TOPIC_PREFIX}/diagnostics/health", "", qos=1)
            time.sleep(5)
        
        restored_memory_count = len(self.diagnostics_received.get("memory", []))
        restored_health_count = len(self.diagnostics_received.get("health", []))
        print(f"\nRestored: {restored_memory_count} memory, {restored_health_count} health messages")
        
        # Summary
        print("\n=== Test Summary ===")
        print(f"Baseline frequency: {baseline_memory_count} messages in 30s")
        print(f"Recovery frequency: {recovery_memory_count} messages in 40s")
        print(f"Restored frequency: {restored_memory_count} messages in 30s")
        
        if recovery_memory_count < baseline_memory_count:
            print("✓ Diagnostic throttling during recovery: PASSED")
        else:
            print("✗ Diagnostic throttling during recovery: FAILED")
            
        if restored_memory_count >= baseline_memory_count - 1:
            print("✓ Normal operation restore: PASSED")
        else:
            print("✗ Normal operation restore: FAILED")
        
        self.client.loop_stop()
        self.client.disconnect()

def main():
    tester = MemoryRecoveryTest()
    tester.run_test()

if __name__ == "__main__":
    main()