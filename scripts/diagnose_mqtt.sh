#!/bin/bash

# MQTT Diagnostics Script
# Helps diagnose MQTT connection issues

MQTT_HOST="192.168.20.27"
MQTT_USER="YOUR_MQTT_USER"
MQTT_PASS="YOUR_MQTT_PASSWORD"

echo "MQTT Diagnostics"
echo "================"
echo

# 1. Check network connectivity
echo "1. Checking network connectivity to MQTT broker..."
if ping -c 3 -W 2 $MQTT_HOST >/dev/null 2>&1; then
    echo "   ✓ Network connectivity OK"
else
    echo "   ✗ Cannot reach MQTT broker at $MQTT_HOST"
    echo "   Check your network connection and broker IP"
    exit 1
fi

# 2. Check MQTT broker port
echo -e "\n2. Checking MQTT broker port (1883)..."
if nc -zv -w 5 $MQTT_HOST 1883 2>&1 | grep -q "succeeded"; then
    echo "   ✓ MQTT port 1883 is open"
else
    echo "   ✗ MQTT port 1883 is not reachable"
    echo "   Check if MQTT broker is running"
    exit 1
fi

# 3. Test MQTT authentication
echo -e "\n3. Testing MQTT authentication..."
if mosquitto_sub -h $MQTT_HOST -u $MQTT_USER -P $MQTT_PASS -t "\$SYS/#" -C 1 -W 2 >/dev/null 2>&1; then
    echo "   ✓ MQTT authentication successful"
else
    echo "   ✗ MQTT authentication failed"
    echo "   Check username and password"
    exit 1
fi

# 4. Subscribe to ESP32 topics
echo -e "\n4. Monitoring ESP32 MQTT activity..."
echo "   Subscribing to all ESP32 topics for 30 seconds..."
echo "   Look for:"
echo "   - Connection status messages"
echo "   - Any successful publishes"
echo "   - Error messages"
echo

timeout 30 mosquitto_sub -h $MQTT_HOST -u $MQTT_USER -P $MQTT_PASS -t "#" -v | grep -E "(boiler|esplan|system/|sensors/)" || true

echo -e "\n5. Testing direct publish/subscribe..."
TEST_TOPIC="test/boiler/diagnostic"
TEST_MESSAGE="Test message $(date +%s)"

# Start subscriber in background
mosquitto_sub -h $MQTT_HOST -u $MQTT_USER -P $MQTT_PASS -t "$TEST_TOPIC" -C 1 -W 5 -v > /tmp/mqtt_test.log 2>&1 &
SUB_PID=$!

# Give subscriber time to connect
sleep 1

# Publish test message
mosquitto_pub -h $MQTT_HOST -u $MQTT_USER -P $MQTT_PASS -t "$TEST_TOPIC" -m "$TEST_MESSAGE"

# Wait for subscriber to receive
wait $SUB_PID 2>/dev/null

if grep -q "$TEST_MESSAGE" /tmp/mqtt_test.log 2>/dev/null; then
    echo "   ✓ MQTT publish/subscribe test successful"
else
    echo "   ✗ MQTT publish/subscribe test failed"
fi

rm -f /tmp/mqtt_test.log

echo -e "\nDiagnostics complete!"
echo "===================="
echo
echo "If all tests passed but ESP32 still can't publish:"
echo "1. Check ESP32 serial logs for connection errors"
echo "2. Verify ESP32 network configuration (IP, gateway, DNS)"
echo "3. Check if ESP32 hostname 'boiler-Boiler' is already connected"
echo "4. Try power cycling the ESP32"
echo "5. Check MQTT broker logs for connection attempts"