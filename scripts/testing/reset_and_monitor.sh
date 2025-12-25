#!/bin/bash
echo "Resetting ESP32 via DTR toggle..."

# Reset via DTR
python3 -c "
import serial
import time
s = serial.Serial('/dev/ttyACM0', 921600)
s.dtr = False
time.sleep(0.1)
s.dtr = True
s.close()
"

sleep 1

echo "Monitoring output..."
python3 -c "
import serial
import time

port = '/dev/ttyACM0'
baudrate = 921600

with serial.Serial(port, baudrate, timeout=1) as ser:
    start_time = time.time()
    while time.time() - start_time < 30:
        if ser.in_waiting > 0:
            line = ser.readline()
            try:
                text = line.decode('utf-8').strip()
                # Highlight phase lock related logs
                if any(keyword in text for keyword in ['Learning', 'PhaseLock', 'checkLockStatus', 'beacons=', 'intervals=', 'Phase-lock', 'Starting learning']):
                    print(f'>>> {text}')
                else:
                    print(text)
            except:
                pass
"