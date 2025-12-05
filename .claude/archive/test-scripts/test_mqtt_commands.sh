#!/bin/bash

# MQTT Remote Control Test Script
# Tests all major MQTT control commands for the ESPlan Boiler Controller

MQTT_HOST="192.168.16.16"
MQTT_USER="YOUR_MQTT_USER"
MQTT_PASS="YOUR_MQTT_PASSWORD"
MQTT_TOPIC_PREFIX="cmd/boiler"

echo "=== ESPlan Boiler Controller MQTT Remote Control Test ==="
echo "Host: $MQTT_HOST"
echo "User: $MQTT_USER"
echo ""

# Function to send MQTT command
send_command() {
    local topic="$1"
    local message="$2"
    local description="$3"
    
    echo "[$description]"
    echo "Topic: $topic"
    echo "Message: $message"
    mosquitto_pub -h "$MQTT_HOST" -u "$MQTT_USER" -P "$MQTT_PASS" -t "$topic" -m "$message" -q 1
    echo "✓ Sent"
    echo ""
    sleep 2
}

# Test 1: Boiler Control
echo "=== Test 1: Boiler Control ==="
send_command "$MQTT_TOPIC_PREFIX/enable" "on" "Enable boiler"
sleep 3
send_command "$MQTT_TOPIC_PREFIX/enable" "off" "Disable boiler"
sleep 3
send_command "$MQTT_TOPIC_PREFIX/enable" "on" "Re-enable boiler"

# Test 2: Heating Control
echo -e "\n=== Test 2: Space Heating Control ==="
send_command "$MQTT_TOPIC_PREFIX/heating" "on" "Enable space heating"
sleep 3
send_command "$MQTT_TOPIC_PREFIX/heating" "off" "Disable space heating"
sleep 3
send_command "$MQTT_TOPIC_PREFIX/heating" "on" "Re-enable space heating"

# Test 3: Water Heating Control
echo -e "\n=== Test 3: Water Heating Control ==="
send_command "$MQTT_TOPIC_PREFIX/water" "on" "Enable water heating"
sleep 3
send_command "$MQTT_TOPIC_PREFIX/water" "off" "Disable water heating"
sleep 3
send_command "$MQTT_TOPIC_PREFIX/water" "on" "Re-enable water heating"

# Test 4: Water Heating Priority
echo -e "\n=== Test 4: Water Heating Priority ==="
send_command "$MQTT_TOPIC_PREFIX/water/priority" "on" "Enable water heating priority"
sleep 3
send_command "$MQTT_TOPIC_PREFIX/water/priority" "off" "Disable water heating priority"

# Test 5: Temperature Settings
echo -e "\n=== Test 5: Temperature Settings ==="
send_command "$MQTT_TOPIC_PREFIX/params/set/targetTemperatureInside" "21.5" "Set target inside temperature to 21.5°C"
send_command "$MQTT_TOPIC_PREFIX/params/set/heating_hysteresis" "2.0" "Set heating hysteresis to 2.0°C"

# Test 6: Water Heater Limits
echo -e "\n=== Test 6: Water Heater Temperature Limits ==="
send_command "$MQTT_TOPIC_PREFIX/params/set/wHeaterConfTempLimitLow" "45.0" "Set water heater low limit to 45°C"
send_command "$MQTT_TOPIC_PREFIX/params/set/wHeaterConfTempLimitHigh" "55.0" "Set water heater high limit to 55°C"
send_command "$MQTT_TOPIC_PREFIX/params/set/wHeaterConfTempHyst" "3.0" "Set water heater hysteresis to 3°C"

# Test 7: System Queries
echo -e "\n=== Test 7: System Status Queries ==="
echo "Subscribing to diagnostics topics for 10 seconds..."
timeout 10 mosquitto_sub -h "$MQTT_HOST" -u "$MQTT_USER" -P "$MQTT_PASS" -t "$MQTT_TOPIC_PREFIX/diagnostics/+" -v &

# Also subscribe to state topics
timeout 10 mosquitto_sub -h "$MQTT_HOST" -u "$MQTT_USER" -P "$MQTT_PASS" -t "$MQTT_TOPIC_PREFIX/state/+" -v &

# Request diagnostics
send_command "$MQTT_TOPIC_PREFIX/diagnostics/request" "all" "Request all diagnostics"

# Test 8: Parameter List
echo -e "\n=== Test 8: Parameter Management ==="
send_command "$MQTT_TOPIC_PREFIX/params/list" "" "Request parameter list"
sleep 2
send_command "$MQTT_TOPIC_PREFIX/params/save" "" "Save parameters to NVS"

# Test 9: Emergency Stop
echo -e "\n=== Test 9: Emergency Stop Test ==="
send_command "$MQTT_TOPIC_PREFIX/emergency/stop" "on" "Trigger emergency stop"
sleep 3
send_command "$MQTT_TOPIC_PREFIX/emergency/stop" "off" "Clear emergency stop"

echo -e "\n=== Test Complete ==="
echo "Monitor the device logs and MQTT topics to verify all commands were processed correctly."
echo ""
echo "To monitor all MQTT traffic:"
echo "mosquitto_sub -h $MQTT_HOST -u $MQTT_USER -P $MQTT_PASS -t 'cmd/boiler/#' -v"