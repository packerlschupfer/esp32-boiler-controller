#!/usr/bin/env python3
"""
Simple OTA Status Monitor via MQTT
Monitors memory and system status during OTA updates
"""

import paho.mqtt.client as mqtt
import json
import time
from datetime import datetime
import sys

# MQTT Configuration
MQTT_HOST = "192.168.16.16"
MQTT_USER = "YOUR_MQTT_USER"
MQTT_PASS = "YOUR_MQTT_PASSWORD"
TOPIC_PREFIX = "cmd/boiler"

class OTAMonitor:
    def __init__(self):
        self.client = mqtt.Client(client_id="ota_monitor")
        self.client.username_pw_set(MQTT_USER, MQTT_PASS)
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        self.last_memory = {}
        self.ota_active = False
        
    def on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            print("✓ Connected to MQTT broker")
            # Subscribe to relevant topics
            topics = [
                (f"{TOPIC_PREFIX}/diagnostics/memory/response", 1),
                (f"{TOPIC_PREFIX}/diagnostics/health/response", 1),
                (f"{TOPIC_PREFIX}/state/ota", 1),
                (f"{TOPIC_PREFIX}/state/system", 1),
            ]
            for topic, qos in topics:
                client.subscribe(topic, qos)
                print(f"  Subscribed to: {topic}")
        else:
            print(f"✗ Failed to connect: {rc}")
    
    def on_message(self, client, userdata, msg):
        timestamp = datetime.now().strftime("%H:%M:%S")
        topic = msg.topic
        
        try:
            if topic.endswith("/memory/response"):
                data = json.loads(msg.payload.decode())
                free_kb = data.get("free_heap", 0) / 1024
                min_kb = data.get("min_free_heap", 0) / 1024
                largest_kb = data.get("largest_free_block", 0) / 1024
                frag = data.get("fragmentation", 0)
                
                # Detect significant changes
                if self.last_memory:
                    diff = self.last_memory.get("free", 0) - free_kb
                    if abs(diff) > 10:  # More than 10KB change
                        print(f"\n[{timestamp}] ⚠️  Memory Change: {diff:+.1f} KB")
                
                self.last_memory = {"free": free_kb, "min": min_kb}
                
                # Color code based on free memory
                if free_kb < 50:
                    color = "\033[91m"  # Red
                elif free_kb < 100:
                    color = "\033[93m"  # Yellow
                else:
                    color = "\033[92m"  # Green
                
                print(f"[{timestamp}] Memory: {color}{free_kb:.1f} KB free\033[0m | "
                      f"Min: {min_kb:.1f} KB | Largest: {largest_kb:.1f} KB | "
                      f"Frag: {frag:.1f}%", end='\r')
                      
            elif topic.endswith("/state/ota"):
                data = json.loads(msg.payload.decode())
                status = data.get("status", "unknown")
                
                if status == "updating":
                    self.ota_active = True
                    progress = data.get("progress", 0)
                    total = data.get("total", 0)
                    if total > 0:
                        percent = (progress / total) * 100
                        print(f"\n[{timestamp}] 🔄 OTA Update: {percent:.1f}% "
                              f"({progress}/{total} bytes)")
                else:
                    if self.ota_active:
                        print(f"\n[{timestamp}] ✓ OTA Update: {status}")
                        self.ota_active = False
                        
            elif topic.endswith("/health/response"):
                data = json.loads(msg.payload.decode())
                uptime = data.get("uptime_seconds", 0)
                tasks = data.get("running_tasks", 0)
                
                if uptime < 60:  # Recently rebooted
                    print(f"\n[{timestamp}] 🔄 System recently rebooted "
                          f"(uptime: {uptime}s)")
                          
        except json.JSONDecodeError:
            pass
        except Exception as e:
            print(f"\n[{timestamp}] Error: {e}")
    
    def request_diagnostics(self):
        """Periodically request diagnostics"""
        while True:
            try:
                # Request memory stats every 2 seconds
                self.client.publish(f"{TOPIC_PREFIX}/diagnostics/memory", "", qos=0)
                
                # Request health less frequently
                if int(time.time()) % 10 == 0:
                    self.client.publish(f"{TOPIC_PREFIX}/diagnostics/health", "", qos=0)
                
                time.sleep(2)
                
            except KeyboardInterrupt:
                break
            except Exception as e:
                print(f"\nError requesting diagnostics: {e}")
                time.sleep(5)
    
    def run(self):
        print("ESPlan Boiler Controller - OTA Status Monitor")
        print("=" * 50)
        print("Press Ctrl+C to exit\n")
        
        try:
            self.client.connect(MQTT_HOST, 1883, 60)
            self.client.loop_start()
            
            # Wait for connection
            time.sleep(2)
            
            # Start requesting diagnostics
            self.request_diagnostics()
            
        except KeyboardInterrupt:
            print("\n\nMonitoring stopped by user")
        except Exception as e:
            print(f"\nError: {e}")
        finally:
            self.client.loop_stop()
            self.client.disconnect()
            print("\nDisconnected")

def main():
    monitor = OTAMonitor()
    monitor.run()

if __name__ == "__main__":
    main()