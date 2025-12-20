#!/usr/bin/env python3
"""
Monitor task stack usage from ESP32 serial output
Helps identify tasks that need larger stacks
"""

import serial
import sys
import re
import time
from datetime import datetime

# ANSI color codes
RED = '\033[91m'
YELLOW = '\033[93m'
GREEN = '\033[92m'
RESET = '\033[0m'

class StackMonitor:
    def __init__(self, port='/dev/ttyACM0', baudrate=921600):
        self.port = port
        self.baudrate = baudrate
        self.task_stacks = {}
        self.stack_warnings = {}
        
    def analyze_line(self, line):
        # Look for stack information in monitoring output
        # Format: "TaskName:1234" where 1234 is stack words remaining
        stack_match = re.search(r'(\w+):(\d+)\s', line)
        if stack_match:
            task_name = stack_match.group(1)
            stack_words = int(stack_match.group(2))
            stack_bytes = stack_words * 4  # ESP32 uses 4 bytes per word
            
            # Track minimum seen
            if task_name not in self.task_stacks or stack_bytes < self.task_stacks[task_name]:
                self.task_stacks[task_name] = stack_bytes
                
            # Warn if low
            if stack_bytes < 800:  # Less than 800 bytes is concerning
                self.stack_warnings[task_name] = stack_bytes
                return f"{RED}WARNING: {task_name} has only {stack_bytes} bytes stack remaining!{RESET}"
            elif stack_bytes < 1200:  # Less than 1200 bytes is worth noting
                return f"{YELLOW}Note: {task_name} has {stack_bytes} bytes stack remaining{RESET}"
                
        # Look for stack overflow or panic
        if "stack canary" in line.lower() or "stack overflow" in line.lower():
            return f"{RED}CRITICAL: Stack overflow detected!{RESET}"
            
        if "guru meditation" in line.lower():
            return f"{RED}CRITICAL: System panic detected!{RESET}"
            
        return None
        
    def print_summary(self):
        print("\n=== Stack Usage Summary ===")
        print(f"{'Task':<20} {'Min Stack Remaining':<20} {'Status'}")
        print("-" * 60)
        
        for task, min_stack in sorted(self.task_stacks.items()):
            status = "OK"
            color = GREEN
            
            if min_stack < 800:
                status = "CRITICAL"
                color = RED
            elif min_stack < 1200:
                status = "LOW"
                color = YELLOW
                
            print(f"{task:<20} {min_stack:<20} {color}{status}{RESET}")
            
        if self.stack_warnings:
            print(f"\n{RED}Tasks with concerning stack usage:{RESET}")
            for task, stack in self.stack_warnings.items():
                print(f"  - {task}: {stack} bytes")
                
    def monitor(self):
        try:
            print(f"Opening serial port {self.port} at {self.baudrate} baud...")
            with serial.Serial(self.port, self.baudrate, timeout=1) as ser:
                print("Monitoring stack usage... (Press Ctrl+C to stop)")
                print("Waiting for MonitoringTask output...\n")
                
                while True:
                    try:
                        line = ser.readline().decode('utf-8', errors='ignore').strip()
                        if line:
                            # Print all lines for context
                            print(f"[{datetime.now().strftime('%H:%M:%S')}] {line}")
                            
                            # Analyze for stack issues
                            warning = self.analyze_line(line)
                            if warning:
                                print(warning)
                                
                    except KeyboardInterrupt:
                        break
                    except Exception as e:
                        print(f"Error reading line: {e}")
                        
        except serial.SerialException as e:
            print(f"Error opening serial port: {e}")
            print("Available ports:")
            import serial.tools.list_ports
            for port in serial.tools.list_ports.comports():
                print(f"  - {port.device}")
            sys.exit(1)
            
        finally:
            self.print_summary()

def main():
    import argparse
    parser = argparse.ArgumentParser(description='Monitor ESP32 task stack usage')
    parser.add_argument('-p', '--port', default='/dev/ttyACM0', help='Serial port')
    parser.add_argument('-b', '--baud', type=int, default=115200, help='Baud rate')
    
    args = parser.parse_args()
    
    monitor = StackMonitor(args.port, args.baud)
    monitor.monitor()

if __name__ == '__main__':
    main()