#!/usr/bin/env python3
import paho.mqtt.client as mqtt
import json
import time
from datetime import datetime, timedelta

# MQTT Configuration
BROKER = "192.168.20.27"
USERNAME = "YOUR_MQTT_USER"
PASSWORD = "YOUR_MQTT_PASSWORD"

# Test scenarios
scenarios = []

def on_connect(client, userdata, flags, rc):
    print(f"Connected with result code {rc}")
    client.subscribe("hotwater/status")
    client.subscribe("sensors/status")
    client.subscribe("boiler/params/+/+")

def on_message(client, userdata, msg):
    topic = msg.topic
    payload = msg.payload.decode()
    
    if topic == "hotwater/status":
        data = json.loads(payload)
        print(f"\n[{datetime.now().strftime('%H:%M:%S')}] Hot Water Status:")
        print(f"  Time: {data['time']}, Water: {data['waterTemp']}°C")
        print(f"  Pre-heating: {data['preheating']}, Schedule: {data['scheduleName']}")
        print(f"  Ready at: {data['estimatedReady']}, Rate: {data['heatingRate']}°C/min")
    
    elif topic == "sensors/status":
        data = json.loads(payload)
        water_temp = data['t']['wt'] / 10.0
        print(f"  [Sensor] Water tank: {water_temp}°C")

def run_scenario(client, name, action):
    print(f"\n{'='*60}")
    print(f"SCENARIO: {name}")
    print(f"{'='*60}")
    action(client)
    time.sleep(5)

def test_heating_rate_change(client):
    print("Testing heating rate parameter change...")
    
    # Change to 2.0°C/min
    client.publish("boiler/params/set/wheater/heatingRate", "2.0")
    print("Set heating rate to 2.0°C/min")
    time.sleep(3)
    
    # Request status
    client.publish("hotwater/control", "status")
    time.sleep(2)
    
    # Change back to 1.0°C/min
    client.publish("boiler/params/set/wheater/heatingRate", "1.0")
    print("Set heating rate back to 1.0°C/min")

def test_manual_override(client):
    print("Testing manual override...")
    
    # Force off
    client.publish("hotwater/control", "off")
    print("Sent OFF command")
    time.sleep(5)
    
    # Force on
    client.publish("hotwater/control", "on")
    print("Sent ON command")
    time.sleep(5)
    
    # Back to auto
    client.publish("hotwater/control", "auto")
    print("Sent AUTO command")

def test_schedule_addition(client):
    print("Testing schedule addition...")
    
    # Add morning schedule
    morning = datetime.now() + timedelta(hours=12)
    schedule = {
        "name": "Morning Test",
        "enabled": True,
        "days": [1,2,3,4,5],
        "startTime": morning.strftime("%H:%M"),
        "duration": 45
    }
    client.publish("hotwater/schedule/add", json.dumps(schedule))
    print(f"Added morning schedule for {morning.strftime('%H:%M')}")
    
    time.sleep(3)
    
    # Add evening schedule (soon)
    evening = datetime.now() + timedelta(minutes=45)
    schedule = {
        "name": "Evening Test",
        "enabled": True,
        "days": [1,2,3,4,5,6,7],
        "startTime": evening.strftime("%H:%M"),
        "duration": 30
    }
    client.publish("hotwater/schedule/add", json.dumps(schedule))
    print(f"Added evening schedule for {evening.strftime('%H:%M')}")

def test_vacation_mode(client):
    print("Testing vacation mode...")
    
    # Enable vacation
    client.publish("hotwater/vacation", "on")
    print("Vacation mode ON")
    time.sleep(5)
    
    # Disable vacation
    client.publish("hotwater/vacation", "off")
    print("Vacation mode OFF")

def monitor_temperature_rise(client):
    print("Monitoring temperature rise for 5 minutes...")
    start_time = time.time()
    
    while time.time() - start_time < 300:  # 5 minutes
        client.publish("hotwater/control", "status")
        time.sleep(30)
        print(f"  [{int(time.time() - start_time)}s] Checking...")

# Main execution
def main():
    client = mqtt.Client()
    client.username_pw_set(USERNAME, PASSWORD)
    client.on_connect = on_connect
    client.on_message = on_message
    
    print("Connecting to MQTT broker...")
    client.connect(BROKER, 1883, 60)
    
    # Start background thread for MQTT
    client.loop_start()
    time.sleep(2)
    
    # Run test scenarios
    scenarios = [
        ("Heating Rate Adjustment", test_heating_rate_change),
        ("Manual Override", test_manual_override),
        ("Schedule Addition", test_schedule_addition),
        ("Vacation Mode", test_vacation_mode),
        ("Temperature Rise Monitoring", monitor_temperature_rise)
    ]
    
    for name, func in scenarios:
        run_scenario(client, name, func)
        time.sleep(10)
    
    print("\n\nAll scenarios completed!")
    client.loop_stop()
    client.disconnect()

if __name__ == "__main__":
    main()