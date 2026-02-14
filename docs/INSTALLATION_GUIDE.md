# ESP32 Installation Guide

Step-by-step guide for installing the Slack Reactions client on your LilyGo T5 ESP32.

**Quick overview:** See [README.md](../README.md#quick-start)

## Prerequisites

1. **Hardware** - LilyGo T5 V2.3.1 (or compatible), USB-C data cable
2. **Software** - PlatformIO Core CLI or VSCode with PlatformIO extension

## Step-by-Step Installation

### 1. Clone and Navigate

```bash
cd slack-reactions/esp32_arduino_client
```

### 2. Configure Device

```bash
cp data/config.json.example data/config.json
```

Edit `data/config.json`:

```json
{
  "device": {
    "id": "",
    "name": "My E-Paper Display",
    "display_variant": "lilygo_t5_depg_bw"
  },
  "wifi": {
    "seed_networks": [
      {"ssid": "YourWiFiName", "password": "YourWiFiPassword"}
    ]
  },
  "server": {
    "host": "slack-reactions.your-domain.com",
    "port": 443,
    "path": "/ws-stream",
    "use_ssl": true
  },
  "security": {
    "auth_token": "",
    "encryption": "ecdh"
  },
  "logging": {
    "default_level": "WARN"
  }
}
```

- `device.id` - Leave empty to auto-generate from hardware MAC (recommended)
- `device.display_variant` - Must match your PlatformIO environment (see [DISPLAY_SUPPORT.md](DISPLAY_SUPPORT.md))
- `security.auth_token` - Leave empty for self-service pairing on first boot
- `security.encryption` - `"ecdh"` (default, auto-generated keys) or `"none"`

Full config reference: see `data/config.json.example`

### 3. Build and Upload

```bash
# Build firmware
pio run -e lilygo_t5_depg_bw

# Upload firmware
pio run -e lilygo_t5_depg_bw -t upload

# Upload config to LittleFS
pio run -e lilygo_t5_depg_bw -t uploadfs
```

### 4. Monitor Boot

```bash
pio device monitor -b 115200
```

### 5. Device Pairing

On first boot with an empty `auth_token`, the device enters **self-service pairing mode**:

1. Device requests a pairing code from the server
2. 8-character code is displayed on the e-paper screen
3. Enter the code in the Slack app home tab
4. Device saves `auth_token` automatically and continues boot

If the device shows "Setup Required", register it on the server admin panel first.

### 6. Done

After pairing, the device automatically generates ECDH encryption keys, uploads the public key to the server, connects via WebSocket, and checks for firmware updates. No manual setup needed.

## WiFi Provisioning

If WiFi connection fails, the device opens a captive portal AP (`SlackReact-Setup`) with a QR code on the e-paper. Connect and configure WiFi from your phone.

**Manual trigger:** Hold the button (GPIO 39) for 3+ seconds during wake.

## Troubleshooting

### Device Not Connecting to WiFi

1. Check logs for `Configuration loaded successfully` (if "using defaults", re-upload: `pio run -t uploadfs`)
2. Verify WiFi credentials in config.json
3. ESP32 only supports 2.4GHz networks

### Server Rejects Connection

Look for `websocket_unknown_device` — device ID not registered or auth token invalid.

### Complete Reset

```bash
pio run -t erase && pio run -t upload && pio run -t uploadfs
```

Erases keys and auth token. Device will re-generate keys and enter pairing mode.

## Successful Connection Indicators

```
[INFO][CONFIG] Configuration loaded successfully
[INFO][WIFI] Connected | ip=192.168.x.x rssi=-50
[INFO][SYSTEM] ECDH P-256 encryption enabled
[INFO][SYSTEM] ECDH public key uploaded to server
[INFO][WS] Connected | url=/ws-stream?rpi_id=your_device_id
```
