#!/usr/bin/env python3
"""
Boot Sequence Monitor for ESPlan Boiler Controller
Captures and analyzes the boot sequence
"""

import serial
import time
import re
import argparse
from datetime import datetime

class BootMonitor:
    def __init__(self, port='/dev/ttyACM0', baudrate=921600):
        self.port = port
        self.baudrate = baudrate
        
    def capture_boot(self, timeout=60):
        """Capture boot sequence"""
        print("Waiting for boot sequence...")
        print("(Reset the device or upload firmware to trigger boot)")
        print("-" * 70)
        
        boot_stages = {
            'boot_start': re.compile(r'ESPlan Boiler Controller'),
            'wifi_init': re.compile(r'WiFi|ETH|Ethernet'),
            'modbus_init': re.compile(r'Modbus|MB8ART|RYN4'),
            'service_register': re.compile(r'Registered service:'),
            'task_start': re.compile(r'Starting.*task|Task.*started'),
            'mqtt_connect': re.compile(r'MQTT.*connect'),
            'init_complete': re.compile(r'System initialization complete'),
            'monitoring_start': re.compile(r'MonitoringTask.*running'),
            'sensors_ready': re.compile(r'Sensors ready'),
        }
        
        stage_times = {}
        boot_logs = []
        boot_start_time = None
        
        try:
            with serial.Serial(self.port, self.baudrate, timeout=2) as ser:
                ser.reset_input_buffer()
                
                start_wait = time.time()
                
                while time.time() - start_wait < timeout:
                    if ser.in_waiting:
                        line = ser.readline()
                        try:
                            decoded = line.decode('utf-8', errors='replace').rstrip()
                            
                            # Store all boot logs
                            if boot_start_time is not None:
                                boot_logs.append(decoded)
                            
                            # Check for boot stages
                            for stage, pattern in boot_stages.items():
                                if pattern.search(decoded) and stage not in stage_times:
                                    if stage == 'boot_start':
                                        boot_start_time = time.time()
                                        stage_times[stage] = 0
                                        print(f"\nBOOT DETECTED at {datetime.now().strftime('%H:%M:%S')}")
                                    elif boot_start_time:
                                        elapsed = int((time.time() - boot_start_time) * 1000)
                                        stage_times[stage] = elapsed
                                        print(f"[+{elapsed:5d}ms] {stage}: {decoded[:80]}...")
                            
                            # Check for errors during boot
                            if boot_start_time and any(x in decoded for x in ['[E]', 'Error', 'Failed']):
                                print(f"[ERROR] {decoded}")
                            
                            # Stop after system is fully initialized
                            if 'init_complete' in stage_times and 'sensors_ready' in stage_times:
                                print("\nBoot sequence complete!")
                                break
                                
                        except:
                            pass
                
                # Summary
                print("\n" + "="*70)
                print("BOOT SEQUENCE SUMMARY:")
                
                if stage_times:
                    print("\nStage timings:")
                    for stage, timing in sorted(stage_times.items(), key=lambda x: x[1]):
                        print(f"  {stage:20s}: {timing:5d}ms")
                    
                    total_boot = max(stage_times.values())
                    print(f"\nTotal boot time: {total_boot}ms ({total_boot/1000:.1f}s)")
                else:
                    print("No boot sequence detected. Please reset the device.")
                    
        except Exception as e:
            print(f"Error: {e}")

def main():
    parser = argparse.ArgumentParser(description='Monitor boot sequence')
    parser.add_argument('-p', '--port', default='/dev/ttyACM0', help='Serial port')
    parser.add_argument('-b', '--baud', type=int, default=115200, help='Baud rate')
    parser.add_argument('-t', '--timeout', type=int, default=60, help='Maximum wait time for boot')
    
    args = parser.parse_args()
    
    monitor = BootMonitor(args.port, args.baud)
    monitor.capture_boot(args.timeout)

if __name__ == "__main__":
    main()