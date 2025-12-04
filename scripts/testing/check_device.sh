#!/bin/bash

echo "Checking device connectivity..."
echo

# Try ping first
echo "Ping test to 192.168.20.27:"
ping -c 3 -W 2 192.168.20.27

echo
echo "MQTT test - requesting status:"
mosquitto_pub -h 192.168.20.27 -u YOUR_MQTT_USER -P YOUR_MQTT_PASSWORD -t "hotwater/control" -m "status"
sleep 2

echo "Waiting for response (5 seconds)..."
mosquitto_sub -h 192.168.20.27 -u YOUR_MQTT_USER -P YOUR_MQTT_PASSWORD -t "hotwater/status" -C 1 -W 5 || echo "No response received"

echo
echo "Done"