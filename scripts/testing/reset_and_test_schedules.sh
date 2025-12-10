#!/bin/bash

BROKER="192.168.20.27"
USER="YOUR_MQTT_USER"
PASS="YOUR_MQTT_PASSWORD"

echo "=== DS3231 Schedule Integration Test ==="
echo

# Reset the device
echo "1. Resetting ESP32 via DTR toggle..."
python3 -c "
import serial
import time
s = serial.Serial('/dev/ttyACM0', 921600)
s.dtr = False
time.sleep(0.1)
s.dtr = True
s.close()
"

echo "2. Waiting for device to boot (30 seconds)..."
sleep 30

echo "3. Checking device is online..."
ping -c 3 192.168.20.40

echo
echo "4. Getting current hot water status..."
mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -C 1 | jq '.'

echo
echo "5. Adding test schedule (30 minutes from now)..."
CURRENT_HOUR=$(date +%H)
CURRENT_MIN=$(date +%M)
SCHEDULE_MIN=$((CURRENT_MIN + 30))
SCHEDULE_HOUR=$CURRENT_HOUR

# Handle minute overflow
if [ $SCHEDULE_MIN -ge 60 ]; then
    SCHEDULE_MIN=$((SCHEDULE_MIN - 60))
    SCHEDULE_HOUR=$((SCHEDULE_HOUR + 1))
    if [ $SCHEDULE_HOUR -ge 24 ]; then
        SCHEDULE_HOUR=0
    fi
fi

echo "   Schedule time: $SCHEDULE_HOUR:$(printf %02d $SCHEDULE_MIN)"
mosquitto_pub -h $BROKER -u $USER -P $PASS -t "hotwater/schedule/add" -m "{
  \"name\": \"Test Schedule\",
  \"days\": 127,
  \"start_hour\": $SCHEDULE_HOUR,
  \"start_minute\": $SCHEDULE_MIN,
  \"end_hour\": $((SCHEDULE_HOUR + 1)),
  \"end_minute\": $SCHEDULE_MIN,
  \"enabled\": true
}"

sleep 3

echo
echo "6. Checking schedule was added..."
mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -C 1 | jq '.schedules'

echo
echo "7. Adding immediate test schedule (5 minutes from now for quick test)..."
QUICK_MIN=$((CURRENT_MIN + 5))
QUICK_HOUR=$CURRENT_HOUR
if [ $QUICK_MIN -ge 60 ]; then
    QUICK_MIN=$((QUICK_MIN - 60))
    QUICK_HOUR=$((QUICK_HOUR + 1))
    if [ $QUICK_HOUR -ge 24 ]; then
        QUICK_HOUR=0
    fi
fi

mosquitto_pub -h $BROKER -u $USER -P $PASS -t "hotwater/schedule/add" -m "{
  \"name\": \"Quick Test\",
  \"days\": 127,
  \"start_hour\": $QUICK_HOUR,
  \"start_minute\": $QUICK_MIN,
  \"end_hour\": $((QUICK_HOUR + 1)),
  \"end_minute\": 0,
  \"enabled\": true
}"

sleep 3

echo
echo "8. Monitoring for pre-heating (press Ctrl+C to stop)..."
echo "   Pre-heating should start soon for 'Quick Test' schedule at $QUICK_HOUR:$(printf %02d $QUICK_MIN)"
echo

mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -v | while read topic payload; do
    timestamp=$(date +%H:%M:%S)
    preheating=$(echo "$payload" | jq -r '.preheating')
    active=$(echo "$payload" | jq -r '.active')
    scheduleName=$(echo "$payload" | jq -r '.scheduleName')
    waterTemp=$(echo "$payload" | jq -r '.waterTemp')
    
    echo "[$timestamp] Pre-heating: $preheating, Active: $active, Schedule: $scheduleName, Temp: ${waterTemp}°C"
    
    # Highlight state changes
    if [ "$preheating" = "true" ]; then
        echo ">>> PRE-HEATING STARTED for $scheduleName!"
    fi
    if [ "$active" = "true" ]; then
        echo ">>> SCHEDULE ACTIVE: $scheduleName"
    fi
done