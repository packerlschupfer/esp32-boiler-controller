#!/usr/bin/env python3
import serial
import time

port = '/dev/ttyACM0'
baudrate = 921600

print(f"Opening serial port {port} at {baudrate} baud...")

try:
    with serial.Serial(port, baudrate, timeout=1) as ser:
        print("Connected. Reading output...")
        start_time = time.time()
        
        while time.time() - start_time < 30:  # Read for 30 seconds
            if ser.in_waiting > 0:
                line = ser.readline()
                try:
                    print(line.decode('utf-8').strip())
                except:
                    print(f"[Raw bytes]: {line}")
                    
except Exception as e:
    print(f"Error: {e}")