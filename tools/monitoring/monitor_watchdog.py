#!/usr/bin/env python3
"""
Watchdog Monitor for ESPlan Boiler Controller
Specifically monitors for task watchdog timeouts and task timing
"""

import serial
import time
import re
import argparse
from datetime import datetime

class WatchdogMonitor:
    def __init__(self, port='/dev/ttyACM0', baudrate=921600):
        self.port = port
        self.baudrate = baudrate
        
    def monitor_task_timing(self, task_name='MonitoringTask', duration=120):
        """Monitor specific task timing and watchdog behavior"""
        task_pattern = re.compile(rf'\[(\d+)\]\[{task_name}\]')
        watchdog_pattern = re.compile(rf'task_wdt.*{task_name}')
        
        first_log_time = None
        main_loop_time = None
        watchdog_errors = []
        task_logs = []
        
        try:
            with serial.Serial(self.port, self.baudrate, timeout=2) as ser:
                print(f"Monitoring {task_name} timing for {duration} seconds...")
                print("-" * 70)
                
                start_time = time.time()
                while time.time() - start_time < duration:
                    if ser.in_waiting:
                        line = ser.readline()
                        try:
                            decoded = line.decode('utf-8', errors='replace').rstrip()
                            
                            # Check for task messages
                            match = task_pattern.search(decoded)
                            if match:
                                timestamp = int(match.group(1))
                                task_logs.append((timestamp, decoded))
                                
                                if first_log_time is None:
                                    first_log_time = timestamp
                                    print(f"[{timestamp}ms] Task started")
                                
                                if 'entering main loop' in decoded:
                                    main_loop_time = timestamp
                                    init_time = main_loop_time - first_log_time
                                    print(f"[{timestamp}ms] Main loop started (init took {init_time}ms)")
                                
                                # Show important messages
                                if any(x in decoded for x in ['running', 'ready', 'loop', 'Heap']):
                                    print(f"[{timestamp}ms] {decoded}")
                            
                            # Check for watchdog errors
                            if watchdog_pattern.search(decoded):
                                watchdog_errors.append(decoded)
                                print(f"\n!!! WATCHDOG ERROR: {decoded}\n")
                                
                        except:
                            pass
                
                # Summary
                print("\n" + "="*70)
                print("SUMMARY:")
                if first_log_time and main_loop_time:
                    print(f"Initialization time: {main_loop_time - first_log_time}ms")
                print(f"Total task logs: {len(task_logs)}")
                print(f"Watchdog errors: {len(watchdog_errors)}")
                print(f"Status: {'PASS' if not watchdog_errors else 'FAIL'}")
                
        except Exception as e:
            print(f"Error: {e}")

def main():
    parser = argparse.ArgumentParser(description='Monitor task watchdog behavior')
    parser.add_argument('-p', '--port', default='/dev/ttyACM0', help='Serial port')
    parser.add_argument('-b', '--baud', type=int, default=115200, help='Baud rate')
    parser.add_argument('-t', '--time', type=int, default=120, help='Monitoring duration in seconds')
    parser.add_argument('--task', default='MonitoringTask', help='Task name to monitor')
    
    args = parser.parse_args()
    
    monitor = WatchdogMonitor(args.port, args.baud)
    monitor.monitor_task_timing(args.task, args.time)

if __name__ == "__main__":
    main()