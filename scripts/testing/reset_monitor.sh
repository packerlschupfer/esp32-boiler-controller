#!/bin/bash
# Reset ESP32 and monitor serial output

echo "Resetting ESP32..."
# Send reset command via serial
echo -ne '\x03\x03' > /dev/ttyACM0
sleep 0.5
echo "reboot" > /dev/ttyACM0
sleep 2

echo "Monitoring output for NTP messages..."
echo "====================================="

# Monitor for 40 seconds, grep for NTP-related messages
timeout 40 cat /dev/ttyACM0 | grep -E "NTP|epoch|syncTime|UTC|DEBUG|result\." --line-buffered