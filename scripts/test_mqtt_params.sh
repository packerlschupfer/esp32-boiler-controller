#!/bin/bash

# MQTT connection parameters
MQTT_HOST="192.168.20.27"
MQTT_USER="YOUR_MQTT_USER"
MQTT_PASS="YOUR_MQTT_PASSWORD"
MQTT_TOPIC_PREFIX="boiler/params"

echo "Testing MQTT parameter system..."
echo "================================"

# First, check if MQTT broker is reachable
echo -n "1. Checking MQTT broker connectivity... "
if mosquitto_sub -h $MQTT_HOST -u $MQTT_USER -P $MQTT_PASS -t "\$SYS/#" -C 1 -W 2 >/dev/null 2>&1; then
    echo "OK"
else
    echo "FAILED"
    echo "   Cannot connect to MQTT broker at $MQTT_HOST"
    exit 1
fi

# Subscribe to parameter status topics in background
echo "2. Starting parameter status monitor..."
mosquitto_sub -h $MQTT_HOST -u $MQTT_USER -P $MQTT_PASS -t "$MQTT_TOPIC_PREFIX/status/#" -v &
SUB_PID=$!

# Give subscriber time to connect
sleep 1

# Test getting all parameters
echo -e "\n3. Requesting all parameters..."
mosquitto_pub -h $MQTT_HOST -u $MQTT_USER -P $MQTT_PASS -t "$MQTT_TOPIC_PREFIX/get/all" -m ""

# Wait for responses
echo "   Waiting for responses (10 seconds)..."
sleep 10

# Test getting a specific parameter
echo -e "\n4. Requesting specific parameter (heating/targetTemp)..."
mosquitto_pub -h $MQTT_HOST -u $MQTT_USER -P $MQTT_PASS -t "$MQTT_TOPIC_PREFIX/get/heating/targetTemp" -m ""

# Wait for response
sleep 2

# Test setting a parameter
echo -e "\n5. Setting heating/targetTemp to 22.5..."
mosquitto_pub -h $MQTT_HOST -u $MQTT_USER -P $MQTT_PASS -t "$MQTT_TOPIC_PREFIX/set/heating/targetTemp" -m '{"value": 22.5}'

# Wait for response
sleep 2

# List all parameters
echo -e "\n6. Requesting parameter list..."
mosquitto_pub -h $MQTT_HOST -u $MQTT_USER -P $MQTT_PASS -t "$MQTT_TOPIC_PREFIX/list" -m ""

# Wait for response
sleep 2

# Kill the subscriber
echo -e "\n7. Stopping monitor..."
kill $SUB_PID 2>/dev/null

echo -e "\nTest complete!"
echo "=============="
echo "If you saw parameter values above, the system is working correctly."
echo "If you only saw 'Failed to publish' errors, check:"
echo "  - Is the ESP32 connected to the network?"
echo "  - Is MQTT enabled in the ESP32 configuration?"
echo "  - Has the ESP32 successfully connected to the MQTT broker?"