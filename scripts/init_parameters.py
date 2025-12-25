#!/usr/bin/env python3
"""
ESPlan Boiler Controller - Parameter Initialization Script

This script initializes all configurable parameters with sensible default values.
It can be used for initial setup or to reset parameters to known good values.
"""

import json
import time
import sys
import os
import argparse
from typing import Dict, Any, Optional

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("Error: paho-mqtt is required. Install with: pip install paho-mqtt")
    sys.exit(1)

# Default MQTT settings
DEFAULT_MQTT_HOST = "192.168.20.27"
DEFAULT_MQTT_PORT = 1883
DEFAULT_MQTT_USER = "YOUR_MQTT_USER"
DEFAULT_MQTT_PASS = "YOUR_MQTT_PASSWORD"

# ANSI color codes
class Colors:
    GREEN = '\033[0;32m'
    YELLOW = '\033[1;33m'
    RED = '\033[0;31m'
    BLUE = '\033[0;34m'
    BOLD = '\033[1m'
    NC = '\033[0m'  # No Color

# Parameter definitions with default values
PARAMETERS = {
    "Water Heater Configuration": {
        "wheater/priorityEnabled": {"value": True, "desc": "Water heating priority over space heating"},
        "wheater/tempLimitLow": {"value": 35.0, "desc": "Water heater low temperature limit (°C)"},
        "wheater/tempLimitHigh": {"value": 60.0, "desc": "Water heater high temperature limit (°C)"},
        "wheater/tempChargeDelta": {"value": 5.0, "desc": "Water heater charging temperature delta (°C)"},
        "wheater/tempSafeLimitHigh": {"value": 80.0, "desc": "Water heater safety high limit (°C)"},
        "wheater/tempSafeLimitLow": {"value": 5.0, "desc": "Water heater safety low limit (°C)"},
        "wheater/hysteresis": {"value": 2.0, "desc": "Water heater temperature hysteresis (°C)"},
    },
    "Heating Configuration": {
        "heating/targetTemp": {"value": 21.0, "desc": "Target indoor temperature (°C)"},
        "heating/curveShift": {"value": 20.0, "desc": "Heating curve shift"},
        "heating/curveCoeff": {"value": 1.5, "desc": "Heating curve coefficient"},
        "heating/burnerLowLimit": {"value": 35.0, "desc": "Minimum burner temperature (°C)"},
        "heating/highLimit": {"value": 75.0, "desc": "Maximum heating temperature (°C)"},
        "heating/hysteresis": {"value": 0.5, "desc": "Heating temperature hysteresis (°C)"},
    },
    "PID Parameters - Space Heating": {
        "pid/spaceHeating/kp": {"value": 2.0, "desc": "Space heating proportional gain"},
        "pid/spaceHeating/ki": {"value": 0.5, "desc": "Space heating integral gain"},
        "pid/spaceHeating/kd": {"value": 0.1, "desc": "Space heating derivative gain"},
    },
    "PID Parameters - Water Heater": {
        "pid/waterHeater/kp": {"value": 1.5, "desc": "Water heater proportional gain"},
        "pid/waterHeater/ki": {"value": 0.3, "desc": "Water heater integral gain"},
        "pid/waterHeater/kd": {"value": 0.05, "desc": "Water heater derivative gain"},
    },
    "Sensor Configuration": {
        "sensor/miThInterval": {"value": 5000, "desc": "MiThermometer update interval (ms)"},
    }
}

class ParameterInitializer:
    def __init__(self, host: str, port: int, username: str, password: str):
        self.host = host
        self.port = port
        self.username = username
        self.password = password
        self.client = mqtt.Client()
        self.connected = False
        self.responses = {}
        
    def connect(self) -> bool:
        """Connect to MQTT broker"""
        try:
            self.client.username_pw_set(self.username, self.password)
            self.client.on_connect = self._on_connect
            self.client.on_message = self._on_message
            
            print(f"Connecting to MQTT broker at {self.host}:{self.port}...")
            self.client.connect(self.host, self.port, 60)
            self.client.loop_start()
            
            # Wait for connection
            timeout = 5
            while not self.connected and timeout > 0:
                time.sleep(0.1)
                timeout -= 0.1
                
            if self.connected:
                print(f"{Colors.GREEN}✓ Connected successfully{Colors.NC}\n")
                # Subscribe to responses
                self.client.subscribe("boiler/params/status/#")
                return True
            else:
                print(f"{Colors.RED}✗ Connection failed{Colors.NC}")
                return False
                
        except Exception as e:
            print(f"{Colors.RED}✗ Connection error: {e}{Colors.NC}")
            return False
            
    def _on_connect(self, client, userdata, flags, rc):
        """Callback for MQTT connection"""
        if rc == 0:
            self.connected = True
        else:
            print(f"{Colors.RED}Connection failed with code {rc}{Colors.NC}")
            
    def _on_message(self, client, userdata, msg):
        """Callback for MQTT messages"""
        topic = msg.topic
        try:
            payload = json.loads(msg.payload.decode())
            self.responses[topic] = payload
        except:
            pass
            
    def set_parameter(self, param: str, value: Any, description: str) -> bool:
        """Set a single parameter"""
        topic = f"boiler/params/set/{param}"
        payload = json.dumps({"value": value})
        
        print(f"{Colors.YELLOW}Setting{Colors.NC} {description}: {Colors.GREEN}{value}{Colors.NC}")
        
        try:
            result = self.client.publish(topic, payload)
            if result.rc == 0:
                print(f"  ✓ Sent successfully")
                return True
            else:
                print(f"  {Colors.RED}✗ Failed to send{Colors.NC}")
                return False
        except Exception as e:
            print(f"  {Colors.RED}✗ Error: {e}{Colors.NC}")
            return False
            
    def save_parameters(self) -> bool:
        """Save all parameters to flash"""
        print(f"\n{Colors.GREEN}=== Saving Parameters to Flash ==={Colors.NC}")
        print("Saving all parameters to non-volatile storage...")
        
        try:
            result = self.client.publish("boiler/params/save", "")
            if result.rc == 0:
                print(f"  ✓ Save command sent successfully")
                return True
            else:
                print(f"  {Colors.RED}✗ Failed to send save command{Colors.NC}")
                return False
        except Exception as e:
            print(f"  {Colors.RED}✗ Error: {e}{Colors.NC}")
            return False
            
    def initialize_all(self, delay: float = 0.2):
        """Initialize all parameters"""
        total_params = sum(len(params) for params in PARAMETERS.values())
        current = 0
        
        for category, params in PARAMETERS.items():
            print(f"\n{Colors.GREEN}=== {category} ==={Colors.NC}")
            
            for param, info in params.items():
                current += 1
                print(f"\n[{current}/{total_params}] ", end="")
                self.set_parameter(param, info["value"], info["desc"])
                time.sleep(delay)  # Avoid overwhelming the system
                
    def disconnect(self):
        """Disconnect from MQTT broker"""
        self.client.loop_stop()
        self.client.disconnect()

