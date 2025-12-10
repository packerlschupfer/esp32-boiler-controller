#!/usr/bin/env python3
"""Boiler Controller Configuration Server - Enhanced."""

import os
import json
import threading
import time
from datetime import datetime
from flask import Flask, render_template, jsonify, request
import paho.mqtt.client as mqtt

app = Flask(__name__)

# Configuration from environment
MQTT_HOST = os.environ.get('MQTT_HOST', '192.168.20.27')
MQTT_PORT = int(os.environ.get('MQTT_PORT', 1883))
MQTT_USER = os.environ.get('MQTT_USER', 'YOUR_MQTT_USER')
MQTT_PASSWORD = os.environ.get('MQTT_PASSWORD', '')
MQTT_PREFIX = os.environ.get('MQTT_TOPIC_PREFIX', 'boiler')

# Current state (updated via MQTT)
state = {
    'connected': False,
    'last_update': None,
    'sensors': {},
    'params': {
        'heating': {},
        'wheater': {},
        'pid': {
            'spaceHeating': {},
            'waterHeater': {},
            'autotune': {}
        },
        'system': {}
    },
    'status': {
        'system': {},
        'health': {},
        'scheduler': {}
    },
    'safety_config': {
        'pump_prot': 15000,
        'sensor_stale': 60000,
        'post_purge': 90000
    },
    'autotune': {
        'status': 'idle',
        'progress': 0,
        'cycles': 0,
        'minCycles': 4,
        'elapsedSec': 0,
        'maxSec': 1800
    },
    'runtime': {},
    'schedules': [],
    'relays': {},
    'system_state': 0,
    'burner_state': 'unknown'
}
state_lock = threading.Lock()

# MQTT Client
mqtt_client = None


def on_connect(client, userdata, flags, reason_code, properties):
    """Handle MQTT connection."""
    print(f"Connected to MQTT: {reason_code}")
    with state_lock:
        state['connected'] = True

    # Subscribe to all boiler topics
    client.subscribe(f"{MQTT_PREFIX}/#")

    # Request current values
    time.sleep(0.5)
    client.publish(f"{MQTT_PREFIX}/params/get/all", "")
    client.publish(f"{MQTT_PREFIX}/cmd/scheduler/list", "")
    client.publish(f"{MQTT_PREFIX}/cmd/fram", "runtime")
    client.publish(f"{MQTT_PREFIX}/cmd/fram", "counters")
    client.publish(f"{MQTT_PREFIX}/cmd/config/get", "safety")  # Request safety config


def on_disconnect(client, userdata, flags, reason_code, properties):
    """Handle MQTT disconnection."""
    print(f"Disconnected from MQTT: {reason_code}")
    with state_lock:
        state['connected'] = False


