#!/usr/bin/env python3
"""
System Monitor for ESPlan Boiler Controller
Monitors serial output for warnings, errors, and system state
"""

import serial
import time
import re
import argparse
import sys
from datetime import datetime

class SystemMonitor:
    def __init__(self, port='/dev/ttyACM0', baudrate=921600):
        self.port = port
        self.baudrate = baudrate
        self.patterns = {
            'warnings': re.compile(r'\[W\]'),
            'errors': re.compile(r'\[E\]'),
            'service_not_found': re.compile(r'Service not found:'),
            'mqtt_failed': re.compile(r'Failed to publish'),
            'monitoring_zero': re.compile(r'MonitoringTask.*?0\.0°C'),
            'system_state': re.compile(r'System State:'),
            'watchdog': re.compile(r'task_wdt'),
            'monitoring_task': re.compile(r'\[(\d+)\]\[MonitoringTask\]'),
            'sensor_update': re.compile(r'Sensor update complete'),
            'relay_state': re.compile(r'Relay States'),
        }
        
    def monitor(self, duration=60, show_all=False):
        """Monitor system for specified duration"""
        counts = {key: 0 for key in self.patterns}
        samples = {key: [] for key in self.patterns}
        
        try:
            with serial.Serial(self.port, self.baudrate, timeout=2) as ser:
                print(f"Connected to {self.port} at {self.baudrate} baud")
                print(f"Monitoring for {duration} seconds...")
                print("-" * 70)
                
                start_time = time.time()
                while time.time() - start_time < duration:
                    if ser.in_waiting:
                        line = ser.readline()
                        try:
                            decoded = line.decode('utf-8', errors='replace').rstrip()
                            
                            # Check patterns
                            for key, pattern in self.patterns.items():
                                if pattern.search(decoded):
                                    counts[key] += 1
                                    if len(samples[key]) < 5:  # Keep first 5 samples
                                        samples[key].append(decoded)
                            
                            # Always show certain messages if requested
                            if show_all or any(x in decoded for x in ['System State:', 'MonitoringTask', 'Error', 'Warning']):
                                print(f"{datetime.now().strftime('%H:%M:%S')} | {decoded}")
                                
                        except Exception as e:
                            # Only show decode errors if verbose
                            if show_all:
                                print(f"Decode error: {e}")
                
                # Print summary
                print("\n" + "="*70)
                print("SUMMARY:")
                for key, count in counts.items():
                    if count > 0:
                        print(f"\n{key}: {count} occurrences")
                        for i, sample in enumerate(samples[key], 1):
                            print(f"  {i}. {sample[:100]}...")
                            
        except serial.SerialException as e:
            print(f"Serial Error: {e}")
            print("Please check that the device is connected and the port is correct")
            sys.exit(1)
        except KeyboardInterrupt:
            print("\nMonitoring stopped by user")
            sys.exit(0)
        except Exception as e:
            print(f"Unexpected error: {e}")
            sys.exit(1)

def main():
    parser = argparse.ArgumentParser(description='Monitor ESPlan Boiler Controller')
    parser.add_argument('-p', '--port', default='/dev/ttyACM0', help='Serial port')
    parser.add_argument('-b', '--baud', type=int, default=115200, help='Baud rate')
    parser.add_argument('-t', '--time', type=int, default=60, help='Monitoring duration in seconds')
    parser.add_argument('-a', '--all', action='store_true', help='Show all messages')
    
    args = parser.parse_args()
    
    monitor = SystemMonitor(args.port, args.baud)
    monitor.monitor(args.time, args.all)

if __name__ == "__main__":
    main()