def main():
    parser = argparse.ArgumentParser(
        description="Initialize ESPlan Boiler Controller parameters",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                        # Use default settings
  %(prog)s --host 192.168.1.100   # Custom MQTT host
  %(prog)s --dry-run              # Show what would be set without sending
  %(prog)s --custom params.json   # Load custom parameter values
        """
    )
    
    parser.add_argument("--host", default=DEFAULT_MQTT_HOST,
                        help=f"MQTT broker host (default: {DEFAULT_MQTT_HOST})")
    parser.add_argument("--port", type=int, default=DEFAULT_MQTT_PORT,
                        help=f"MQTT broker port (default: {DEFAULT_MQTT_PORT})")
    parser.add_argument("--user", default=DEFAULT_MQTT_USER,
                        help=f"MQTT username (default: {DEFAULT_MQTT_USER})")
    parser.add_argument("--password", default=DEFAULT_MQTT_PASS,
                        help=f"MQTT password (default: {DEFAULT_MQTT_PASS})")
    parser.add_argument("--delay", type=float, default=0.2,
                        help="Delay between parameter sets in seconds (default: 0.2)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Show what would be set without actually sending")
    parser.add_argument("--custom", type=str, metavar="FILE",
                        help="Load custom parameter values from JSON file")
    parser.add_argument("--export", type=str, metavar="FILE",
                        help="Export default parameters to JSON file")
    
    args = parser.parse_args()
    
    # Export parameters if requested
    if args.export:
        export_data = {}
        for category, params in PARAMETERS.items():
            export_data[category] = {
                param: {
                    "value": info["value"],
                    "description": info["desc"]
                }
                for param, info in params.items()
            }
        
        with open(args.export, 'w') as f:
            json.dump(export_data, f, indent=2)
        print(f"Parameters exported to {args.export}")
        return
    
    # Load custom parameters if provided
    if args.custom:
        try:
            with open(args.custom, 'r') as f:
                custom_params = json.load(f)
            
            # Update PARAMETERS with custom values
            for category, params in custom_params.items():
                if category in PARAMETERS:
                    for param, info in params.items():
                        if param in PARAMETERS[category]:
                            PARAMETERS[category][param]["value"] = info.get("value", 
                                                                           PARAMETERS[category][param]["value"])
            print(f"Loaded custom parameters from {args.custom}")
        except Exception as e:
            print(f"{Colors.RED}Error loading custom parameters: {e}{Colors.NC}")
            sys.exit(1)
    
    # Header
    print("=" * 50)
    print(f"{Colors.BOLD}ESPlan Boiler Controller Parameter Initialization{Colors.NC}")
    print("=" * 50)
    print(f"MQTT Broker: {args.host}:{args.port}")
    print(f"MQTT User: {args.user}")
    
    if args.dry_run:
        print(f"\n{Colors.YELLOW}DRY RUN MODE - No parameters will be sent{Colors.NC}")
        print("\nParameters that would be set:")
        for category, params in PARAMETERS.items():
            print(f"\n{Colors.GREEN}{category}:{Colors.NC}")
            for param, info in params.items():
                print(f"  {param} = {info['value']} ({info['desc']})")
        return
    
    # Initialize parameters
    initializer = ParameterInitializer(args.host, args.port, args.user, args.password)
    
    if not initializer.connect():
        sys.exit(1)
    
    try:
        initializer.initialize_all(args.delay)
        initializer.save_parameters()
        
        print("\n" + "=" * 50)
        print(f"{Colors.GREEN}Parameter initialization complete!{Colors.NC}")
        print("=" * 50)
        print("\nYou can verify parameters by requesting them:")
        print(f"  mosquitto_pub -h {args.host} -u {args.user} -P {args.password} -t 'boiler/params/list' -m ''")
        print("\nOr monitor all parameter topics:")
        print(f"  mosquitto_sub -v -h {args.host} -u {args.user} -P {args.password} -t 'boiler/params/#'")
        
    except KeyboardInterrupt:
        print(f"\n{Colors.YELLOW}Initialization interrupted by user{Colors.NC}")
    finally:
        initializer.disconnect()

if __name__ == "__main__":
    main()