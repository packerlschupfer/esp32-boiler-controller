#!/bin/bash

BROKER="192.168.20.27"
USER="YOUR_MQTT_USER"
PASS="YOUR_MQTT_PASSWORD"

echo "=== Testing DS3231 Schedule Integration ==="
echo

# Check current status
echo "1. Current hot water status:"
mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -C 1 | jq '.'
echo

# Add a test schedule (every day at current time + 30 minutes)
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

echo "2. Adding test schedule for $SCHEDULE_HOUR:$(printf %02d $SCHEDULE_MIN)"
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
echo "3. List all schedules:"
mosquitto_pub -h $BROKER -u $USER -P $PASS -t "hotwater/schedule/list" -m "1"
sleep 2
mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -C 1 | jq '.schedules'

echo
echo "4. Monitor for pre-heating (should start soon):"
timeout 120 mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -v | while read topic payload; do
    echo "[$(date +%H:%M:%S)] $payload" | jq -c '{preheating, scheduleName, estimatedReady}'
done