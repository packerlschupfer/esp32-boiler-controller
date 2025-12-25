#!/bin/bash

# MQTT broker is at .16, device is at .138
BROKER="192.168.20.27"
USER="YOUR_MQTT_USER"
PASS="YOUR_MQTT_PASSWORD"

echo "=== Pre-heating Demonstration ==="
echo "Current time: $(date '+%H:%M')"
echo "Device IP: 192.168.20.40"
echo "MQTT Broker: $BROKER"
echo

# Get current day of week
DOW=$(date +%w)
DAYMASK=$((1 << DOW))

# Schedule for 15 minutes from now to allow pre-heating
CURRENT_HOUR=$(date +%-H)
CURRENT_MIN=$(date +%-M)
START_MIN=$((CURRENT_MIN + 15))
START_HOUR=$CURRENT_HOUR

if [ $START_MIN -ge 60 ]; then
    START_MIN=$((START_MIN - 60))
    START_HOUR=$((START_HOUR + 1))
    if [ $START_HOUR -ge 24 ]; then
        START_HOUR=0
    fi
fi

END_MIN=$((START_MIN + 30))
END_HOUR=$START_HOUR
if [ $END_MIN -ge 60 ]; then
    END_MIN=$((END_MIN - 60))
    END_HOUR=$((END_HOUR + 1))
    if [ $END_HOUR -ge 24 ]; then
        END_HOUR=0
    fi
fi

echo "Creating test schedule:"
echo "  Day: $(date +%A) (mask $DAYMASK)"
echo "  Start: $(printf %02d:%02d $START_HOUR $START_MIN)"
echo "  End: $(printf %02d:%02d $END_HOUR $END_MIN)"
echo "  Pre-heating should start ~10 minutes before start time"
echo

JSON=$(cat <<EOF
{
  "name": "Demo Shower",
  "days": $DAYMASK,
  "start_hour": $START_HOUR,
  "start_minute": $START_MIN,
  "end_hour": $END_HOUR,
  "end_minute": $END_MIN,
  "enabled": true
}
EOF
)

echo "Sending schedule..."
if mosquitto_pub -h $BROKER -u $USER -P $PASS -t "hotwater/schedule/add" -m "$JSON"; then
    echo "✓ Schedule sent successfully"
else
    echo "✗ Failed to send schedule"
    exit 1
fi

# Request status
sleep 2
echo
echo "Requesting status..."
mosquitto_pub -h $BROKER -u $USER -P $PASS -t "hotwater/control" -m "status"

# Get status
sleep 2
echo
echo "Current device status:"
if STATUS=$(mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -C 1 -W 5 2>/dev/null); then
    echo "$STATUS" | python3 -m json.tool
else
    echo "No response from device"
fi

echo
echo "To monitor pre-heating, run:"
echo "watch -n 30 'mosquitto_sub -h $BROKER -u $USER -P $PASS -t \"hotwater/status\" -C 1 -W 5 | python3 -m json.tool'"