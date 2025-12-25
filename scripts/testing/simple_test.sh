#!/bin/bash

BROKER="192.168.20.27"
USER="YOUR_MQTT_USER"
PASS="YOUR_MQTT_PASSWORD"

echo "=== Pre-heating Test Suite ==="
echo

# Test 1: Check current status
echo "1. Current Status:"
mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -C 1 | jq '.'
echo

# Test 2: Change heating rate
echo "2. Testing heating rate change:"
echo "   Setting rate to 2.0°C/min..."
mosquitto_pub -h $BROKER -u $USER -P $PASS -t "boiler/params/set/wheater/heatingRate" -m "2.0"
sleep 2
echo "   Checking status..."
mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -C 1 | jq '.heatingRate'
echo

# Test 3: Manual control
echo "3. Testing manual control:"
echo "   Turning OFF..."
mosquitto_pub -h $BROKER -u $USER -P $PASS -t "hotwater/control" -m "off"
sleep 5
echo "   Status:"
mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -C 1 | jq '{active,preheating}'

echo "   Turning ON..."
mosquitto_pub -h $BROKER -u $USER -P $PASS -t "hotwater/control" -m "on"
sleep 5
echo "   Status:"
mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -C 1 | jq '{active,preheating}'
echo

# Test 4: Add test schedule
echo "4. Adding test schedule for 30 minutes from now:"
SCHEDULE_TIME=$(date -d "+30 minutes" +"%H:%M")
mosquitto_pub -h $BROKER -u $USER -P $PASS -t "hotwater/schedule/add" -m "{\"name\":\"Test Schedule\",\"enabled\":true,\"days\":[1,2,3,4,5,6,7],\"startTime\":\"$SCHEDULE_TIME\",\"duration\":30}"
echo "   Schedule set for $SCHEDULE_TIME"
echo

# Test 5: Monitor temperature
echo "5. Monitoring water temperature (5 checks, 20s apart):"
for i in {1..5}; do
    echo -n "   Check $i: "
    TEMP=$(mosquitto_sub -h $BROKER -u $USER -P $PASS -t "sensors/status" -C 1 | jq -r '.t.wt/10')
    STATUS=$(mosquitto_sub -h $BROKER -u $USER -P $PASS -t "hotwater/status" -C 1 | jq -c '{waterTemp,preheating,estimatedReady}')
    echo "Tank: ${TEMP}°C | $STATUS"
    sleep 20
done

echo
echo "=== Test Suite Complete ==="