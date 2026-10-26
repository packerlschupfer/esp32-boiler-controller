# Parameter Initialization Scripts

This directory contains scripts to initialize and manage parameters for the ESPlan Boiler Controller.

## Scripts

### `init_parameters.sh` - Bash Script
Simple bash script for quick parameter initialization.

**Usage:**
```bash
chmod +x init_parameters.sh
./init_parameters.sh
```

**Environment variables:**
```bash
export MQTT_HOST="192.168.20.27"
export MQTT_USER="YOUR_MQTT_USER"
export MQTT_PASS="YOUR_MQTT_PASSWORD"
./init_parameters.sh
```

### `init_parameters.py` - Python Script
Full-featured Python script with more options and better error handling.

**Installation:**
```bash
pip install paho-mqtt
```

**Basic usage:**
```bash
python3 init_parameters.py
```

**Advanced options:**
```bash
# Use custom MQTT host
python3 init_parameters.py --host 192.168.1.100

# Dry run - show what would be set without sending
python3 init_parameters.py --dry-run

# Use custom parameter values
python3 init_parameters.py --custom my_params.json

# Export default parameters to file
python3 init_parameters.py --export default_params.json

# Slower initialization (if system is overwhelmed)
python3 init_parameters.py --delay 0.5
```

### `custom_parameters.json` - Configuration File
Example JSON file with all parameters and their default values. You can copy and modify this file to create your own parameter sets.

**Usage:**
```bash
# Copy and edit
cp custom_parameters.json my_home_params.json
# Edit my_home_params.json with your values
python3 init_parameters.py --custom my_home_params.json
```

## Parameter Categories

1. **Water Heater Configuration**
   - Temperature limits and hysteresis
   - Priority settings
   - Safety limits

2. **Heating Configuration**
   - Target indoor temperature
   - Heating curve parameters
   - Temperature limits

3. **PID Parameters**
   - Space heating PID gains (Kp, Ki, Kd)
   - Water heater PID gains

4. **Sensor Configuration**
   - Update intervals
   - Sensor-specific settings

## Examples

### Initial Setup
```bash
# First time setup with default values
python3 init_parameters.py

# Monitor the results
mosquitto_sub -v -h 192.168.20.27 -u YOUR_MQTT_USER -P YOUR_MQTT_PASSWORD -t "boiler/params/#"
```

### Winter/Summer Presets
Create different parameter files for different seasons:

```bash
# Export defaults
python3 init_parameters.py --export winter_params.json
python3 init_parameters.py --export summer_params.json

# Edit the files with appropriate values
# Winter: Higher target temps, different curve
# Summer: Lower targets, water heating only

# Apply winter settings
python3 init_parameters.py --custom winter_params.json

# Apply summer settings
python3 init_parameters.py --custom summer_params.json
```

### Testing New PID Values
```bash
# Test without applying
python3 init_parameters.py --dry-run --custom test_pid.json

# If looks good, apply
python3 init_parameters.py --custom test_pid.json
```

## Troubleshooting

1. **Connection refused**: Check MQTT broker is running and accessible
2. **Authentication failed**: Verify username and password
3. **No response**: Ensure ESP32 is connected to network and MQTT
4. **Parameters not saving**: Check that PersistentStorageTask is running

## Safety Notes

- Always test new parameters carefully
- Keep backups of working parameter sets
- Monitor system behavior after changes
- Some parameters have safety limits that cannot be exceeded