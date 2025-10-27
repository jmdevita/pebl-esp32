#!/bin/bash
# Release script for ESP32 firmware
# Usage: ./scripts/release.sh 1.0.3 "Bug fixes and improvements"

set -e  # Exit on error

VERSION="$1"
CHANGELOG="${2:-Release $VERSION}"

if [ -z "$VERSION" ]; then
    echo "Usage: $0 <version> [changelog]"
    echo "Example: $0 1.0.3 'Bug fixes and improvements'"
    exit 1
fi

echo "========================================"
echo "Creating Release: v$VERSION"
echo "Changelog: $CHANGELOG"
echo "========================================"

# Update version.txt
echo "$VERSION" > version.txt
echo "✓ Updated version.txt"

# Build all variants
echo ""
echo "Building all variants..."
VARIANTS=(
    "lilygo_t5_depg_bw"
    "lilygo_t5_gdew_bw"
    "lilygo_t5_gdem_4g"
    "lilygo_t5_gdew_4g"
    "lilygo_t5_gdey_4g"
    "lilygo_t5_v231"
    "lilygo_t5_213"
    "lilygo_t5s_27"
)

for variant in "${VARIANTS[@]}"; do
    echo "  Building $variant..."
    pio run -e "$variant" > /dev/null 2>&1
    if [ $? -eq 0 ]; then
        echo "    ✓ $variant built successfully"
    else
        echo "    ✗ $variant build failed"
        exit 1
    fi
done

echo ""
echo "========================================"
echo "✓ All variants built successfully!"
echo "========================================"
echo ""
echo "Firmware binaries are in .pio/build/<variant>/firmware.bin"
echo ""
echo "Next steps:"
echo "1. Test firmware on a device"
echo "2. Publish to server with:"
echo "   ./scripts/publish_all.sh $VERSION '$CHANGELOG'"
echo ""
