#!/bin/bash

BROKER="192.168.20.27"
USER="YOUR_MQTT_USER"
PASS="YOUR_MQTT_PASSWORD"

echo "=== Pre-heating Test Demo ==="
echo "Current local time: $(date '+%H:%M')"
echo

# Get current day of week (0=Sunday, 4=Thursday)
DOW=$(date +%w)
DAYMASK=$((1 << DOW))
echo "Today is $(date +%A), using day mask: $DAYMASK"

# Set schedule for 10 minutes from now to allow pre-heating to kick in
CURRENT_HOUR=$(date +%-H)
CURRENT_MIN=$(date +%-M)
START_MIN=$((CURRENT_MIN + 10))
START_HOUR=$CURRENT_HOUR

# Handle minute overflow
if [ $START_MIN -ge 60 ]; then
    START_MIN=$((START_MIN - 60))
    START_HOUR=$((START_HOUR + 1))
    if [ $START_HOUR -ge 24 ]; then
        START_HOUR=0
    fi
fi

# End time is 30 minutes after start
END_MIN=$((START_MIN + 30))
END_HOUR=$START_HOUR
if [ $END_MIN -ge 60 ]; then
    END_MIN=$((END_MIN - 60))
    END_HOUR=$((END_HOUR + 1))
    if [ $END_HOUR -ge 24 ]; then
        END_HOUR=0
    fi
fi

echo "Creating schedule for: $(printf %02d:%02d $START_HOUR $START_MIN) - $(printf %02d:%02d $END_HOUR $END_MIN)"
echo "Pre-heating should start in ~5-7 minutes"
echo

# Create JSON payload
JSON=$(cat <<EOF
{
  "name": "Pre-heat Demo",
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
echo "$JSON" | python3 -m json.tool
echo

# Send schedule
mosquitto_pub -h $BROKER -u $USER -P $PASS -t "hotwater/schedule/add" -m "$JSON"
echo "✓ Schedule sent"

# Request immediate status
sleep 2
mosquitto_pub -h $BROKER -u $USER -P $PASS -t "hotwater/control" -m "status"
sleep 1

echo
echo "Current status:"
mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -C 1 -W 5 | python3 -m json.tool || echo "No response"

echo
echo "=== Monitoring Pre-heating (updates every 30s) ==="
echo "Watch for 'preheating: true' to appear in ~5-7 minutes"
echo "Press Ctrl+C to stop"
echo

# Monitor loop
while true; do
    STATUS=$(mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -C 1 -W 10 2>/dev/null || echo "{}")
    if [ -n "$STATUS" ] && [ "$STATUS" != "{}" ]; then
        TIME=$(echo "$STATUS" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('time','??:??'))" 2>/dev/null || echo "??:??")
        PREHEAT=$(echo "$STATUS" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('preheating',False))" 2>/dev/null || echo "false")
        ACTIVE=$(echo "$STATUS" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('active',False))" 2>/dev/null || echo "false")
        TEMP=$(echo "$STATUS" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('waterTemp',0))" 2>/dev/null || echo "0")
        READY=$(echo "$STATUS" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('estimatedReady','N/A'))" 2>/dev/null || echo "N/A")
        SCHEDULE=$(echo "$STATUS" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('scheduleName',''))" 2>/dev/null || echo "")
        
        if [ "$PREHEAT" = "True" ]; then
            echo "[$TIME] 🔥 PRE-HEATING ACTIVE! Schedule: $SCHEDULE, Water: ${TEMP}°C, Ready by: $READY"
        elif [ "$ACTIVE" = "True" ]; then
            echo "[$TIME] ✓ SCHEDULE ACTIVE! Water: ${TEMP}°C"
        else
            echo "[$TIME] Waiting... Water: ${TEMP}°C (Pre-heat starts ~5-7 min before $START_HOUR:$(printf %02d $START_MIN))"
        fi
    else
        echo "[$(date +%H:%M)] No response from device"
    fi
    sleep 30
done