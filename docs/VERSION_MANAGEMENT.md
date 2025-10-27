# Version Management Guide

## Overview

This project uses a **version.txt-based system** for managing firmware versions. The version is automatically read during build time and embedded into the ESP32 firmware binary.

**Benefits:**
- ✅ Single source of truth (`version.txt`)
- ✅ All build variants automatically use the same version
- ✅ Easy automation and scripting
- ✅ Version consistency between firmware and server
- ✅ No manual editing of platformio.ini required

---

## How It Works

### Components

1. **version.txt** - Plain text file containing the current version (e.g., `1.0.2`)
2. **scripts/set_version.py** - PlatformIO pre-build script that reads version.txt
3. **platformio.ini** - Configured to run the script before each build

### Build Flow

```
1. You run: pio run -e lilygo_t5_gdew_4g
2. PlatformIO executes: scripts/set_version.py (pre-build)
3. Script reads: version.txt
4. Script sets: -D APP_VERSION="1.0.2"
5. Firmware compiles with version embedded
6. Result: firmware.bin with version 1.0.2
```

### Version Retrieval in Code

The version is automatically available via ESP-IDF:

```cpp
const esp_app_desc_t* app_desc = esp_ota_get_app_description();
String version = String(app_desc->version);
Serial.printf("Firmware Version: %s\n", version.c_str());
```

This is used by the OTA system to check for updates.

---

## Usage

### Basic Version Update

```bash
# Update version number
echo "1.0.3" > version.txt

# Build firmware (automatically uses new version)
pio run -e lilygo_t5_gdew_4g
```

### Using Helper Scripts

#### 1. Bump Version Automatically

```bash
# Increment patch version (1.0.2 -> 1.0.3)
./scripts/bump_version.sh patch

# Increment minor version (1.0.2 -> 1.1.0)
./scripts/bump_version.sh minor

# Increment major version (1.0.2 -> 2.0.0)
./scripts/bump_version.sh major
```

#### 2. Build All Variants

```bash
# Build all 8 display variants with current version
./scripts/release.sh $(cat version.txt) "Bug fixes and improvements"
```

#### 3. Publish to Server

```bash
# Set your admin API key
export FIRMWARE_ADMIN_KEY="your_admin_api_key"

# Publish all built variants to server
./scripts/publish_all.sh $(cat version.txt) "Bug fixes and improvements"
```

---

## Version Numbering Convention

We follow **Semantic Versioning** (SemVer):

```
MAJOR.MINOR.PATCH[-PRERELEASE]
```

### Examples

| Version | Type | Description |
|---------|------|-------------|
| `1.0.0` | Stable | Initial release |
| `1.0.1` | Patch | Bug fixes, security patches |
| `1.1.0` | Minor | New features (backward compatible) |
| `2.0.0` | Major | Breaking changes (e.g., config format changes) |
| `1.0.2-beta.1` | Pre-release | Beta testing |
| `1.0.2-rc.1` | Release Candidate | Final testing before release |

### When to Bump

- **PATCH (1.0.X)**: Bug fixes, minor improvements, security patches
- **MINOR (1.X.0)**: New features, enhancements (backward compatible)
- **MAJOR (X.0.0)**: Breaking changes, config format changes, major rewrites

---

## Complete Release Workflow

### Step 1: Decide on Version Number

```bash
# Patch release (bug fixes): 1.0.2 -> 1.0.3
./scripts/bump_version.sh patch

# Minor release (new features): 1.0.2 -> 1.1.0
./scripts/bump_version.sh minor

# Major release (breaking changes): 1.0.2 -> 2.0.0
./scripts/bump_version.sh major
```

### Step 2: Build All Variants

```bash
# Reads version from version.txt and builds all 8 variants
./scripts/release.sh $(cat version.txt) "Your changelog here"
```

**Output:**
```
========================================
Creating Release: v1.0.3
Changelog: Bug fixes and improvements
========================================

Building all variants...
  Building lilygo_t5_depg_bw...
    ✓ lilygo_t5_depg_bw built successfully
  Building lilygo_t5_gdew_bw...
    ✓ lilygo_t5_gdew_bw built successfully
  ...
```

### Step 3: Test on One Device

```bash
# Flash to test device
pio run -e lilygo_t5_gdew_4g --target upload

# Monitor to verify version
pio device monitor
# Look for: Firmware Version: 1.0.3
```

### Step 4: Publish to Server

```bash
# Set admin API key (do this once per session)
export FIRMWARE_ADMIN_KEY="your_admin_api_key"

# Publish all variants
./scripts/publish_all.sh $(cat version.txt) "Your changelog here"
```

**Output:**
```
========================================
Publishing Firmware: v1.0.3
Server: https://slack-reactions.devita.dev
Changelog: Bug fixes and improvements
========================================

Publishing lilygo_t5_depg_bw...
  ✓ lilygo_t5_depg_bw published successfully
Publishing lilygo_t5_gdew_4g...
  ✓ lilygo_t5_gdew_4g published successfully
...
```

### Step 5: Verify Deployment

1. **Check device logs** for OTA update detection:
   ```
   [OTA] Update available: 1.0.2 → 1.0.3
   ```

2. **Check server logs** for downloads:
   ```
   firmware_download_started device_id=X display_variant=lilygo_t5_gdew_4g
   ```

3. **Query database** to see published firmware:
   ```sql
   SELECT version, display_variant, active, created_at
   FROM firmware_versions
   ORDER BY created_at DESC;
   ```

---

## Development Workflow

### Development Builds (Pre-release)

