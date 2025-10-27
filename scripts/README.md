# Build and Release Scripts

This directory contains helper scripts for managing firmware versions, building, and publishing releases.

## Scripts Overview

### set_version.py
**PlatformIO pre-build script** - Automatically executed before each build.

- Reads version from `../version.txt`
- Sets `APP_VERSION` build flag
- Embeds version in ESP32 app descriptor
- **Do not run manually** - PlatformIO runs this automatically

### bump_version.sh
**Version increment helper**

```bash
# Increment patch version (1.0.2 -> 1.0.3)
./scripts/bump_version.sh patch

# Increment minor version (1.0.2 -> 1.1.0)
./scripts/bump_version.sh minor

# Increment major version (1.0.2 -> 2.0.0)
./scripts/bump_version.sh major
```

### release.sh
**Build all firmware variants**

```bash
# Build all 8 display variants with current version
./scripts/release.sh 1.0.3 "Bug fixes and improvements"

# Or use current version from file
./scripts/release.sh $(cat version.txt) "Your changelog"
```

**What it does:**
1. Updates `version.txt` with specified version
2. Builds all supported display variants
3. Reports success/failure for each build

### publish_all.sh
**Publish firmware to server**

```bash
# Set admin API key (do once per session)
export FIRMWARE_ADMIN_KEY="your_admin_api_key"

# Publish all built variants
./scripts/publish_all.sh 1.0.3 "Bug fixes and improvements"
```

**Environment Variables:**
- `FIRMWARE_ADMIN_KEY` - Admin API key (required)
- `FIRMWARE_SERVER_URL` - Server URL (default: https://slack-reactions.devita.dev)

## Quick Start

### Full Release Workflow

```bash
# 1. Bump version
./scripts/bump_version.sh patch

# 2. Build all variants
./scripts/release.sh $(cat version.txt) "Your changelog"

# 3. Test on one device
pio run -e lilygo_t5_gdew_4g --target upload
pio device monitor

# 4. Publish to server
export FIRMWARE_ADMIN_KEY="your_key"
./scripts/publish_all.sh $(cat version.txt) "Your changelog"
```

## Documentation

See **[docs/VERSION_MANAGEMENT.md](../docs/VERSION_MANAGEMENT.md)** for complete documentation on:
- Version management system
- Semantic versioning guidelines
- CI/CD integration
- Troubleshooting
- Best practices

## Requirements

- **bash** - For shell scripts
- **Python 3** - For set_version.py (used by PlatformIO)
- **PlatformIO** - For building firmware
- **curl** - For publish_all.sh (uploading to server)

All scripts are designed to work on macOS, Linux, and Windows (with Git Bash or WSL).
