#!/bin/bash
# Flash ESP32 firmware and config to a device
# Usage: ./flash.sh [--dry-run] [--erase]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIG_FILE="$SCRIPT_DIR/data/config.json"
CONFIG_EXAMPLE="$SCRIPT_DIR/data/config.json.example"

# ============================================================================
# Editable defaults — change these for your setup
# ============================================================================
DEFAULT_NAME="pebl"
DEFAULT_VARIANT="lilygo_t5_gdew_4g"
DEFAULT_ROTATION=1
# ============================================================================

# Parse arguments
DRY_RUN=false
ERASE=false
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=true ;;
        --erase)   ERASE=true ;;
    esac
done

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

info()  { echo -e "${CYAN}[INFO]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; }
success() { echo -e "${GREEN}[OK]${NC} $1"; }

echo ""
echo -e "${BOLD}╔══════════════════════════════════════╗${NC}"
if $DRY_RUN; then
echo -e "${BOLD}║  ESP32 E-Paper Flasher ${YELLOW}(DRY RUN)${NC}${BOLD}   ║${NC}"
else
if $ERASE; then
echo -e "${BOLD}║  ESP32 E-Paper Flasher ${RED}(ERASE)${NC}${BOLD}      ║${NC}"
else
echo -e "${BOLD}║     ESP32 E-Paper Device Flasher     ║${NC}"
fi
fi
echo -e "${BOLD}╚══════════════════════════════════════╝${NC}"
echo ""

# --- Check prerequisites ---
if ! command -v pio &> /dev/null; then
    error "PlatformIO CLI not found. Install with: pip install platformio"
    exit 1
fi

if $ERASE && ! command -v esptool.py &> /dev/null; then
    error "esptool.py not found. Install with: pip install esptool"
    exit 1
fi

# --- Detect USB port ---
info "Detecting USB serial port..."
PORTS=($(ls /dev/cu.usbserial-* 2>/dev/null || true))

if [ ${#PORTS[@]} -eq 0 ]; then
    if $DRY_RUN; then
        PORT="(no device connected)"
        warn "No USB serial devices found (dry run — continuing anyway)"
    else
        error "No USB serial devices found. Is the ESP32 plugged in?"
        exit 1
    fi
elif [ ${#PORTS[@]} -eq 1 ]; then
    PORT="${PORTS[0]}"
    success "Found port: $PORT"
else
    echo ""
    echo "Multiple USB serial devices found:"
    for i in "${!PORTS[@]}"; do
        echo "  $((i+1))) ${PORTS[$i]}"
    done
    echo ""
    read -p "Select port [1-${#PORTS[@]}]: " PORT_CHOICE
    PORT="${PORTS[$((PORT_CHOICE-1))]}"
    if [ -z "$PORT" ]; then
        error "Invalid selection"
        exit 1
    fi
fi

# --- Prompt for config ---
echo ""
echo -e "${BOLD}Device Configuration${NC}"
echo "Press Enter to accept defaults shown in [brackets]."
echo ""

read -p "  Device name [$DEFAULT_NAME]: " INPUT_NAME
DEVICE_NAME="${INPUT_NAME:-$DEFAULT_NAME}"

echo ""
echo "  Display variants:"
echo "    1) lilygo_t5_gdew_4g   (GDEW0213T5D 4-level grayscale) *recommended"
echo "    2) lilygo_t5_gdew_bw   (GDEW0213T5D black & white)"
echo "    3) lilygo_t5_depg_bw   (DEPG0213BN black & white)"
echo "    4) lilygo_t5_gdem_4g   (GDEM0213B74 4-level grayscale)"
echo "    5) lilygo_t5_gdey_4g   (GDEY0213B74 4-level grayscale)"
echo ""

VARIANT_OPTIONS=("lilygo_t5_gdew_4g" "lilygo_t5_gdew_bw" "lilygo_t5_depg_bw" "lilygo_t5_gdem_4g" "lilygo_t5_gdey_4g")

# Find default index
DEFAULT_VARIANT_NUM=1
for i in "${!VARIANT_OPTIONS[@]}"; do
    if [ "${VARIANT_OPTIONS[$i]}" = "$DEFAULT_VARIANT" ]; then
        DEFAULT_VARIANT_NUM=$((i+1))
        break
    fi
done

read -p "  Select variant [$DEFAULT_VARIANT_NUM]: " INPUT_VARIANT_NUM
VARIANT_NUM="${INPUT_VARIANT_NUM:-$DEFAULT_VARIANT_NUM}"
DISPLAY_VARIANT="${VARIANT_OPTIONS[$((VARIANT_NUM-1))]}"

if [ -z "$DISPLAY_VARIANT" ]; then
    error "Invalid variant selection"
    exit 1
fi

read -p "  Display rotation (0-3) [$DEFAULT_ROTATION]: " INPUT_ROTATION
DISPLAY_ROTATION="${INPUT_ROTATION:-$DEFAULT_ROTATION}"

# --- Read existing config and patch values ---
if [ ! -f "$CONFIG_FILE" ]; then
    if [ -f "$CONFIG_EXAMPLE" ]; then
        cp "$CONFIG_EXAMPLE" "$CONFIG_FILE"
        warn "Created config.json from example template"
    else
        error "No config.json or config.json.example found in data/"
        exit 1
    fi
fi

# --- Summary ---
echo ""
echo -e "${BOLD}┌──────────────────────────────────────┐${NC}"
echo -e "${BOLD}│  Flash Summary                       │${NC}"
echo -e "${BOLD}├──────────────────────────────────────┤${NC}"
printf "${BOLD}│${NC}  %-14s %-22s${BOLD}│${NC}\n" "Port:" "$PORT"
printf "${BOLD}│${NC}  %-14s %-22s${BOLD}│${NC}\n" "Name:" "$DEVICE_NAME"
printf "${BOLD}│${NC}  %-14s %-22s${BOLD}│${NC}\n" "Variant:" "$DISPLAY_VARIANT"
printf "${BOLD}│${NC}  %-14s %-22s${BOLD}│${NC}\n" "Rotation:" "$DISPLAY_ROTATION"
printf "${BOLD}│${NC}  %-14s %-22s${BOLD}│${NC}\n" "Device ID:" "(auto from MAC)"
if $ERASE; then
printf "${BOLD}│${NC}  %-14s ${RED}%-22s${NC}${BOLD}│${NC}\n" "Erase:" "FULL FLASH WIPE"
fi
if $DRY_RUN; then
printf "${BOLD}│${NC}  %-14s ${YELLOW}%-22s${NC}${BOLD}│${NC}\n" "Mode:" "DRY RUN"
fi
echo -e "${BOLD}└──────────────────────────────────────┘${NC}"

if $DRY_RUN; then
    echo ""
    info "Config that would be written:"
    echo ""
    python3 -c "
import json
with open('$CONFIG_FILE', 'r') as f:
    config = json.load(f)
config['device']['name'] = '$DEVICE_NAME'
config['device']['display_variant'] = '$DISPLAY_VARIANT'
config['device']['id'] = ''
config['display']['rotation'] = $DISPLAY_ROTATION
print(json.dumps(config, indent=2))
"
    echo ""
    success "Dry run complete. No changes were made."
    info "Run without --dry-run to flash for real."
    exit 0
fi

# --- Write config ---
python3 -c "
import json
with open('$CONFIG_FILE', 'r') as f:
    config = json.load(f)
config['device']['name'] = '$DEVICE_NAME'
config['device']['display_variant'] = '$DISPLAY_VARIANT'
config['device']['id'] = ''
config['display']['rotation'] = $DISPLAY_ROTATION
with open('$CONFIG_FILE', 'w') as f:
    json.dump(config, f, indent=2)
    f.write('\n')
"

echo ""
read -p "Proceed with flash? [Y/n]: " CONFIRM
CONFIRM="${CONFIRM:-Y}"
if [[ ! "$CONFIRM" =~ ^[Yy]$ ]]; then
    info "Aborted."
    exit 0
fi

# --- Erase flash (if requested) ---
if $ERASE; then
    echo ""
    info "Erasing entire flash..."
    esptool.py --port "$PORT" erase_flash
    success "Flash erased"
fi

# --- Flash filesystem (config + certs) ---
echo ""
info "Uploading filesystem (config + certificates)..."
pio run -e "$DISPLAY_VARIANT" --target uploadfs --upload-port "$PORT"
success "Filesystem uploaded"

# --- Flash firmware ---
echo ""
info "Flashing firmware..."
pio run -e "$DISPLAY_VARIANT" --target upload --upload-port "$PORT"
success "Firmware flashed"

# --- Done ---
echo ""
echo -e "${GREEN}${BOLD}Device flashed successfully!${NC}"
echo ""
echo "  The device will boot and start a WiFi captive portal"
echo "  for network setup (unless seed_networks are configured)."
echo ""
