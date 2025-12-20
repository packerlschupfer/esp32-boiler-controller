#!/bin/bash

BROKER="192.168.20.27"
USER="YOUR_MQTT_USER"
PASS="YOUR_MQTT_PASSWORD"

echo "=== Adding Test Schedule ==="
echo

# Get current day of week (0=Sunday, 4=Thursday)
DOW=$(date +%w)
DAYMASK=$((1 << DOW))
echo "Today is $(date +%A), day $DOW, bitmask: $DAYMASK"

# Set schedule for 5 minutes from now
CURRENT_HOUR=$(date +%-H)
CURRENT_MIN=$(date +%-M)
START_MIN=$((CURRENT_MIN + 5))
START_HOUR=$CURRENT_HOUR

# Handle minute overflow
if [ $START_MIN -ge 60 ]; then
    START_MIN=$((START_MIN - 60))
    START_HOUR=$((START_HOUR + 1))
fi

# End time is 30 minutes after start
END_MIN=$((START_MIN + 30))
END_HOUR=$START_HOUR
if [ $END_MIN -ge 60 ]; then
    END_MIN=$((END_MIN - 60))
    END_HOUR=$((END_HOUR + 1))
fi

echo "Schedule: $START_HOUR:$(printf %02d $START_MIN) - $END_HOUR:$(printf %02d $END_MIN)"
echo

# Create JSON payload
JSON=$(cat <<EOF
{
  "name": "Test Pre-heat",
  "days": $DAYMASK,
  "start_hour": $START_HOUR,
  "start_minute": $START_MIN,
  "end_hour": $END_HOUR,
  "end_minute": $END_MIN,
  "enabled": true
}
EOF
)

echo "Sending schedule:"
echo "$JSON" | jq '.'
echo

# Send to legacy topic
mosquitto_pub -h $BROKER -u $USER -P $PASS -t "hotwater/schedule/add" -m "$JSON"
echo "Schedule sent to hotwater/schedule/add"

# Wait and check status
echo
echo "Waiting 5 seconds for processing..."
sleep 5

echo "Requesting status..."
mosquitto_pub -h $BROKER -u $USER -P $PASS -t "hotwater/control" -m "status"
sleep 2

echo
echo "Current hot water status:"
mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -C 1 -W 5 | jq '.' || echo "No response received"

echo
echo "Monitor for pre-heating (Ctrl+C to stop):"
while true; do
    mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -C 1 -W 10 | jq -r '"[\(.time)] Preheating: \(.preheating), Active: \(.active), Temp: \(.waterTemp)°C, Ready: \(.estimatedReady)"' || true
    sleep 30
done