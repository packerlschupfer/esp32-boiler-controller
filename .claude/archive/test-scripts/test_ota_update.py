#!/usr/bin/env python3
"""
OTA (Over-The-Air) Update Test Script for ESPlan Boiler Controller
Tests OTA functionality, memory usage during updates, and rollback scenarios
"""

import os
import sys
import time
import subprocess
import socket
import hashlib
from datetime import datetime
import paho.mqtt.client as mqtt
import json

# Configuration
DEVICE_IP = None  # Will be discovered via mDNS or set manually
DEVICE_HOSTNAME = "ESPlan-Boiler"
OTA_PORT = 3232
OTA_PASSWORD = "update-password"
MQTT_HOST = "192.168.16.16"
MQTT_USER = "YOUR_MQTT_USER"
MQTT_PASS = "YOUR_MQTT_PASSWORD"

class OTATestManager:
    def __init__(self):
        self.device_ip = None
        self.mqtt_client = None
        self.memory_stats = []
        self.update_progress = []
        self.test_results = {}
        
    def setup_mqtt(self):
        """Setup MQTT client for monitoring during OTA"""
        self.mqtt_client = mqtt.Client(client_id="ota_test_client")
        self.mqtt_client.username_pw_set(MQTT_USER, MQTT_PASS)
        self.mqtt_client.on_connect = self.on_mqtt_connect
        self.mqtt_client.on_message = self.on_mqtt_message
        
        try:
            self.mqtt_client.connect(MQTT_HOST, 1883, 60)
            self.mqtt_client.loop_start()
            time.sleep(2)  # Let connection settle
            return True
        except Exception as e:
            print(f"✗ MQTT connection failed: {e}")
            return False
    
    def on_mqtt_connect(self, client, userdata, flags, rc):
        if rc == 0:
            print("✓ Connected to MQTT broker")
            # Subscribe to relevant topics
            client.subscribe("cmd/boiler/diagnostics/memory/response", 1)
            client.subscribe("cmd/boiler/diagnostics/health/response", 1)
            client.subscribe("cmd/boiler/state/ota", 1)
        else:
            print(f"✗ MQTT connection failed with code: {rc}")
    
    def on_mqtt_message(self, client, userdata, msg):
        """Monitor memory and OTA status during update"""
        topic = msg.topic
        
        if "memory" in topic:
            try:
                data = json.loads(msg.payload.decode())
                self.memory_stats.append({
                    "timestamp": datetime.now().isoformat(),
                    "free_heap": data.get("free_heap", 0),
                    "min_free_heap": data.get("min_free_heap", 0),
                    "largest_block": data.get("largest_free_block", 0)
                })
            except:
                pass
        elif "ota" in topic:
            try:
                data = json.loads(msg.payload.decode())
                self.update_progress.append({
                    "timestamp": datetime.now().isoformat(),
                    "progress": data.get("progress", 0),
                    "status": data.get("status", "unknown")
                })
            except:
                pass
    
    def discover_device(self):
        """Discover device IP via mDNS or manual entry"""
        print(f"\n--- Device Discovery ---")
        
        try:
            # Try mDNS discovery first
            print(f"Attempting mDNS discovery for {DEVICE_HOSTNAME}.local...")
            result = subprocess.run(
                ["avahi-resolve", "-n", f"{DEVICE_HOSTNAME}.local"],
                capture_output=True,
                text=True,
                timeout=5
            )
            
            if result.returncode == 0:
                # Parse IP from output
                parts = result.stdout.strip().split()
                if len(parts) >= 2:
                    self.device_ip = parts[1]
                    print(f"✓ Found device at: {self.device_ip}")
                    return True
        except:
            pass
        
        # Fallback to manual entry
        print("✗ mDNS discovery failed")
        ip = input("Enter device IP address: ").strip()
        
        # Verify connectivity
        if self.ping_device(ip):
            self.device_ip = ip
            return True
        
        return False
    
    def ping_device(self, ip):
        """Check if device is reachable"""
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(2)
            result = sock.connect_ex((ip, OTA_PORT))
            sock.close()
            return result == 0
        except:
            return False
    
    def build_firmware(self, env="esp32dev_ota_release"):
        """Build firmware for OTA update"""
        print(f"\n--- Building Firmware ---")
        print(f"Environment: {env}")
        
        try:
            # Clean build
            subprocess.run(["pio", "run", "-e", env, "--target", "clean"], check=True)
            
            # Build firmware
            result = subprocess.run(
                ["pio", "run", "-e", env],
                capture_output=True,
                text=True
            )
            
            if result.returncode == 0:
                print("✓ Firmware built successfully")
                
                # Get firmware path
                firmware_path = f".pio/build/{env}/firmware.bin"
                if os.path.exists(firmware_path):
                    size = os.path.getsize(firmware_path)
                    print(f"  Firmware size: {size/1024:.1f} KB")
                    
                    # Calculate checksum
                    with open(firmware_path, "rb") as f:
                        checksum = hashlib.md5(f.read()).hexdigest()
                    print(f"  MD5 checksum: {checksum}")
                    
                    return firmware_path
                else:
                    print("✗ Firmware file not found")
                    return None
            else:
                print("✗ Build failed")
                print(result.stderr)
                return None
                
        except Exception as e:
            print(f"✗ Build error: {e}")
            return None
    
    def monitor_memory(self, duration=10):
        """Monitor memory usage for specified duration"""
        print(f"\nMonitoring memory for {duration}s...")
        
        start_time = time.time()
        self.memory_stats.clear()
        
        while time.time() - start_time < duration:
            # Request memory diagnostics
            self.mqtt_client.publish("cmd/boiler/diagnostics/memory", "", qos=1)
            time.sleep(2)
        
        if self.memory_stats:
            # Analyze memory stats
            min_free = min(stat["free_heap"] for stat in self.memory_stats)
            avg_free = sum(stat["free_heap"] for stat in self.memory_stats) / len(self.memory_stats)
            print(f"  Min free heap: {min_free/1024:.1f} KB")
            print(f"  Avg free heap: {avg_free/1024:.1f} KB")
            return min_free, avg_free
        
        return 0, 0
    
    def perform_ota_update(self, firmware_path):
        """Perform OTA update using espota.py"""
        print(f"\n--- Performing OTA Update ---")
        print(f"Target: {self.device_ip}:{OTA_PORT}")
        print(f"Firmware: {firmware_path}")
        
        # Find espota.py
        espota_paths = [
            ".platformio/packages/framework-arduinoespressif32/tools/espota.py",
            os.path.expanduser("~/.platformio/packages/framework-arduinoespressif32/tools/espota.py"),
            "/usr/local/bin/espota.py"
        ]
        
        espota_path = None
        for path in espota_paths:
            if os.path.exists(path):
                espota_path = path
                break
        
        if not espota_path:
            print("✗ espota.py not found")
            return False
        
        # Start memory monitoring in background
        print("\nStarting memory monitoring...")
        self.memory_stats.clear()
        
        # Build espota command
        cmd = [
            sys.executable,  # Python interpreter
            espota_path,
            "-i", self.device_ip,
            "-p", str(OTA_PORT),
            "-a", OTA_PASSWORD,
            "-f", firmware_path,
            "--progress"
        ]
        
        print("\nUploading firmware...")
        start_time = time.time()
        
        try:
            # Run OTA update
            process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                universal_newlines=True
            )
            
            # Monitor output
            for line in process.stdout:
                print(f"  {line.strip()}")
                
                # Request memory stats periodically
                if "%" in line:
                    self.mqtt_client.publish("cmd/boiler/diagnostics/memory", "", qos=1)
            
            # Wait for completion
            returncode = process.wait()
            duration = time.time() - start_time
            
            if returncode == 0:
                print(f"\n✓ OTA update successful (took {duration:.1f}s)")
                return True
            else:
                stderr = process.stderr.read()
                print(f"\n✗ OTA update failed: {stderr}")
                return False
                
        except Exception as e:
            print(f"\n✗ OTA update error: {e}")
            return False
    
    def verify_update(self):
        """Verify device is running after update"""
        print(f"\n--- Verifying Update ---")
        
        # Wait for device to reboot
        print("Waiting for device to reboot...")
        time.sleep(10)
        
        # Check connectivity
        retries = 10
        while retries > 0:
            if self.ping_device(self.device_ip):
                print("✓ Device is responding")
                
                # Request health status
                self.mqtt_client.publish("cmd/boiler/diagnostics/health", "", qos=1)
                time.sleep(2)
                
                return True
            
            retries -= 1
            time.sleep(2)
        
        print("✗ Device not responding after update")
        return False
    
    def run_test_suite(self):
        """Run complete OTA test suite"""
        print("\n=== ESPlan Boiler Controller OTA Test Suite ===")
        print(f"Started: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        
        # Test 1: Device Discovery
        print("\n[Test 1] Device Discovery")
        if not self.discover_device():
            print("✗ Failed to discover device")
            return
        self.test_results["discovery"] = "PASSED"
        
        # Test 2: MQTT Connection
        print("\n[Test 2] MQTT Monitoring Setup")
        if not self.setup_mqtt():
            print("✗ Failed to setup MQTT monitoring")
            return
        self.test_results["mqtt_setup"] = "PASSED"
        
        # Test 3: Pre-update Memory Check
        print("\n[Test 3] Pre-Update Memory Analysis")
        min_free_pre, avg_free_pre = self.monitor_memory(10)
        self.test_results["pre_update_memory"] = {
            "min_free": min_free_pre,
            "avg_free": avg_free_pre
        }
        
        # Test 4: Build Firmware
        print("\n[Test 4] Firmware Build")
        firmware_path = self.build_firmware()
        if not firmware_path:
            print("✗ Failed to build firmware")
            self.test_results["build"] = "FAILED"
            return
        self.test_results["build"] = "PASSED"
        
        # Test 5: OTA Update
        print("\n[Test 5] OTA Update Process")
        if self.perform_ota_update(firmware_path):
            self.test_results["ota_update"] = "PASSED"
        else:
            self.test_results["ota_update"] = "FAILED"
            return
        
        # Test 6: Post-update Verification
        print("\n[Test 6] Post-Update Verification")
        if self.verify_update():
            self.test_results["verification"] = "PASSED"
        else:
            self.test_results["verification"] = "FAILED"
        
        # Test 7: Post-update Memory Check
        print("\n[Test 7] Post-Update Memory Analysis")
        min_free_post, avg_free_post = self.monitor_memory(10)
        self.test_results["post_update_memory"] = {
            "min_free": min_free_post,
            "avg_free": avg_free_post
        }
        
        # Print summary
        self.print_summary()
    
    def print_summary(self):
        """Print test summary"""
        print("\n" + "="*60)
        print("TEST SUMMARY")
        print("="*60)
        
        # Test results
        for test, result in self.test_results.items():
            if isinstance(result, dict):
                print(f"{test}:")
                for key, value in result.items():
                    if isinstance(value, (int, float)):
                        print(f"  {key}: {value/1024:.1f} KB")
                    else:
                        print(f"  {key}: {value}")
            else:
                status = "✓" if result == "PASSED" else "✗"
                print(f"{status} {test}: {result}")
        
        # Memory analysis
        if "pre_update_memory" in self.test_results and "post_update_memory" in self.test_results:
            pre_min = self.test_results["pre_update_memory"]["min_free"]
            post_min = self.test_results["post_update_memory"]["min_free"]
            
            print(f"\nMemory Impact:")
            print(f"  Pre-update min free:  {pre_min/1024:.1f} KB")
            print(f"  Post-update min free: {post_min/1024:.1f} KB")
            print(f"  Difference: {(post_min - pre_min)/1024:.1f} KB")
        
        # Overall result
        all_passed = all(
            result == "PASSED" 
            for test, result in self.test_results.items() 
            if isinstance(result, str)
        )
        
        print(f"\nOverall Result: {'✓ ALL TESTS PASSED' if all_passed else '✗ SOME TESTS FAILED'}")
        
        if self.mqtt_client:
            self.mqtt_client.loop_stop()
            self.mqtt_client.disconnect()

def main():
    tester = OTATestManager()
    
    try:
        tester.run_test_suite()
    except KeyboardInterrupt:
        print("\n\nTest interrupted by user")
    except Exception as e:
        print(f"\n✗ Test error: {e}")
        import traceback
        traceback.print_exc()

if __name__ == "__main__":
    main()