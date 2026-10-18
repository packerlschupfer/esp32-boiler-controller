#!/usr/bin/env python3
"""
Pre-build script to check for credentials.ini and provide helpful guidance.

This script makes credentials.ini optional by:
1. Checking if credentials.ini exists
2. Providing clear guidance if missing
3. Allowing build to continue with defaults from ProjectConfig.h
4. Checking the keys platformio.ini expands as ${credentials.<key>}

A credentials.ini without ota_password broke the espota upload envs (--auth=${credentials.
ota_password}) with an error that does not say which key is missing (io-5, 2026-09-14).
Only key names are printed, never their values.

Usage: Automatically called by PlatformIO via extra_scripts in platformio.ini
       Standalone check: python3 scripts/check_credentials.py
"""

import configparser
import os

# Keys platformio.ini expands as ${credentials.<key>} - all of them must exist
REQUIRED_KEYS = {
    "build_flags": "MQTT_USERNAME / MQTT_PASSWORD / OTA_PASSWORD build flags (all envs)",
    "ota_password": "--auth for the OTA upload envs (esp32dev_ota, *_prod_ota, *_dev_ota)",
}

# Placeholder values from credentials.example.ini (underscores) and older templates (hyphens)
EXAMPLE_VALUES = (
    "your_mqtt_username", "your_mqtt_password", "your_ota_password",
    "your-mqtt-username", "your-mqtt-password", "your-ota-password",
)

try:
    Import("env")  # noqa: F821 - injected by PlatformIO (SCons)
    project_dir = env.get("PROJECT_DIR")  # noqa: F821
except NameError:
    # Standalone run: use the repository this script lives in
    project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

credentials_file = os.path.join(project_dir, "credentials.ini")
example_file = os.path.join(project_dir, "credentials.example.ini")

# Check if credentials.ini exists
if not os.path.exists(credentials_file):
    print("")
    print("=" * 70)
    print("WARNING: credentials.ini not found")
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
        print("WARNING: credentials.example.ini also missing!")
        print("   This file should be in version control as a template.")
        print("")

    # Create a minimal credentials.ini that won't override ProjectConfig.h defaults.
    # Every ${credentials.<key>} key has to be present: PlatformIO aborts the build on an
    # undefined one, so an empty build_flags keeps the ProjectConfig.h defaults instead.
    print("Creating minimal credentials.ini (allows build to proceed)...")
    with open(credentials_file, 'w') as f:
        f.write("; Auto-generated minimal credentials.ini\n")
        f.write("; Copy credentials.example.ini and edit with real values for production\n")
        f.write(";\n")
        f.write("; build_flags is empty on purpose: MQTT_USERNAME, MQTT_PASSWORD and\n")
        f.write("; OTA_PASSWORD then keep their ProjectConfig.h defaults.\n")
        f.write("[credentials]\n")
        f.write("build_flags =\n")
        f.write("ota_password = update-password\n")
    print("Created credentials.ini (using ProjectConfig.h defaults)")
    print("")
else:
    # Credentials file exists - verify it defines every key platformio.ini expands.
    # RawConfigParser: a '%' in a password must not be read as an interpolation.
    parser = configparser.RawConfigParser()
    try:
        parser.read(credentials_file)
    except configparser.Error as e:
        # Only the error type: the message can quote a line of the file
        print("")
        print("=" * 70)
        print("WARNING: credentials.ini could not be parsed")
        print("=" * 70)
        print("")
        print(f"  {type(e).__name__} - check the INI syntax against credentials.example.ini")
        print("")
        print("=" * 70)
        print("")
        parser = None

    if parser is not None:
        missing = [key for key in REQUIRED_KEYS if not parser.has_option("credentials", key)]
        if missing:
            print("")
            print("=" * 70)
            print("WARNING: credentials.ini is missing required keys")
            print("=" * 70)
            print("")
            print("platformio.ini expands these; a missing key stops the build or the OTA")
            print("upload with an 'Invalid ${credentials....}' error:")
            for key in missing:
                print(f"  - {key}: {REQUIRED_KEYS[key]}")
            print("")
            print("Add them under [credentials] (see credentials.example.ini).")
            print("")
            print("=" * 70)
            print("")

    # Verify it's not the example
    with open(credentials_file, 'r') as f:
        content = f.read()
        if any(value in content for value in EXAMPLE_VALUES):
            print("")
            print("=" * 70)
            print("WARNING: credentials.ini contains example values")
            print("=" * 70)
            print("")
            print("Please edit credentials.ini with real credentials:")
            print("  - MQTT_USERNAME")
            print("  - MQTT_PASSWORD")
            print("  - OTA_PASSWORD")
            print("")
            print("=" * 70)
            print("")
