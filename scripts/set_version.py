#!/usr/bin/env python3
"""
PlatformIO Pre-Build Script: Set Firmware Version from version.txt

Reads version from version.txt in project root and sets it as a build flag.
The version is embedded in the ESP32 app descriptor and can be retrieved via:
    const esp_app_desc_t* app_desc = esp_ota_get_app_description();
    String version = String(app_desc->version);

Usage:
    Add to platformio.ini:
    [env]
    extra_scripts = pre:scripts/set_version.py
"""

Import("env")
import os
import sys

# Get project directory
project_dir = env.get("PROJECT_DIR")
version_file = os.path.join(project_dir, "version.txt")

# Check if version.txt exists
if not os.path.exists(version_file):
    print("=" * 70)
    print("ERROR: version.txt not found!")
    print(f"Expected location: {version_file}")
    print("Please create version.txt with your firmware version (e.g., '1.0.2')")
    print("=" * 70)
    sys.exit(1)

# Read version from file
try:
    with open(version_file, 'r') as f:
        version = f.read().strip()
except Exception as e:
    print("=" * 70)
    print(f"ERROR: Failed to read version.txt: {e}")
    print("=" * 70)
    sys.exit(1)

# Validate version format (basic check)
if not version or len(version) == 0:
    print("=" * 70)
    print("ERROR: version.txt is empty!")
    print("Please add a version number (e.g., '1.0.2')")
    print("=" * 70)
    sys.exit(1)

if len(version) > 32:
    print("=" * 70)
    print(f"WARNING: Version string is very long ({len(version)} chars): {version}")
    print("ESP32 app descriptor limits version to 32 characters")
    print("Truncating to 32 characters...")
    print("=" * 70)
    version = version[:32]

# Set the version as a build flag
# This will be embedded in the ESP32 app descriptor
env.Append(CPPDEFINES=[
    ("APP_VERSION", f'\\"{version}\\"')
])

# Also set project version (used by ESP-IDF)
env.Replace(PROJECT_VER=version)

# Print version info during build
print("=" * 70)
print(f"  Building firmware version: {version}")
print(f"  Source: {version_file}")
print("=" * 70)
