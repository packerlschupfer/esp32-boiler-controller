#!/usr/bin/env python3
import serial
import time
import sys

def monitor_serial():
    try:
        # Open serial connection
        ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
        
        # Clear any existing data
        ser.reset_input_buffer()
        
        # Send reset to restart the device
        ser.write(b'\x03\x03')  # Ctrl+C
        time.sleep(0.1)
        ser.write(b'reboot\r\n')
        time.sleep(1)
        
        print("Monitoring serial output for NTP messages...")
        print("-" * 80)
        
        start_time = time.time()
        ntp_messages = []
        
        # Monitor for 30 seconds
        while time.time() - start_time < 30:
            if ser.in_waiting:
                try:
                    line = ser.readline().decode('utf-8', errors='ignore').strip()
                    if line:
                        # Print all lines with timestamp
                        timestamp = time.strftime("%H:%M:%S", time.localtime())
                        print(f"[{timestamp}] {line}")
                        
                        # Collect NTP-related messages
                        if any(keyword in line for keyword in ['NTP', 'epoch', 'syncTime', 'UTC', 'time']):
                            ntp_messages.append(f"[{timestamp}] {line}")
                except Exception as e:
                    pass
        
        # Summary of NTP messages
        if ntp_messages:
            print("\n" + "=" * 80)
            print("NTP-RELATED MESSAGES SUMMARY:")
            print("=" * 80)
            for msg in ntp_messages:
                print(msg)
        
        ser.close()
        
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    monitor_serial()