```bash
# Use pre-release version for testing
echo "1.1.0-beta.1" > version.txt

# Build and flash
pio run -e lilygo_t5_gdew_4g --target upload
```

### Quick Iteration

```bash
# No need to update version for local testing
# Just rebuild and flash
pio run -e lilygo_t5_gdew_4g --target upload
```

### Feature Branches

```bash
# Use descriptive pre-release tags
echo "1.1.0-feature-wifi-improvements" > version.txt

# Build and test
pio run -e lilygo_t5_gdew_4g
```

---

## Helper Scripts Reference

### bump_version.sh

**Purpose:** Automatically increment version number

**Usage:**
```bash
./scripts/bump_version.sh [major|minor|patch]
```

**Examples:**
```bash
# Current: 1.0.2
./scripts/bump_version.sh patch   # -> 1.0.3
./scripts/bump_version.sh minor   # -> 1.1.0
./scripts/bump_version.sh major   # -> 2.0.0
```

---

### release.sh

**Purpose:** Build all firmware variants with current version

**Usage:**
```bash
./scripts/release.sh <version> [changelog]
```

**Examples:**
```bash
./scripts/release.sh 1.0.3 "Bug fixes and improvements"
./scripts/release.sh $(cat version.txt) "Your changelog"
```

**What it does:**
1. Updates version.txt with specified version
2. Builds all 8 display variants
3. Reports success/failure for each variant

---

### publish_all.sh

**Purpose:** Publish all built firmware to server

**Usage:**
```bash
./scripts/publish_all.sh <version> [changelog] [api_key]
```

**Environment Variables:**
- `FIRMWARE_ADMIN_KEY` - Admin API key (recommended)
- `FIRMWARE_SERVER_URL` - Server URL (default: https://slack-reactions.devita.dev)

**Examples:**
```bash
# Using environment variable (recommended)
export FIRMWARE_ADMIN_KEY="your_key"
./scripts/publish_all.sh 1.0.3 "Bug fixes"

# Passing API key directly
./scripts/publish_all.sh 1.0.3 "Bug fixes" "your_key"
```

**What it does:**
1. Checks that firmware binaries exist
2. POSTs each variant to `/api/firmware/admin/publish`
3. Reports success/failure summary

---

## Troubleshooting

### Error: "version.txt not found"

**Cause:** version.txt is missing

**Solution:**
```bash
echo "1.0.0" > version.txt
```

---

### Error: "version.txt is empty"

**Cause:** version.txt exists but has no content

**Solution:**
```bash
echo "1.0.0" > version.txt
```

---

### Build shows wrong version

**Cause:** Cached build with old version

**Solution:**
```bash
# Clean build cache
pio run -e lilygo_t5_gdew_4g --target clean

# Rebuild
pio run -e lilygo_t5_gdew_4g
```

---

### Version mismatch between variants

**Cause:** Built different variants at different times

**Solution:**
```bash
# Rebuild all variants fresh
./scripts/release.sh $(cat version.txt) "Rebuild all variants"
```

---

### Publish script fails with "API key not provided"

**Cause:** FIRMWARE_ADMIN_KEY environment variable not set

**Solution:**
```bash
export FIRMWARE_ADMIN_KEY="your_admin_api_key"
./scripts/publish_all.sh 1.0.3 "Changelog"
```

---

## CI/CD Integration

### Example: GitHub Actions

```yaml
name: Build and Release Firmware

on:
  push:
    tags:
      - 'v*'

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2

      - name: Set up Python
        uses: actions/setup-python@v2
        with:
          python-version: '3.9'

      - name: Install PlatformIO
        run: pip install platformio

      - name: Extract version from tag
        run: echo "${GITHUB_REF#refs/tags/v}" > version.txt

      - name: Build all variants
        run: ./scripts/release.sh $(cat version.txt) "Release $(cat version.txt)"

      - name: Publish to server
        env:
          FIRMWARE_ADMIN_KEY: ${{ secrets.FIRMWARE_ADMIN_KEY }}
        run: ./scripts/publish_all.sh $(cat version.txt) "Release $(cat version.txt)"
```

---

## Best Practices

### 1. Keep version.txt in Git
```bash
git add version.txt
git commit -m "Bump version to 1.0.3"
```

### 2. Tag Releases in Git
```bash
git tag -a v1.0.3 -m "Release 1.0.3: Bug fixes"
git push origin v1.0.3
```

### 3. Document Changes in CHANGELOG.md
Keep a changelog alongside version.txt to track what changed in each version.

### 4. Test Before Publishing
Always test on at least one device before publishing to all devices via OTA.

### 5. Use Pre-release Versions for Testing
```bash
echo "1.1.0-beta.1" > version.txt
```

### 6. Coordinate with Server Version
Make sure the version you publish to the server matches the version in the firmware binary.

---

## Files

### version.txt
```
1.0.2
```
Plain text file containing current version. **Edit this to update version.**

### scripts/set_version.py
Python script executed by PlatformIO during build. Reads version.txt and sets build flags. **Do not edit unless you know what you're doing.**

### scripts/bump_version.sh
Helper script to increment version number automatically.

### scripts/release.sh
Helper script to build all firmware variants.

### scripts/publish_all.sh
Helper script to publish all variants to server.

---

## Related Documentation

- [OTA Update System](../../docs/OTA_VARIANT_GUIDE.md) - Display variant tracking and OTA updates
- [Installation Guide](./INSTALLATION_GUIDE.md) - Initial setup instructions
- [Display Support](./DISPLAY_SUPPORT.md) - Supported hardware variants

---

**Last Updated:** October 26, 2025
**Current Version:** See `version.txt` in project root
