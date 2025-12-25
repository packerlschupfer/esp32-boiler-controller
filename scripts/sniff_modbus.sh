#!/bin/bash
# Modbus RTU Sniffer for RS485 Bus
# Captures and decodes traffic between ESP32 and ANDRTF3 sensor
#
# Hardware setup:
#   - Connect USB-RS485 adapter A/B to the bus
#   - DO NOT connect TX (receive-only mode)
#
# Usage: ./sniff_modbus.sh [device] [logfile]

set -u

DEVICE="${1:-/dev/ttyUSB485}"
LOGFILE="${2:-modbus_capture_$(date +%Y%m%d_%H%M%S).log}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# Modbus function codes
declare -A FUNC_CODES=(
    [1]="Read Coils"
    [2]="Read Discrete Inputs"
    [3]="Read Holding Registers"
    [4]="Read Input Registers"
    [5]="Write Single Coil"
    [6]="Write Single Register"
    [15]="Write Multiple Coils"
    [16]="Write Multiple Registers"
)

# Exception codes
declare -A EXCEPTION_CODES=(
    [1]="Illegal Function"
    [2]="Illegal Data Address"
    [3]="Illegal Data Value"
    [4]="Slave Device Failure"
    [5]="Acknowledge"
    [6]="Slave Device Busy"
    [8]="Memory Parity Error"
)

echo -e "${BLUE}======================================${NC}"
echo -e "${BLUE}Modbus RTU Sniffer${NC}"
echo -e "${BLUE}======================================${NC}"
echo "Device:  $DEVICE"
echo "Logfile: $LOGFILE"
echo ""
echo -e "${YELLOW}IMPORTANT: USB-RS485 adapter must be in receive-only mode!${NC}"
echo -e "${YELLOW}           (TX line disconnected or floating)${NC}"
echo ""
echo "Press Ctrl+C to stop"
echo ""

# Check device exists
if [ ! -e "$DEVICE" ]; then
    echo -e "${RED}Error: Device $DEVICE not found${NC}"
    exit 1
fi

# Configure serial port: 9600 8E1 (matching ANDRTF3 default)
stty -F "$DEVICE" 9600 cs8 parenb -parodd -cstopb raw -echo

