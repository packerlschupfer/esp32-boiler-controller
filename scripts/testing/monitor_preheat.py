#!/usr/bin/env python3
import paho.mqtt.client as mqtt
import json
import time
from datetime import datetime

# MQTT Configuration
BROKER = "192.168.20.27"
USERNAME = "YOUR_MQTT_USER"
PASSWORD = "YOUR_MQTT_PASSWORD"

# State tracking
last_status = {}
preheat_start = None

def on_connect(client, userdata, flags, rc):
    print(f"Connected to MQTT broker (result: {rc})")
    client.subscribe("hotwater/status")
    client.subscribe("sensors/status")
    client.subscribe("burner/status")
    client.subscribe("burner/request/+")
    print("Subscribed to topics")

def on_message(client, userdata, msg):
    global last_status, preheat_start
    
    timestamp = datetime.now().strftime('%H:%M:%S')
    
    if msg.topic == "hotwater/status":
        try:
            data = json.loads(msg.payload)
            
            # Track pre-heating start
            if data.get('preheating') and not last_status.get('preheating'):
                preheat_start = time.time()
                print(f"\n[{timestamp}] PRE-HEATING STARTED for '{data.get('scheduleName')}'")
            
            # Check for state changes
            if data != last_status:
                print(f"\n[{timestamp}] Hot Water Status Update:")
                print(f"  Time: {data['time']}, Water Temp: {data['waterTemp']}°C")
                print(f"  Pre-heating: {data['preheating']}, Active: {data['active']}")
                print(f"  Schedule: {data['scheduleName']}, Ready at: {data['estimatedReady']}")
                
                if preheat_start and data.get('preheating'):
                    duration_min = (time.time() - preheat_start) / 60
                    print(f"  Pre-heating duration: {duration_min:.1f} minutes")
                
                last_status = data
        except:
            pass
    
    elif msg.topic == "sensors/status":
        try:
            data = json.loads(msg.payload)
            temps = data.get('t', {})
            water_temp = temps.get('wt', 0) / 10.0
            boiler_temp = temps.get('bt', 0) / 10.0
            return_temp = temps.get('rt', 0) / 10.0
            
            print(f"[{timestamp}] Sensor Temps: Water={water_temp:.1f}°C, Boiler={boiler_temp:.1f}°C, Return={return_temp:.1f}°C")
        except:
            pass
    
    elif msg.topic.startswith("burner/request/"):
        source = msg.topic.split('/')[-1]
        try:
            data = json.loads(msg.payload)
            if data.get('enabled'):
                print(f"[{timestamp}] Burner Request from {source}: Target={data.get('targetTemp')}°C, Priority={data.get('priority')}")
        except:
            pass
    
    elif msg.topic == "burner/status":
        try:
            data = json.loads(msg.payload)
            if data.get('state') != 'IDLE':
                print(f"[{timestamp}] Burner State: {data.get('state')}")
        except:
            pass

def main():
    print("Hot Water Pre-heating Monitor")
    print("="*60)
    
    client = mqtt.Client()
    client.username_pw_set(USERNAME, PASSWORD)
    client.on_connect = on_connect
    client.on_message = on_message
    
    print("Connecting to MQTT broker...")
    client.connect(BROKER, 1883, 60)
    
    print("Monitoring pre-heating behavior...")
    print("Press Ctrl+C to stop\n")
    
    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print("\nMonitoring stopped")
        client.disconnect()

if __name__ == "__main__":
    main()