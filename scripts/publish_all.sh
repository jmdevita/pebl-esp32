#!/bin/bash
# Publish all firmware variants to server
# Usage: ./scripts/publish_all.sh 1.0.3 "Bug fixes" [admin_api_key]

set -e  # Exit on error

VERSION="$1"
CHANGELOG="${2:-Release $VERSION}"
API_KEY="${3:-$FIRMWARE_ADMIN_KEY}"  # Can set FIRMWARE_ADMIN_KEY env var
SERVER_URL="${FIRMWARE_SERVER_URL:-https://slack-reactions.devita.dev}"

if [ -z "$VERSION" ]; then
    echo "Usage: $0 <version> [changelog] [api_key]"
    echo "Example: $0 1.0.3 'Bug fixes and improvements'"
    echo ""
    echo "Set FIRMWARE_ADMIN_KEY env var to avoid passing API key:"
    echo "  export FIRMWARE_ADMIN_KEY='your_key'"
    echo "  $0 1.0.3 'Bug fixes'"
    exit 1
fi

if [ -z "$API_KEY" ]; then
    echo "Error: API key not provided"
    echo "Either:"
    echo "  1. Pass as 3rd argument: $0 $VERSION '$CHANGELOG' 'your_key'"
    echo "  2. Set env var: export FIRMWARE_ADMIN_KEY='your_key'"
    exit 1
fi

echo "========================================"
echo "Publishing Firmware: v$VERSION"
echo "Server: $SERVER_URL"
echo "Changelog: $CHANGELOG"
echo "========================================"

# Get absolute path to project root
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Variants to publish (add/remove as needed)
VARIANTS=(
    "lilygo_t5_depg_bw"
    "lilygo_t5_gdew_4g"
    # Add more variants as needed:
    # "lilygo_t5_gdew_bw"
    # "lilygo_t5_gdem_4g"
    # "lilygo_t5_gdey_4g"
)

SUCCESS_COUNT=0
FAIL_COUNT=0

for variant in "${VARIANTS[@]}"; do
    FIRMWARE_PATH="$PROJECT_ROOT/.pio/build/$variant/firmware.bin"

    if [ ! -f "$FIRMWARE_PATH" ]; then
        echo "✗ $variant - firmware.bin not found (skipping)"
        echo "  Run: pio run -e $variant"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        continue
    fi

    echo ""
    echo "Publishing $variant..."

    RESPONSE=$(curl -s -X POST "$SERVER_URL/api/firmware/admin/publish" \
        -H "Content-Type: application/json" \
        -d "{
            \"version\": \"$VERSION\",
            \"display_variant\": \"$variant\",
            \"file_path\": \"$FIRMWARE_PATH\",
            \"changelog\": \"$CHANGELOG\",
            \"required\": false,
            \"api_key\": \"$API_KEY\"
        }")

    # Check if response contains "status":"published"
    if echo "$RESPONSE" | grep -q '"status":"published"'; then
        echo "  ✓ $variant published successfully"
        SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
    else
        echo "  ✗ $variant publish failed"
        echo "  Response: $RESPONSE"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
done

echo ""
echo "========================================"
echo "Summary:"
echo "  ✓ Success: $SUCCESS_COUNT"
if [ $FAIL_COUNT -gt 0 ]; then
    echo "  ✗ Failed:  $FAIL_COUNT"
fi
echo "========================================"

exit $FAIL_COUNT