# Calculate CRC16 Modbus
calc_crc() {
    local data="$1"
    local crc=65535  # 0xFFFF

    for ((i=0; i<${#data}; i+=2)); do
        local byte=$((16#${data:$i:2}))
        crc=$((crc ^ byte))

        for ((j=0; j<8; j++)); do
            if ((crc & 1)); then
                crc=$(((crc >> 1) ^ 0xA001))
            else
                crc=$((crc >> 1))
            fi
        done
    done

    # Return as low-high (Modbus byte order)
    printf "%02X%02X" $((crc & 0xFF)) $(((crc >> 8) & 0xFF))
}

# Decode a Modbus frame
decode_frame() {
    local hex="$1"
    local timestamp="$2"
    local len=$((${#hex} / 2))

    if [ $len -lt 4 ]; then
        echo -e "${YELLOW}[$timestamp] Incomplete frame (${len} bytes): $hex${NC}"
        return
    fi

    local addr=$((16#${hex:0:2}))
    local func=$((16#${hex:2:2}))
    local is_exception=0

    # Check for exception response (function code with high bit set)
    if [ $func -ge 128 ]; then
        is_exception=1
        func=$((func - 128))
    fi

    local func_name="${FUNC_CODES[$func]:-Unknown}"

    # Extract CRC (last 2 bytes)
    local data_end=$((${#hex} - 4))
    local frame_crc="${hex:$data_end:4}"
    local data_hex="${hex:0:$data_end}"

    # Verify CRC
    local calc_crc_val=$(calc_crc "$data_hex")
    local crc_ok="OK"
    local crc_color="$GREEN"
    if [ "$frame_crc" != "$calc_crc_val" ]; then
        crc_ok="BAD (expected $calc_crc_val)"
        crc_color="$RED"
    fi

    # Log raw frame
    echo "[$timestamp] RAW: $hex" >> "$LOGFILE"

    if [ $is_exception -eq 1 ]; then
        # Exception response
        local exc_code=$((16#${hex:4:2}))
        local exc_name="${EXCEPTION_CODES[$exc_code]:-Unknown}"
        echo -e "${RED}[$timestamp] EXCEPTION from addr $addr: $exc_name (code $exc_code)${NC}"
        echo "[$timestamp] EXCEPTION: addr=$addr func=$func exc=$exc_code ($exc_name)" >> "$LOGFILE"
    elif [ $func -eq 4 ]; then
        # Read Input Registers - most common for ANDRTF3
        if [ $len -eq 8 ]; then
            # Request: addr(1) + func(1) + reg(2) + count(2) + crc(2)
            local reg=$((16#${hex:4:4}))
            local count=$((16#${hex:8:4}))
            echo -e "${CYAN}[$timestamp] REQUEST: addr=$addr func=$func ($func_name) reg=$reg count=$count ${crc_color}CRC:$crc_ok${NC}"
            echo "[$timestamp] REQUEST: addr=$addr func=$func reg=$reg count=$count crc=$crc_ok" >> "$LOGFILE"
        else
            # Response: addr(1) + func(1) + bytecount(1) + data(n) + crc(2)
            local bytecount=$((16#${hex:4:2}))
            local data_start=6
            local data_bytes="${hex:$data_start:$((bytecount*2))}"

            # Parse register values
            local values=""
            for ((i=0; i<${#data_bytes}; i+=4)); do
                if [ $((i+4)) -le ${#data_bytes} ]; then
                    local val=$((16#${data_bytes:$i:4}))
                    # Handle signed values
                    if [ $val -ge 32768 ]; then
                        val=$((val - 65536))
                    fi
                    values="$values $val"
                fi
            done

            # For temperature, convert to degrees
            local temp_info=""
            if [ -n "$values" ]; then
                local first_val=$(echo $values | awk '{print $1}')
                if [ "$first_val" -eq 0 ]; then
                    temp_info=" ${RED}[0x0000 SENSOR ERROR]${NC}"
                elif [ "$first_val" -eq -1 ]; then
                    temp_info=" ${RED}[0xFFFF MODBUS ERROR]${NC}"
                elif [ "$first_val" -ge -400 ] && [ "$first_val" -le 1250 ]; then
                    local temp_c=$(awk "BEGIN {printf \"%.1f\", $first_val/10}")
                    temp_info=" ${GREEN}[${temp_c}°C]${NC}"
                else
                    temp_info=" ${YELLOW}[OUT OF RANGE]${NC}"
                fi
            fi

            echo -e "${GREEN}[$timestamp] RESPONSE: addr=$addr func=$func bytes=$bytecount values=$values${temp_info} ${crc_color}CRC:$crc_ok${NC}"
            echo "[$timestamp] RESPONSE: addr=$addr func=$func bytes=$bytecount values=$values crc=$crc_ok" >> "$LOGFILE"
        fi
    elif [ $func -eq 3 ]; then
        # Read Holding Registers
        if [ $len -eq 8 ]; then
            local reg=$((16#${hex:4:4}))
            local count=$((16#${hex:8:4}))
            echo -e "${CYAN}[$timestamp] REQUEST: addr=$addr func=$func ($func_name) reg=$reg count=$count ${crc_color}CRC:$crc_ok${NC}"
            echo "[$timestamp] REQUEST: addr=$addr func=$func reg=$reg count=$count crc=$crc_ok" >> "$LOGFILE"
        else
            local bytecount=$((16#${hex:4:2}))
            local data_bytes="${hex:6:$((bytecount*2))}"
            echo -e "${GREEN}[$timestamp] RESPONSE: addr=$addr func=$func bytes=$bytecount data=$data_bytes ${crc_color}CRC:$crc_ok${NC}"
            echo "[$timestamp] RESPONSE: addr=$addr func=$func bytes=$bytecount data=$data_bytes crc=$crc_ok" >> "$LOGFILE"
        fi
    else
        # Generic frame
        echo -e "${BLUE}[$timestamp] FRAME: addr=$addr func=$func ($func_name) len=$len ${crc_color}CRC:$crc_ok${NC}"
        echo "[$timestamp] FRAME: addr=$addr func=$func len=$len hex=$hex crc=$crc_ok" >> "$LOGFILE"
    fi
}

# Frame detection state
frame_buffer=""
last_byte_time=0
FRAME_GAP_MS=10  # 3.5 char times at 9600 = ~4ms, use 10ms for safety

echo -e "${GREEN}Listening...${NC}"
echo ""
echo "# Modbus RTU Capture started $(date)" > "$LOGFILE"
echo "# Device: $DEVICE" >> "$LOGFILE"
echo "" >> "$LOGFILE"

# Read bytes and detect frame boundaries
while true; do
    # Read one byte with timeout
    byte=$(timeout 0.1 dd if="$DEVICE" bs=1 count=1 2>/dev/null | xxd -p)
    current_time=$(date +%s%N)
    current_ms=$((current_time / 1000000))

    if [ -n "$byte" ]; then
        # Check for frame gap (new frame starting)
        if [ -n "$frame_buffer" ] && [ $last_byte_time -gt 0 ]; then
            gap_ms=$((current_ms - last_byte_time))
            if [ $gap_ms -gt $FRAME_GAP_MS ]; then
                # Process previous frame
                timestamp=$(date +%H:%M:%S.%3N)
                decode_frame "$frame_buffer" "$timestamp"
                frame_buffer=""
            fi
        fi

        frame_buffer="${frame_buffer}${byte}"
        last_byte_time=$current_ms
    else
        # Timeout - if we have data, it's a complete frame
        if [ -n "$frame_buffer" ]; then
            timestamp=$(date +%H:%M:%S.%3N)
            decode_frame "$frame_buffer" "$timestamp"
            frame_buffer=""
            last_byte_time=0
        fi
    fi
done