def on_message(client, userdata, msg):
    """Handle incoming MQTT messages."""
    topic = msg.topic
    try:
        payload = msg.payload.decode('utf-8')
        # Try to parse as JSON
        try:
            data = json.loads(payload)
        except json.JSONDecodeError:
            data = payload

        with state_lock:
            state['last_update'] = datetime.now().isoformat()

            # Parameter updates - MUST be checked BEFORE generic /status/ handler!
            # Grouped responses (boiler/params/status/{category})
            if topic.endswith('/params/status/heating'):
                if isinstance(data, dict):
                    state['params']['heating'].update(data)
            elif topic.endswith('/params/status/wheater'):
                if isinstance(data, dict):
                    state['params']['wheater'].update(data)
            elif topic.endswith('/params/status/pid'):
                if isinstance(data, dict):
                    if 'spaceHeating' in data:
                        state['params']['pid']['spaceHeating'].update(data['spaceHeating'])
                    if 'waterHeater' in data:
                        state['params']['pid']['waterHeater'].update(data['waterHeater'])
            # Individual parameter updates (boiler/params/status/{category}/{param})
            elif '/params/status/heating/' in topic:
                key = topic.split('/')[-1]
                if isinstance(data, dict) and 'value' in data:
                    state['params']['heating'][key] = data['value']
                else:
                    state['params']['heating'][key] = data
            elif '/params/status/wheater/' in topic:
                key = topic.split('/')[-1]
                if isinstance(data, dict) and 'value' in data:
                    state['params']['wheater'][key] = data['value']
                else:
                    state['params']['wheater'][key] = data
            elif '/params/status/pid/spaceHeating/' in topic:
                key = topic.split('/')[-1]
                if isinstance(data, dict) and 'value' in data:
                    state['params']['pid']['spaceHeating'][key] = data['value']
                else:
                    state['params']['pid']['spaceHeating'][key] = data
            elif '/params/status/pid/waterHeater/' in topic:
                key = topic.split('/')[-1]
                if isinstance(data, dict) and 'value' in data:
                    state['params']['pid']['waterHeater'][key] = data['value']
                else:
                    state['params']['pid']['waterHeater'][key] = data
            elif '/params/status/pid/autotune/' in topic:
                key = topic.split('/')[-1]
                if isinstance(data, dict) and 'value' in data:
                    state['params']['pid']['autotune'][key] = data['value']
                else:
                    state['params']['pid']['autotune'][key] = data
            # System parameters (grouped)
            elif topic.endswith('/params/status/system'):
                if isinstance(data, dict):
                    state['params']['system'].update(data)
            # Individual system parameter updates
            elif '/params/status/system/' in topic:
                key = topic.split('/')[-1]
                if isinstance(data, dict) and 'value' in data:
                    state['params']['system'][key] = data['value']
                else:
                    state['params']['system'][key] = data

            # PID auto-tuning status and progress
            elif '/status/pid/autotune' in topic:
                if isinstance(data, str):
                    state['autotune']['status'] = data
                elif isinstance(data, dict):
                    state['autotune'].update(data)
            elif '/pid_autotune/progress' in topic:
                if isinstance(data, dict):
                    state['autotune'].update(data)
                    if 'state' in data:
                        state['autotune']['status'] = data['state']
            elif '/status/pid/params' in topic:
                # Full PID params response
                if isinstance(data, dict):
                    if 'spaceHeating' in data:
                        state['params']['pid']['spaceHeating'].update(data['spaceHeating'])
                    if 'waterHeater' in data:
                        state['params']['pid']['waterHeater'].update(data['waterHeater'])
                    if 'autotune' in data:
                        state['params']['pid']['autotune'].update(data['autotune'])

            # Sensor data
            elif '/status/sensors' in topic:
                state['sensors'] = data
                # Extract system state bits
                if isinstance(data, dict):
                    if 's' in data:
                        state['system_state'] = data['s']
                    if 'r' in data:
                        state['relays'] = parse_relay_bits(data['r'])

            # Status topics
            elif '/status/health' in topic:
                state['status']['health'] = data
            elif '/status/system' in topic:
                state['status']['system'] = data
            elif '/status/scheduler' in topic:
                state['status']['scheduler'] = data
            elif '/status/burner' in topic:
                state['burner_state'] = data
            elif '/status/fram/runtime' in topic:
                state['runtime'] = data
            elif '/status/fram/counters' in topic:
                state['counters'] = data
            elif '/status/safety_config' in topic:
                if isinstance(data, dict):
                    state['safety_config'].update(data)
            elif '/status/' in topic:
                # Generic status handler - MUST be last for /status/ topics
                key = topic.split('/')[-1]
                state['status'][key] = data

            # Scheduler responses
            elif '/scheduler/response' in topic:
                if isinstance(data, dict) and 'schedules' in data:
                    state['schedules'] = data['schedules']
                elif isinstance(data, list):
                    state['schedules'] = data

    except Exception as e:
        print(f"Error processing message {topic}: {e}")


def parse_relay_bits(bits):
    """Parse relay bitmap into named states."""
    return {
        'burner': bool(bits & 0x01),
        'heating_pump': bool(bits & 0x02),
        'water_pump': bool(bits & 0x04),
        'half_power': bool(bits & 0x08),
        'water_mode': bool(bits & 0x10)
    }


def parse_system_bits(bits):
    """Parse system state bitmap."""
    return {
        'system_enabled': bool(bits & 0x01),
        'heating_enabled': bool(bits & 0x02),
        'heating_on': bool(bits & 0x04),
        'water_enabled': bool(bits & 0x08),
        'water_on': bool(bits & 0x10),
        'water_priority': bool(bits & 0x20)
    }


def mqtt_thread():
    """MQTT client thread."""
    global mqtt_client

    mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    mqtt_client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
    mqtt_client.on_connect = on_connect
    mqtt_client.on_disconnect = on_disconnect
    mqtt_client.on_message = on_message

    while True:
        try:
            mqtt_client.connect(MQTT_HOST, MQTT_PORT, 60)
            mqtt_client.loop_forever()
        except Exception as e:
            print(f"MQTT connection error: {e}")
            time.sleep(5)


# Flask Routes

@app.route('/')
def index():
    """Main configuration page."""
    return render_template('index.html')


@app.route('/api/state')
def get_state():
    """Get current state with parsed system bits."""
    with state_lock:
        result = dict(state)
        result['system_bits'] = parse_system_bits(state['system_state'])
        return jsonify(result)


@app.route('/api/param', methods=['POST'])
def set_param():
    """Set a parameter via MQTT.

    IMPORTANT: Parameter SET topics require 'set/' prefix!
    e.g., params/heating/targetTemp -> params/set/heating/targetTemp
    """
    data = request.json
    topic = data.get('topic')
    value = data.get('value')

    if not topic or value is None:
        return jsonify({'error': 'Missing topic or value'}), 400

    # Add 'set/' prefix for parameter writes (firmware requires this)
    # params/heating/foo -> params/set/heating/foo
    if topic.startswith('params/') and '/set/' not in topic and not topic.startswith('params/get/') and not topic.startswith('params/save'):
        parts = topic.split('/', 1)  # ['params', 'heating/targetTemp']
        topic = f"{parts[0]}/set/{parts[1]}"

    full_topic = f"{MQTT_PREFIX}/{topic}"
    mqtt_client.publish(full_topic, str(value))

    return jsonify({'success': True, 'topic': full_topic, 'value': value})


