#!/usr/bin/env python3
"""
Pre-build script to check for credentials.ini and provide helpful guidance.

This script makes credentials.ini optional by:
1. Checking if credentials.ini exists
2. Providing clear guidance if missing
3. Allowing build to continue with defaults from ProjectConfig.h

Usage: Automatically called by PlatformIO via extra_scripts in platformio.ini
"""

Import("env")
import os
import sys

# Get project directory
project_dir = env.get("PROJECT_DIR")
credentials_file = os.path.join(project_dir, "credentials.ini")
example_file = os.path.join(project_dir, "credentials.example.ini")

# Check if credentials.ini exists
if not os.path.exists(credentials_file):
    print("")
    print("=" * 70)
    print("⚠️  WARNING: credentials.ini not found")
    print("=" * 70)
    print("")
    print("The build will continue using default values from ProjectConfig.h:")
    print("  - MQTT_USERNAME: (empty)")
    print("  - MQTT_PASSWORD: (empty)")
    print("  - OTA_PASSWORD: 'update-password'")
    print("")
    print("For production deployment:")
    print(f"  1. Copy:  cp credentials.example.ini credentials.ini")
    print(f"  2. Edit:  nano credentials.ini")
    print("  3. Never commit credentials.ini to git (already in .gitignore)")
    print("")
    print("=" * 70)
    print("")

    # Check if example file exists to guide user
    if not os.path.exists(example_file):
        print("⚠️  WARNING: credentials.example.ini also missing!")
        print("   This file should be in version control as a template.")
        print("")

    # Create a minimal credentials.ini that won't override ProjectConfig.h defaults
    # This prevents PlatformIO build errors from missing extra_configs file
    print("Creating minimal credentials.ini (allows build to proceed)...")
    with open(credentials_file, 'w') as f:
        f.write("; Auto-generated minimal credentials.ini\n")
        f.write("; Copy credentials.example.ini and edit with real values for production\n")
        f.write(";\n")
        f.write("; These are intentionally NOT defined so ProjectConfig.h defaults are used:\n")
        f.write("; [credentials]\n")
        f.write("; MQTT_USERNAME = your-mqtt-username\n")
        f.write("; MQTT_PASSWORD = your-mqtt-password\n")
        f.write("; OTA_PASSWORD = your-ota-password\n")
    print("Created credentials.ini (using ProjectConfig.h defaults)")
    print("")
else:
    # Credentials file exists - verify it's not the example
    with open(credentials_file, 'r') as f:
        content = f.read()
        if 'your-mqtt-username' in content or 'your-mqtt-password' in content:
            print("")
            print("=" * 70)
            print("⚠️  WARNING: credentials.ini contains example values")
            print("=" * 70)
            print("")
            print("Please edit credentials.ini with real credentials:")
            print("  - MQTT_USERNAME")
            print("  - MQTT_PASSWORD")
            print("  - OTA_PASSWORD")
            print("")
            print("=" * 70)
            print("")
