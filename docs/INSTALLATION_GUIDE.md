# ESP32 Installation Guide

Complete step-by-step guide for installing the Slack Reactions client on your LilyGo T5 ESP32.

**Quick overview:** For a high-level summary, see [README.md](../README.md#quick-start)

## Prerequisites

1. **Hardware**
   - LilyGo T5 V2.3.1 (or compatible) - See [supported devices](../README.md#supported-devices)
   - USB-C cable (data capable, not charge-only)

2. **Software**
   - PlatformIO Core CLI or VSCode with PlatformIO extension
   - Python 3.x for serial monitoring
   - Git for cloning the repository

## Step-by-Step Installation

### 1. Clone and Navigate to Project

```bash
cd ~/Documents/Personal\ Apps/slack-reactions
cd esp32_arduino_client
```

### 2. Configure Device Settings

Edit `data/config.json` with your specific settings.

**📖 For complete configuration reference including power management, quiet hours, and display policies, see [README.md - Configuration](../README.md#configuration)**

**Minimal example:**

```json
{
  "device": {
    "id": "your_unique_device_id",  // Must match server registration
    "name": "Living Room Display"
  },
  "wifi": {
    "ssid": "YourWiFiName",
    "password": "YourWiFiPassword",
    "timeout_ms": 15000,
    "reconnect_interval_ms": 30000
  },
  "server": {
    "host": "slack-reactions.your-domain.com",  // Your server
    "port": 443,
    "path": "/ws-stream",
    "use_ssl": true
  },
  "security": {
    "use_aes": true,
    "aes_key": "dGhpc2lzYXRlc3RrZXlmb3JhZXMyNTZlbmNyeXB0aW8="  // 32-byte key in base64
  },
  "logging": {
    "default_level": "WARN",  // Change to "INFO" for debugging
    "enable_test_commands": false  // Set to true for debugging
  }
}
```

### 3. Generate AES Key (Optional)

If you need a new AES key:

```bash
# Generate random 32-byte key and encode to base64
openssl rand -base64 32 | head -c 44
```

**⚠️ Security note:** For important security considerations about key transmission, see [SECURITY_NOTES.md](SECURITY_NOTES.md)

### 4. Build and Upload Firmware

#### First Time Setup (Clean Install)

```bash
# 1. Clean any previous builds
pio run -t clean

# 2. Build the firmware
pio run

# 3. Upload the firmware
pio run -t upload

# 4. Upload the configuration file to SPIFFS
pio run -t uploadfs
```

#### If Serial Port is Busy

```bash
# Kill any processes using the serial port
pkill -f screen
pkill -f "/dev/cu.usbserial"

# Then retry upload
pio run -t upload
```

### 5. Monitor Device Boot

```bash
# Using screen (Press Ctrl+A, then K to exit)
screen /dev/cu.usbserial-595D0191541 115200

# Or using PlatformIO
pio device monitor -b 115200

# Or using Python
python3 -c "
import serial
ser = serial.Serial('/dev/cu.usbserial-595D0191541', 115200)
while True:
    print(ser.readline().decode('utf-8', errors='replace').rstrip())
"
```

## Troubleshooting

### Device Not Connecting to WiFi

1. **Check SPIFFS has config**:
   ```
   [INFO][CONFIG] Configuration loaded successfully
   ```
   If you see "using defaults", SPIFFS upload failed.

2. **Re-upload SPIFFS**:
   ```bash
   # Make sure serial monitor is closed first!
   pio run -t uploadfs
   ```

3. **Verify WiFi credentials** in config.json
4. **Check 2.4GHz network** - ESP32 doesn't support 5GHz WiFi

### Server Rejects Connection

Look for: `websocket_unknown_device`

**Solution**: Device ID not registered on server
1. Register device on server first
2. Link to Slack account via app home
3. Ensure device ID in config.json matches exactly

### AES Encryption Not Working

Check Slack app home shows "No Encryption"

**Solution**: ESP32 needs to send AES key
1. Verify `use_aes: true` in config.json
2. Check AES key is exactly 32 bytes (44 chars base64)
3. Restart device to trigger registration

### Complete Reset (Nuclear Option)

If things aren't working, do a complete reset:

```bash
# 1. Erase entire flash including SPIFFS
pio run -t erase

# 2. Upload firmware
pio run -t upload

# 3. Upload config to SPIFFS
pio run -t uploadfs

# 4. Monitor boot sequence
screen /dev/cu.usbserial-595D0191541 115200
```

## Successful Connection Indicators

When everything is working correctly:

1. **WiFi Connected**:
   ```
   [INFO][WIFI] Connected | ip=192.168.x.x rssi=-50
   ```

2. **WebSocket Connected**:
   ```
   [INFO][WS] Connected | url=/ws-stream?rpi_id=your_device_id
   ```

3. **AES Registration Sent**:
   ```
   [INFO][WS] Sent AES registration
   ```

4. **Server Accepts Connection**:
   - No disconnection messages
   - Display shows "Waiting for reactions..."

## Production Deployment

For production use:

1. **Disable Debug Features**:
   - Keep `ENABLE_DEBUG_FEATURES` commented out in main.cpp
   - Set log level to "WARN" in config.json
   - Set `enable_test_commands` to false

2. **Secure Your Keys**:
   - Use a unique, strong AES key
   - Don't commit keys to version control
   - Consider using different keys per device

3. **Monitor Health**:
   - Check Slack app home for connection status
   - Device shows as "Connected" with green status
   - AES encryption confirmed

## Common Issues and Solutions

| Problem | Symptom | Solution |
|---------|---------|----------|
| SPIFFS empty | SSID empty, using defaults | Upload SPIFFS: `pio run -t uploadfs` |
| Wrong device ID | Server rejects connection | Update config.json with registered ID |
| Serial port busy | Upload fails | Close serial monitor, kill screen processes |
| AES key wrong length | AES init fails | Use exactly 32-byte key (44 chars base64) |
| Old config cached | Changes don't apply | Erase flash and re-upload everything |

## Next Steps

After successful installation:

1. **Test Reactions**: Have someone react to your Slack message
2. **Monitor Logs**: Check for encrypted message receipt
3. **Verify Display**: E-paper should update with reaction details
4. **Production Mode**: Disable debug features for daily use

## Support

- Check serial output for error messages
- Verify server logs for connection attempts
- Ensure device ID is registered and linked
- Review this guide for common issues