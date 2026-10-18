# Boiler Controller Config Add-on

A Home Assistant add-on for configuring the ESPlan Boiler Controller via MQTT.

## Features

- Live sensor display (temperatures)
- PID tuning for space heating and water heater
- Temperature setpoints configuration
- System control (on/off, restart)
- Save parameters to device NVS

## Installation

### Local Development

1. Copy the `addon` folder to your Home Assistant `/addons/` directory:
   ```bash
   cp -r addon /usr/share/hassio/addons/local/boiler_config
   ```

2. In Home Assistant, go to **Settings → Add-ons → Add-on Store**

3. Click the three dots menu → **Check for updates**

4. Find "Boiler Controller Config" in the **Local add-ons** section

5. Click **Install**

### Repository Installation

Add this repository to Home Assistant:
1. Go to **Settings → Add-ons → Add-on Store**
2. Click three dots → **Repositories**
3. Add the repository URL

## Configuration

```yaml
mqtt_host: "192.168.20.27"    # MQTT broker IP
mqtt_port: 1883               # MQTT port
mqtt_user: "YOUR_MQTT_USER"        # MQTT username
mqtt_password: "your_password" # MQTT password
mqtt_topic_prefix: "boiler"   # Topic prefix
```

## Usage

1. Open the add-on from the sidebar (Boiler Config)
2. The UI shows live sensor data and current parameter values
3. Enter new values in the input fields and press Enter to send
4. Click **Save to NVS** to persist changes on the device
5. Click **Refresh** to request current values from device

## MQTT Topics

The add-on uses these MQTT topics:

### Parameters (read/write)
- `boiler/params/pid/spaceHeating/{kp,ki,kd}`
- `boiler/params/pid/waterHeater/{kp,ki,kd}`
- `boiler/params/heating/{comfort,eco,frost,minBoiler,maxBoiler}`
- `boiler/params/wheater/{target,hysteresis,boilerTarget}`

### Commands
- `boiler/params/save` - Save to NVS
- `boiler/params/get/all` - Request all parameters
- `boiler/cmd/system` - on/off/restart
- `boiler/cmd/heating` - on/off
- `boiler/cmd/water` - on/off

### Status (read-only)
- `boiler/status/sensors` - Live sensor data

## Development

To run locally without Home Assistant:

```bash
cd rootfs/app
pip install -r requirements.txt
export MQTT_HOST=192.168.20.27
export MQTT_USER=YOUR_MQTT_USER
export MQTT_PASSWORD=your_password
python server.py
```

Then open http://localhost:8099
