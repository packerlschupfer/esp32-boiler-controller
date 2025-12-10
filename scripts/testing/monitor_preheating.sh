#!/bin/bash

BROKER="192.168.20.27"
USER="YOUR_MQTT_USER"
PASS="YOUR_MQTT_PASSWORD"

echo "=== Monitoring Pre-heating Status ==="
echo "Schedule set for 2:15 AM"
echo "Pre-heating should start around 1:35 AM (40 minutes before)"
echo "Press Ctrl+C to stop"
echo

while true; do
    STATUS=$(mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -C 1 -W 5 2>/dev/null || echo "{}")
    
    if [ -n "$STATUS" ] && [ "$STATUS" != "{}" ]; then
        TIME=$(echo "$STATUS" | grep -o '"time":"[^"]*"' | cut -d'"' -f4)
        PREHEAT=$(echo "$STATUS" | grep -o '"preheating":[^,]*' | cut -d':' -f2)
        ACTIVE=$(echo "$STATUS" | grep -o '"active":[^,]*' | cut -d':' -f2)
        TEMP=$(echo "$STATUS" | grep -o '"waterTemp":[^,]*' | cut -d':' -f2)
        READY=$(echo "$STATUS" | grep -o '"estimatedReady":"[^"]*"' | cut -d'"' -f4)
        SCHEDULE=$(echo "$STATUS" | grep -o '"scheduleName":"[^"]*"' | cut -d'"' -f4)
        
        if [ "$PREHEAT" = "true" ]; then
            echo "[$TIME] 🔥 PRE-HEATING ACTIVE! Schedule: $SCHEDULE, Water: ${TEMP}°C, Ready by: $READY"
        elif [ "$ACTIVE" = "true" ]; then
            echo "[$TIME] ✓ Schedule active: $SCHEDULE, Water: ${TEMP}°C"
        else
            echo "[$TIME] ⏳ Waiting for pre-heat (starts ~1:35), Water: ${TEMP}°C"
        fi
    else
        echo "[$(date +%H:%M)] No response"
    fi
    
    sleep 30
done