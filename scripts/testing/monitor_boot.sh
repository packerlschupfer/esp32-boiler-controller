#!/bin/bash

echo "=== Monitoring Boot Process ==="
echo "Press Ctrl+C to stop"
echo

# Monitor serial output
pio device monitor -b 115200 --filter time | tee boot_log_$(date +%Y%m%d_%H%M%S).txt