@app.route('/api/command', methods=['POST'])
def send_command():
    """Send a command via MQTT."""
    data = request.json
    topic = data.get('topic')
    payload = data.get('payload', '')

    if not topic:
        return jsonify({'error': 'Missing topic'}), 400

    full_topic = f"{MQTT_PREFIX}/{topic}"

    # If payload is dict/list, send as JSON
    if isinstance(payload, (dict, list)):
        mqtt_client.publish(full_topic, json.dumps(payload))
    else:
        mqtt_client.publish(full_topic, str(payload))

    return jsonify({'success': True, 'topic': full_topic})


@app.route('/api/save', methods=['POST'])
def save_params():
    """Save parameters to NVS."""
    mqtt_client.publish(f"{MQTT_PREFIX}/params/save", "")
    return jsonify({'success': True, 'message': 'Save command sent'})


@app.route('/api/refresh', methods=['POST'])
def refresh_params():
    """Request all parameters from device."""
    mqtt_client.publish(f"{MQTT_PREFIX}/params/get/all", "")
    mqtt_client.publish(f"{MQTT_PREFIX}/cmd/scheduler/list", "")
    mqtt_client.publish(f"{MQTT_PREFIX}/cmd/fram", "runtime")
    mqtt_client.publish(f"{MQTT_PREFIX}/cmd/fram", "counters")
    return jsonify({'success': True, 'message': 'Refresh requested'})


@app.route('/api/schedule/add', methods=['POST'])
def add_schedule():
    """Add a new schedule."""
    data = request.json

    schedule = {
        'type': data.get('type', 'water'),  # 'water' or 'heating'
        'days': data.get('days', 127),  # Bitmask: Sun=1, Mon=2, ... Sat=64
        'startHour': data.get('startHour', 6),
        'startMinute': data.get('startMinute', 0),
        'endHour': data.get('endHour', 7),
        'endMinute': data.get('endMinute', 0),
        'enabled': data.get('enabled', True)
    }

    mqtt_client.publish(f"{MQTT_PREFIX}/cmd/scheduler/add", json.dumps(schedule))

    # Request updated list after a short delay
    time.sleep(0.5)
    mqtt_client.publish(f"{MQTT_PREFIX}/cmd/scheduler/list", "")

    return jsonify({'success': True, 'schedule': schedule})


@app.route('/api/schedule/remove', methods=['POST'])
def remove_schedule():
    """Remove a schedule by index."""
    data = request.json
    index = data.get('index')

    if index is None:
        return jsonify({'error': 'Missing index'}), 400

    mqtt_client.publish(f"{MQTT_PREFIX}/cmd/scheduler/remove", str(index))

    # Request updated list after a short delay
    time.sleep(0.5)
    mqtt_client.publish(f"{MQTT_PREFIX}/cmd/scheduler/list", "")

    return jsonify({'success': True, 'removed': index})


@app.route('/api/schedule/list', methods=['GET'])
def list_schedules():
    """Get current schedules."""
    mqtt_client.publish(f"{MQTT_PREFIX}/cmd/scheduler/list", "")
    time.sleep(0.3)
    with state_lock:
        return jsonify({'schedules': state['schedules']})


@app.route('/api/safety_config', methods=['POST'])
def set_safety_config():
    """Set safety configuration parameters."""
    data = request.json
    param = data.get('param')
    value = data.get('value')

    if not param or value is None:
        return jsonify({'error': 'Missing param or value'}), 400

    # Validate parameter name
    valid_params = ['pump_protection_ms', 'sensor_stale_ms', 'post_purge_ms']
    if param not in valid_params:
        return jsonify({'error': f'Invalid parameter: {param}'}), 400

    # Send via MQTT
    topic = f"{MQTT_PREFIX}/cmd/config/{param}"
    mqtt_client.publish(topic, str(value))

    # Request updated config after short delay
    time.sleep(0.3)
    mqtt_client.publish(f"{MQTT_PREFIX}/cmd/config/get", "safety")

    return jsonify({'success': True, 'param': param, 'value': value})


@app.route('/api/safety_config', methods=['GET'])
def get_safety_config():
    """Get current safety configuration."""
    mqtt_client.publish(f"{MQTT_PREFIX}/cmd/config/get", "safety")
    time.sleep(0.3)
    with state_lock:
        return jsonify(state['safety_config'])


if __name__ == '__main__':
    # Start MQTT thread
    mqtt_t = threading.Thread(target=mqtt_thread, daemon=True)
    mqtt_t.start()

    # Give MQTT time to connect
    time.sleep(1)

    # Start Flask
    app.run(host='0.0.0.0', port=8099, debug=False, threaded=True)
