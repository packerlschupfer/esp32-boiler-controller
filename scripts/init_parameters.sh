#!/bin/bash

# ESPlan Boiler Controller - Parameter Initialization Script
# This script sets all configurable parameters to sensible default values

# MQTT Connection Settings
MQTT_HOST="${MQTT_HOST:-192.168.20.27}"
MQTT_USER="${MQTT_USER:-YOUR_MQTT_USER}"
MQTT_PASS="${MQTT_PASS:-YOUR_MQTT_PASSWORD}"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Function to publish MQTT message
publish_param() {
    local topic="$1"
    local value="$2"
    local description="$3"
    
    echo -e "${YELLOW}Setting${NC} $description: ${GREEN}$value${NC}"
    mosquitto_pub -h "$MQTT_HOST" -u "$MQTT_USER" -P "$MQTT_PASS" \
        -t "boiler/params/set/$topic" \
        -m "{\"value\": $value}" 2>/dev/null
    
    if [ $? -eq 0 ]; then
        echo -e "  ✓ Success"
    else
        echo -e "  ${RED}✗ Failed${NC}"
    fi
    
    # Small delay to avoid overwhelming the system
    sleep 0.2
}

echo "================================================"
echo "ESPlan Boiler Controller Parameter Initialization"
echo "================================================"
echo "MQTT Broker: $MQTT_HOST"
echo "MQTT User: $MQTT_USER"
echo ""

# Test MQTT connection
echo -n "Testing MQTT connection... "
mosquitto_pub -h "$MQTT_HOST" -u "$MQTT_USER" -P "$MQTT_PASS" -t "test/connection" -m "test" 2>/dev/null
if [ $? -eq 0 ]; then
    echo -e "${GREEN}OK${NC}"
else
    echo -e "${RED}FAILED${NC}"
    echo "Please check your MQTT connection settings"
    exit 1
fi
echo ""

# Monitor responses in background (optional)
if command -v gnome-terminal &> /dev/null; then
    echo "Opening monitor window..."
    gnome-terminal -- bash -c "mosquitto_sub -v -h $MQTT_HOST -u $MQTT_USER -P $MQTT_PASS -t 'boiler/params/#'; read"
elif command -v xterm &> /dev/null; then
    xterm -e "mosquitto_sub -v -h $MQTT_HOST -u $MQTT_USER -P $MQTT_PASS -t 'boiler/params/#'" &
else
    echo "Tip: Run this in another terminal to monitor responses:"
    echo "mosquitto_sub -v -h $MQTT_HOST -u $MQTT_USER -P $MQTT_PASS -t 'boiler/params/#'"
fi
echo ""

# ============================================
# WATER HEATER CONFIGURATION
# ============================================
echo -e "${GREEN}=== Water Heater Configuration ===${NC}"

publish_param "wheater/priorityEnabled" "true" "Water heating priority"
publish_param "wheater/tempLimitLow" "35.0" "Water heater low temp limit (°C)"
publish_param "wheater/tempLimitHigh" "60.0" "Water heater high temp limit (°C)"
publish_param "wheater/tempChargeDelta" "5.0" "Water heater charging delta (°C)"
publish_param "wheater/tempSafeLimitHigh" "80.0" "Water heater safety high limit (°C)"
publish_param "wheater/tempSafeLimitLow" "5.0" "Water heater safety low limit (°C)"
publish_param "wheater/hysteresis" "2.0" "Water heater hysteresis (°C)"

echo ""

# ============================================
# HEATING CONFIGURATION
# ============================================
echo -e "${GREEN}=== Heating Configuration ===${NC}"

publish_param "heating/targetTemp" "21.0" "Target indoor temperature (°C)"
publish_param "heating/curveShift" "20.0" "Heating curve shift"
publish_param "heating/curveCoeff" "1.5" "Heating curve coefficient"
publish_param "heating/burnerLowLimit" "35.0" "Minimum burner temperature (°C)"
publish_param "heating/highLimit" "75.0" "Maximum heating temperature (°C)"
publish_param "heating/hysteresis" "0.5" "Heating hysteresis (°C)"

echo ""

# ============================================
# PID PARAMETERS - SPACE HEATING
# ============================================
echo -e "${GREEN}=== PID Parameters - Space Heating ===${NC}"

publish_param "pid/spaceHeating/kp" "2.0" "Space heating proportional gain"
publish_param "pid/spaceHeating/ki" "0.5" "Space heating integral gain"
publish_param "pid/spaceHeating/kd" "0.1" "Space heating derivative gain"

echo ""

# ============================================
# PID PARAMETERS - WATER HEATER
# ============================================
echo -e "${GREEN}=== PID Parameters - Water Heater ===${NC}"

publish_param "pid/waterHeater/kp" "1.5" "Water heater proportional gain"
publish_param "pid/waterHeater/ki" "0.3" "Water heater integral gain"
publish_param "pid/waterHeater/kd" "0.05" "Water heater derivative gain"

echo ""

# ============================================
# SENSOR CONFIGURATION
# ============================================
echo -e "${GREEN}=== Sensor Configuration ===${NC}"

publish_param "sensor/miThInterval" "5000" "MiThermometer update interval (ms)"

echo ""

# ============================================
# SAVE ALL PARAMETERS
# ============================================
echo -e "${GREEN}=== Saving Parameters to Flash ===${NC}"
echo "Saving all parameters to non-volatile storage..."
mosquitto_pub -h "$MQTT_HOST" -u "$MQTT_USER" -P "$MQTT_PASS" \
    -t "boiler/params/save" -m "" 2>/dev/null

if [ $? -eq 0 ]; then
    echo -e "  ✓ Parameters saved successfully"
else
    echo -e "  ${RED}✗ Failed to save parameters${NC}"
fi

echo ""
echo "================================================"
echo -e "${GREEN}Parameter initialization complete!${NC}"
echo "================================================"
echo ""
echo "You can verify parameters by requesting them:"
echo "  mosquitto_pub -h $MQTT_HOST -u $MQTT_USER -P $MQTT_PASS -t 'boiler/params/list' -m ''"
echo ""
echo "Or get a specific parameter:"
echo "  mosquitto_pub -h $MQTT_HOST -u $MQTT_USER -P $MQTT_PASS -t 'boiler/params/get/heating/targetTemp' -m ''"
echo ""