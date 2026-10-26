#!/usr/bin/env python3
import serial
import time
import sys

# Simple serial monitor script
ser = serial.Serial('/dev/ttyACM0', 921600, timeout=0.1)
print("Monitoring serial output (Ctrl+C to exit)...")
print("-" * 80)

try:
    while True:
        if ser.in_waiting:
            data = ser.read(ser.in_waiting)
            print(data.decode('utf-8', errors='ignore'), end='', flush=True)
        time.sleep(0.01)
except KeyboardInterrupt:
    print("\nMonitoring stopped.")
    ser.close()