# ESP32 Arduino Client

Arduino/C++ client for LilyGo T5 e-paper displays. Shows real-time Slack and Discord reactions with power management and battery monitoring. Part of [Pebl](https://pebl.ink).

## Supported Devices

### Tested & Working (2.13", 212x104)

| Environment | Display Chip | Mode |
|-------------|-------------|------|
| `lilygo_t5_gdew_4g` | GDEW0213T5D | 4-level grayscale (**recommended**) |
| `lilygo_t5_gdew_bw` | GDEW0213T5D | BW only |
| `lilygo_t5_depg_bw` | DEPG0213BN | BW (most common T5 V2.3.1) |

### Untested (Should Work)

| Environment | Display Chip | Mode |
|-------------|-------------|------|
| `lilygo_t5_gdem_4g` | GDEM0213B74 | 4-level grayscale |
| `lilygo_t5_gdey_4g` | GDEY0213B74 | 4-level grayscale |
| `lilygo_t5s_27` | GDEY027T91 | BW (2.7", 264x176) |

**Important**: Match your environment to your actual display chip label. Do NOT use `gdem_4g` or `gdey_4g` for GDEW0213T5D displays.

## Features

- Real-time WebSocket (WSS) connection with auto-reconnect and exponential backoff
- AES-256-CBC / ECDH+AES encryption with lock/unlock status indicator
- Graphical emoji rendering (PNG download + decode) with text fallback
- Smart deep sleep on battery, always-awake on USB
- 1-minute wake cycles during work hours, 30-minute during quiet hours/weekends
- Battery indicator (3-dot display with charging icon)
- Automatic timezone detection via server GeoIP
- Graceful shutdown with proper resource cleanup
- Configuration via SPIFFS JSON file

## Quick Start

### Flash a Device

```bash
./flash.sh           # Interactive: prompts for name, variant, rotation
./flash.sh --dry-run  # Preview config without flashing
```

The script auto-detects the USB port, writes config, uploads filesystem, and flashes firmware.

| Default | Value |
|---------|-------|
| Name | `pebl` |
| Variant | `lilygo_t5_gdew_4g` |
| Rotation | `1` (landscape) |

### Manual Flash

```bash
# 1. Edit data/config.json
# 2. Upload filesystem + firmware
pio run -e lilygo_t5_gdew_4g --target uploadfs --upload-port /dev/cu.usbserial-XXXX
pio run -e lilygo_t5_gdew_4g --target upload --upload-port /dev/cu.usbserial-XXXX
pio device monitor -b 115200
```

## Configuration

### Power Management

| Setting | Default | Description |
|---------|---------|-------------|
| `sleep_enabled` | `true` | Deep sleep on battery |
| `sleep_duration_min` | `1` | Minutes between wake cycles |
| `battery_pin` | `36` | GPIO for battery voltage |
| `usb_threshold_v` | `4.2` | Voltage threshold for USB detection |

**Behavior**: USB = always awake. Battery = 1-min wake cycles (active hours), 30-min cycles (quiet hours). ~6 days on 2000mAh battery.

### Quiet Hours

```json
"quiet_hours": {
  "start_hour": 23,
  "end_hour": 7,
  "sleep_multiplier": 6
}
```

Weekdays: quiet hours from 11 PM - 7 AM. Weekends: quiet all day.

### Display Policy

```json
"display_policy": {
  "skip_refresh_on_no_message": true,
  "show_sleep_text": false,
  "update_battery_on_wake": false
}
```

With `skip_refresh_on_no_message` enabled, the display only refreshes when new reactions arrive (~20 refreshes/day instead of ~1,440).

### Display Rotation

`0` = portrait, `1` = landscape (default), `2` = portrait inverted, `3` = landscape inverted

## Boot Button

The BOOT button (GPIO 39) supports three actions based on hold duration:

| Hold | Action | Details |
|------|--------|---------|
| Short press | Wake | Wakes device from deep sleep |
| 3–14 seconds | Add platform | Shows QR code to link an additional platform (e.g., add Discord to a Slack-linked device) |
| 15+ seconds | WiFi setup | Launches captive portal for WiFi provisioning |

At the 3-second mark, the display shows feedback so you know when to release. The "Add platform" QR code points to the `/connect` page with device credentials pre-filled. After 3 minutes the device restarts automatically to connect with all linked platforms. Press the button to restart immediately.

The "Add platform" option only appears if the device is already paired (has an auth token). On an unpaired device, only WiFi provisioning is available.

## Project Structure

```
esp32_arduino_client/
├── src/
│   ├── main.cpp                 # Main application
│   ├── config/ConfigManager.*   # Configuration management
│   ├── security/SecurityManager.*  # Encryption
│   └── resilience/ResilienceManager.*  # Connection health
├── data/config.json             # Device configuration
├── docs/                        # Detailed guides
├── platformio.ini               # Build configuration
└── flash.sh                     # Interactive flash script
```

## Troubleshooting

See [docs/INSTALLATION_GUIDE.md](docs/INSTALLATION_GUIDE.md) for detailed troubleshooting.

| Problem | Quick Fix |
|---------|-----------|
| Display not working | Verify display type matches hardware. See [Display Support](docs/DISPLAY_SUPPORT.md) |
| WiFi won't connect | Check SSID/password in config.json. Must be 2.4GHz |
| WebSocket fails | Verify `wss://` URL and device registration |
| Upload errors | Hold BOOT button during upload. Use a data-capable USB cable |

## Documentation

- [Installation Guide](docs/INSTALLATION_GUIDE.md) — Setup, auth, and troubleshooting
- [Display Support](docs/DISPLAY_SUPPORT.md) — Driver details and identification
- [Security Notes](docs/SECURITY_NOTES.md) — Encryption and key management

## License

See main project LICENSE file.
