#!/bin/bash
# Clear all retained MQTT messages under boiler/#
# Usage: ./clear_retained_mqtt.sh [host] [user] [password]

HOST="${1:-192.168.16.27}"
USER="${2:-gasburner}"
PASS="${3:-2qBWx35yfqgJrDYF}"
TOPIC_BASE="boiler/#"

echo "Clearing retained MQTT messages from $HOST"
echo "Topic pattern: $TOPIC_BASE"
echo ""

# Get all retained messages and extract topics
TOPICS=$(mosquitto_sub -h "$HOST" -u "$USER" -P "$PASS" -t "$TOPIC_BASE" -v --retained-only -W 2 2>/dev/null | awk '{print $1}')

if [ -z "$TOPICS" ]; then
    echo "No retained messages found."
    exit 0
fi

echo "Found retained messages on topics:"
echo "$TOPICS"
echo ""

read -p "Delete all retained messages? [y/N] " -n 1 -r
echo ""

if [[ $REPLY =~ ^[Yy]$ ]]; then
    for topic in $TOPICS; do
        echo "Clearing: $topic"
        mosquitto_pub -h "$HOST" -u "$USER" -P "$PASS" -t "$topic" -r -n
    done
    echo ""
    echo "Done. All retained messages cleared."
else
    echo "Aborted."
fi
