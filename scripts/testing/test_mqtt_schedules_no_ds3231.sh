#!/bin/bash

BROKER="192.168.20.27"
USER="YOUR_MQTT_USER"
PASS="YOUR_MQTT_PASSWORD"

echo "=== Testing MQTT Schedule Commands (without DS3231 hardware) ==="
echo
echo "Note: The DS3231 hardware is not connected, so schedules won't be active"
echo "but we can test if the MQTT commands are received and processed."
echo

# Check device status
echo "1. Checking device online status..."
mosquitto_sub -h $BROKER -u $USER -P $PASS -t "system/device/ip" -C 1
echo

# Check if hot water status is being published
echo "2. Checking hot water status topic..."
timeout 5 mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -C 1 | jq '.' || echo "No hot water status received"
echo

# Try the legacy control commands that should work
echo "3. Testing legacy hot water control commands..."
echo "   Sending 'on' command..."
mosquitto_pub -h $BROKER -u $USER -P $PASS -t "hotwater/control" -m "on"
sleep 2

echo "   Sending 'status' command..."
mosquitto_pub -h $BROKER -u $USER -P $PASS -t "hotwater/control" -m "status"
sleep 2

echo "   Checking for response..."
timeout 5 mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -C 1 | jq '.' || echo "No response"
echo

# Monitor all MQTT topics to see what's being published
echo "4. Monitoring all MQTT topics for 30 seconds..."
timeout 30 mosquitto_sub -h $BROKER -u $USER -P $PASS -t "#" -v | grep -E "(hotwater|wheater|temperature|sensor)" | head -20