#!/bin/bash

# OTA Update Script for ESPlan Boiler Controller
# Simple script to perform OTA firmware updates

# Configuration
DEVICE_IP="${1:-}"
OTA_PORT="${OTA_PORT:-3232}"
OTA_PASSWORD="${OTA_PASSWORD:-update-password}"
BUILD_ENV="${2:-esp32dev_ota_release}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=== ESPlan Boiler Controller OTA Update ==="
echo ""

# Check if IP address provided
if [ -z "$DEVICE_IP" ]; then
    echo -e "${RED}Error: Device IP address required${NC}"
    echo "Usage: $0 <device_ip> [build_env]"
    echo "Example: $0 192.168.1.100"
    echo "         $0 192.168.1.100 esp32dev_ota_debug_selective"
    exit 1
fi

# Check if platformio is installed
if ! command -v pio &> /dev/null; then
    echo -e "${RED}Error: PlatformIO not found${NC}"
    echo "Please install PlatformIO first"
    exit 1
fi

echo "Target Device: $DEVICE_IP:$OTA_PORT"
echo "Build Environment: $BUILD_ENV"
echo ""

# Step 1: Build firmware
echo -e "${YELLOW}Step 1: Building firmware...${NC}"
if pio run -e "$BUILD_ENV"; then
    echo -e "${GREEN}✓ Build successful${NC}"
else
    echo -e "${RED}✗ Build failed${NC}"
    exit 1
fi

# Get firmware path
FIRMWARE_PATH=".pio/build/$BUILD_ENV/firmware.bin"

if [ ! -f "$FIRMWARE_PATH" ]; then
    echo -e "${RED}Error: Firmware file not found at $FIRMWARE_PATH${NC}"
    exit 1
fi

# Show firmware info
FIRMWARE_SIZE=$(stat -f%z "$FIRMWARE_PATH" 2>/dev/null || stat -c%s "$FIRMWARE_PATH" 2>/dev/null)
FIRMWARE_SIZE_KB=$((FIRMWARE_SIZE / 1024))
echo "Firmware size: ${FIRMWARE_SIZE_KB} KB"

# Calculate MD5 checksum
if command -v md5sum &> /dev/null; then
    CHECKSUM=$(md5sum "$FIRMWARE_PATH" | cut -d' ' -f1)
elif command -v md5 &> /dev/null; then
    CHECKSUM=$(md5 -q "$FIRMWARE_PATH")
else
    CHECKSUM="(unable to calculate)"
fi
echo "MD5 checksum: $CHECKSUM"
echo ""

# Step 2: Check device connectivity
echo -e "${YELLOW}Step 2: Checking device connectivity...${NC}"
if ping -c 1 -W 2 "$DEVICE_IP" > /dev/null 2>&1; then
    echo -e "${GREEN}✓ Device is reachable${NC}"
else
    echo -e "${RED}✗ Device is not reachable${NC}"
    echo "Please check the IP address and network connection"
    exit 1
fi

# Step 3: Find espota.py
echo -e "${YELLOW}Step 3: Locating OTA upload tool...${NC}"
ESPOTA_PATHS=(
    ".platformio/packages/framework-arduinoespressif32/tools/espota.py"
    "$HOME/.platformio/packages/framework-arduinoespressif32/tools/espota.py"
    "/usr/local/bin/espota.py"
)

ESPOTA_PATH=""
for path in "${ESPOTA_PATHS[@]}"; do
    if [ -f "$path" ]; then
        ESPOTA_PATH="$path"
        break
    fi
done

if [ -z "$ESPOTA_PATH" ]; then
    echo -e "${RED}Error: espota.py not found${NC}"
    echo "Please ensure framework-arduinoespressif32 is installed"
    exit 1
fi

echo -e "${GREEN}✓ Found espota.py${NC}"
echo ""

# Step 4: Perform OTA update
echo -e "${YELLOW}Step 4: Uploading firmware via OTA...${NC}"
echo "This may take 1-2 minutes..."
echo ""

# Run OTA update
python3 "$ESPOTA_PATH" \
    -i "$DEVICE_IP" \
    -p "$OTA_PORT" \
    -a "$OTA_PASSWORD" \
    -f "$FIRMWARE_PATH" \
    --progress

if [ $? -eq 0 ]; then
    echo ""
    echo -e "${GREEN}✓ OTA update successful!${NC}"
    echo ""
    echo "The device will reboot automatically."
    echo "Please wait 10-15 seconds before accessing the device."
else
    echo ""
    echo -e "${RED}✗ OTA update failed${NC}"
    echo ""
    echo "Troubleshooting tips:"
    echo "1. Check if OTA password is correct (current: $OTA_PASSWORD)"
    echo "2. Ensure device has enough free memory for update"
    echo "3. Verify device is not in the middle of critical operation"
    echo "4. Check if another OTA update is already in progress"
    exit 1
fi

# Optional: Monitor device after update
echo ""
read -p "Monitor device after update? (y/n) " -n 1 -r
echo ""
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Waiting for device to reboot..."
    sleep 10
    
    echo "Checking device status..."
    for i in {1..10}; do
        if ping -c 1 -W 2 "$DEVICE_IP" > /dev/null 2>&1; then
            echo -e "${GREEN}✓ Device is back online${NC}"
            break
        else
            echo "Attempt $i/10: Device not responding yet..."
            sleep 2
        fi
    done
fi

echo ""
echo "OTA update process complete!"