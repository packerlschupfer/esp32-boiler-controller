#!/usr/bin/env python3
"""
Sensor Monitor for ESPlan Boiler Controller
Monitors sensor readings and validity flags
"""

import serial
import time
import re
import argparse
import json
from datetime import datetime

class SensorMonitor:
    def __init__(self, port='/dev/ttyACM0', baudrate=921600):
        self.port = port
        self.baudrate = baudrate
        self.sensor_pattern = re.compile(r'sensors/status.*?({.*?})')
        self.temp_patterns = {
            'boiler_output': re.compile(r'Boiler.*?Output:\s*([\d.]+|N/A)'),
            'boiler_return': re.compile(r'Boiler.*?Return:\s*([\d.]+|N/A)'),
            'tank': re.compile(r'Tank:\s*([\d.]+|N/A)'),
            'wheater_output': re.compile(r'Water Heater.*?Output:\s*([\d.]+|N/A)'),
            'wheater_return': re.compile(r'Water Heater.*?Return:\s*([\d.]+|N/A)'),
            'outside': re.compile(r'Outside:\s*([\d.]+|N/A)'),
            'inside': re.compile(r'Inside:\s*([\d.]+|N/A)'),
            'heating_return': re.compile(r'Heating Return:\s*([\d.]+|N/A)'),
        }
        
    def monitor_sensors(self, duration=60, json_output=False):
        """Monitor sensor readings"""
        readings = []
        zero_readings = []
        invalid_readings = []
        
        try:
            with serial.Serial(self.port, self.baudrate, timeout=2) as ser:
                print(f"Monitoring sensors for {duration} seconds...")
                print("-" * 70)
                
                start_time = time.time()
                last_reading_time = None
                
                while time.time() - start_time < duration:
                    if ser.in_waiting:
                        line = ser.readline()
                        try:
                            decoded = line.decode('utf-8', errors='replace').rstrip()
                            
                            # Look for JSON sensor data from MQTT
                            json_match = self.sensor_pattern.search(decoded)
                            if json_match:
                                try:
                                    sensor_data = json.loads(json_match.group(1))
                                    timestamp = sensor_data.get('timestamp', 0)
                                    
                                    if json_output:
                                        print(f"\n[{timestamp}ms] Sensor JSON:")
                                        print(json.dumps(sensor_data, indent=2))
                                    
                                    readings.append(sensor_data)
                                    
                                    # Check for invalid/zero readings
                                    for category in ['boiler', 'water_heater', 'heating', 'environment']:
                                        if category in sensor_data:
                                            for sensor, data in sensor_data[category].items():
                                                if isinstance(data, dict):
                                                    value = data.get('value', 0)
                                                    valid = data.get('valid', False)
                                                    
                                                    if not valid:
                                                        invalid_readings.append(f"{category}.{sensor}")
                                                    elif value == 0.0:
                                                        zero_readings.append(f"{category}.{sensor}")
                                except:
                                    pass
                            
                            # Look for text sensor output
                            if 'MonitoringTask' in decoded and any(p in decoded for p in ['Output:', 'Return:', 'Tank:', 'Outside:', 'Inside:']):
                                current_time = time.time()
                                if last_reading_time:
                                    interval = int((current_time - last_reading_time) * 1000)
                                    print(f"\n[+{interval}ms] {decoded}")
                                else:
                                    print(f"\n{decoded}")
                                last_reading_time = current_time
                                
                                # Check for 0.0°C readings
                                if '0.0°C' in decoded:
                                    print("  WARNING: Zero temperature detected!")
                                
                        except:
                            pass
                
                # Summary
                print("\n" + "="*70)
                print("SENSOR MONITORING SUMMARY:")
                print(f"Total readings captured: {len(readings)}")
                
                if invalid_readings:
                    print(f"\nInvalid sensor readings: {len(set(invalid_readings))}")
                    for sensor in set(invalid_readings):
                        print(f"  - {sensor}")
                
                if zero_readings:
                    print(f"\nZero value readings: {len(set(zero_readings))}")
                    for sensor in set(zero_readings):
                        print(f"  - {sensor}")
                
                if readings and not invalid_readings and not zero_readings:
                    print("\nAll sensors reporting valid data!")
                    
        except Exception as e:
            print(f"Error: {e}")

def main():
    parser = argparse.ArgumentParser(description='Monitor sensor readings')
    parser.add_argument('-p', '--port', default='/dev/ttyACM0', help='Serial port')
    parser.add_argument('-b', '--baud', type=int, default=115200, help='Baud rate')
    parser.add_argument('-t', '--time', type=int, default=60, help='Monitoring duration in seconds')
    parser.add_argument('-j', '--json', action='store_true', help='Show JSON sensor data')
    
    args = parser.parse_args()
    
    monitor = SensorMonitor(args.port, args.baud)
    monitor.monitor_sensors(args.time, args.json)

if __name__ == "__main__":
    main()