# ESP32 Arduino Client Configuration

## Setup Instructions

1. Copy the example config file:
   ```bash
   cp config.json.example config.json
   ```

2. Edit `config.json` with your settings:
   - **WiFi credentials**: Managed automatically via WiFi provisioning mode (no config needed)
   - **Server URL**: Update `server.host` with your server address
   - **Device ID**: Update `device.id` with a unique identifier
   - **AES Key**: Update `security.aes_key` with your encryption key

3. Upload to your ESP32 device using PlatformIO or Arduino IDE

## Important Security Notes

⚠️ **NEVER commit `config.json` to version control!**

- The file contains sensitive credentials (AES keys, server URLs)
- WiFi credentials are stored securely in ESP32 NVS (not in config.json)
- Only `config.json.example` should be tracked in git
- Always use the example file as a template for new devices
