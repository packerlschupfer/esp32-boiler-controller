#!/usr/bin/env python3
import serial
import time
import sys

port = '/dev/ttyACM0'
baudrate = 921600

try:
    ser = serial.Serial(port, baudrate, timeout=1)
    print(f"Connected to {port} at {baudrate} baud")
    print("Press Ctrl+C to exit\n")
    
    while True:
        if ser.in_waiting:
            line = ser.readline().decode('utf-8', errors='ignore').rstrip()
            if line:
                print(line)
                sys.stdout.flush()
                
                # Check for specific messages related to pre-heating
                if "HWScheduler" in line or "Pre-heat" in line or "preheating" in line:
                    print(f">>> PRE-HEATING: {line}")
                    
except KeyboardInterrupt:
    print("\nExiting...")
except Exception as e:
    print(f"Error: {e}")
finally:
    if 'ser' in locals() and ser.is_open:
        ser.close()