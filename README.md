# ESP32 Arduino Slack Reactions Client

Arduino/C++ implementation for LilyGo T5 e-paper displays to show real-time Slack reactions with power management and battery monitoring.

## Supported Devices

### ✅ Tested & Working

**2.13" Displays (212×104 resolution):**
- **DEPG0213BN** - 2-level BW (most common T5 V2.3.1)
  - Environment: `lilygo_t5_depg_bw`
  - Driver: GxEPD2_213_B74
- **GDEW0213T5D** - 4-level grayscale (UC8151D controller)
  - Environment: `lilygo_t5_gdew_4g` (grayscale, **recommended**)
  - Driver: GxEPD2_213_flex (GDEW0213I5F)
  - Alternative: `lilygo_t5_gdew_bw` (BW only)

### ⚠️ Untested (Should Work)

**2.13" Displays:**
- **GDEM0213B74 / GDEY0213B74** - 4-level grayscale
  - Environments: `lilygo_t5_gdem_4g` / `lilygo_t5_gdey_4g`
  - **Note**: Do NOT use these for GDEW0213T5D displays

**2.7" Display:**
- **GDEY027T91** - 2-level BW (264×176)
  - Environment: `lilygo_t5s_27`

See [docs/DISPLAY_SUPPORT.md](docs/DISPLAY_SUPPORT.md) for detailed driver information and troubleshooting.

## Features

### Core Functionality
- ✅ Real-time WebSocket Secure (WSS) connection
- ✅ AES-256-CBC encryption for message security (hex-encoded format)
- ✅ Automatic WiFi and WebSocket reconnection with exponential backoff (15s-60s)
- ✅ Configuration via SPIFFS JSON file
- ✅ Modular architecture (ConfigManager, SecurityManager, ResilienceManager)
- ✅ Connection health monitoring with server heartbeats
- ✅ Message queuing for reliable delivery during disconnections
- ✅ Production-ready with configurable logging levels

### Power Management (NEW)
- ✅ **Smart Deep Sleep** - Automatically sleeps on battery, stays awake on USB
- ✅ **USB Detection** - Detects when plugged in vs battery powered
- ✅ **Configurable Sleep Cycles** - Default 5 minutes, adjustable
- ✅ **Battery Life Extension** - From 20 hours to ~5 days on 2000mAh battery
- ✅ **Wake on Timer** - Periodically wakes to check for messages

### Battery Monitoring (NEW)
- ✅ **Visual Battery Indicator** - Shows battery level in top-right corner
- ✅ **Percentage-Based Display** - 3 circles represent charge level:
  - ●●● = 80-100% battery
  - ●●○ = 30-79% battery
  - ●○○ = 1-29% battery
  - ○○○ = Critical (<1%)
- ✅ **Charging Indicator** - Lightning bolt (⚡) shows when USB connected
- ✅ **Accurate Voltage Mapping** - Calibrated for LiPo batteries

## Prerequisites

1. **PlatformIO** - Install via VS Code extension or CLI
2. **USB Driver** - CH340/CH341 or CP2102 depending on your board
3. **LilyGo T5 Board** - One of the supported models

## Installation

### 1. Clone and Navigate

```bash
cd esp32_arduino_client
```

### 2. Configure Your Device

Edit `data/config.json` with your settings:

```json
{
  "device": {
    "id": "your_device_id",
    "name": "Your Display Name"
  },
  "wifi": {
    "ssid": "Your_WiFi_SSID",
    "password": "Your_WiFi_Password",
    "timeout_ms": 15000,
    "reconnect_interval_ms": 30000
  },
  "server": {
    "host": "your-server.com",
    "port": 443,
    "path": "/ws-stream",
    "use_ssl": true
  },
  "display": {
    "width": 212,
    "height": 104,
    "rotation": 1,
    "pins": {
      "cs": 5,
      "dc": 17,
      "rst": 16,
      "busy": 4,
      "sclk": 18,
      "mosi": 23
    }
  },
  "timing": {
    "heartbeat_interval_ms": 15000,
    "heartbeat_timeout_ms": 30000,
    "ws_initial_reconnect_ms": 15000,
    "ws_max_reconnect_ms": 60000
  },
  "power": {
    "sleep_enabled": true,
    "sleep_duration_min": 5,
    "battery_pin": 36,
    "usb_threshold_v": 4.2
  },
  "security": {
    "use_aes": true,
    "aes_key": "base64_encoded_32_byte_key"
  },
  "logging": {
    "default_level": "WARN",
    "enable_test_commands": false
  }
}
```

### 3. Build and Upload

Using PlatformIO CLI:

```bash
# Build firmware
pio run

# Upload firmware
pio run -t upload

# Upload filesystem (config.json)
pio run -t uploadfs

# Monitor serial output
pio device monitor -b 115200
```

Using VS Code with PlatformIO:

1. Open the project folder in VS Code
2. Select your environment from the PlatformIO toolbar
3. Click "Upload Filesystem Image" (→ icon)
4. Click "Upload" (→ icon)
5. Click "Serial Monitor" to view output

## Configuration

### Selecting Display Type

**IMPORTANT**: Match your environment to your actual display chip! Check the chip label on your e-paper screen.

**Tested Configurations:**
- `lilygo_t5_depg_bw` → **DEPG0213BN** (2-level BW) ✅
- `lilygo_t5_gdew_bw` → **GDEW0213T5D** (2-level BW only) ✅
- `lilygo_t5_gdew_4g` → **GDEW0213T5D** (4-level grayscale, **recommended**) ✅

**Untested (For Different Chips):**
- `lilygo_t5_gdem_4g` → **GDEM0213B74** (4-level, untested)
- `lilygo_t5_gdey_4g` → **GDEY0213B74** (4-level, untested)
- `lilygo_t5s_27` → **GDEY027T91** (2.7" BW, untested)

**⚠️ Common Mistake**: Do NOT use `gdem_4g` or `gdey_4g` for GDEW0213T5D displays - they use incompatible drivers!

Build and upload:
```bash
pio run -e lilygo_t5_depg_bw -t upload  # For your display type
```

### Display Rotation

- `0` - Portrait (0°)
- `1` - Landscape (90°) - **Default**
- `2` - Portrait inverted (180°)
- `3` - Landscape inverted (270°)

### Power Management Settings

Configure power management in the `power` section:

- `sleep_enabled` - Enable/disable deep sleep on battery (default: true)
- `sleep_duration_min` - Minutes to sleep between wake cycles (default: 5)
- `battery_pin` - GPIO pin for battery voltage reading (default: 36 for LilyGo T5)
- `usb_threshold_v` - Voltage threshold for USB detection (default: 4.2V)

**Power Behavior:**
- **USB Connected**: Always awake for instant reactions
- **Battery Mode**: Sleeps after 2 minutes, wakes every 5 minutes to check messages
- **Battery Life**: ~5 days on 2000mAh battery with sleep enabled

### Battery Indicator

The battery indicator appears in the top-right corner of all displays:

```
⚡●●● = USB charging, 80-100% battery
  ●●● = Battery mode, 80-100%
  ●●○ = Battery mode, 30-79%
  ●○○ = Battery mode, 1-29%
  ○○○ = Battery critical (<1%)
```

### Encryption & Security

This client uses AES-256-CBC encryption with a hex-encoded message format:

**Message Format:**
- Server sends: `"AES:" + (iv + ciphertext).hex()`
- IV: First 32 hex characters (16 bytes)
- Ciphertext: Remaining hex characters (variable length)

**Security Features:**
- End-to-end encryption with 256-bit AES keys
- Unique IV per message
- PKCS7 padding for block alignment
- Encrypted data as opaque blob (security best practice)
- Automatic key registration via WebSocket handshake

**Key Management:**
- AES key stored base64-encoded in config.json
- Key must be 32 bytes (256 bits)
- Generate with: `openssl rand -base64 32`
- Server and client must share the same key

### Getting Auth Token

1. Register your device with the server
2. Link your Slack account via OAuth
3. Retrieve the auth token from the server

## Project Structure

```
esp32_arduino_client/
├── src/
│   ├── main.cpp                     # Main application
│   ├── config/
│   │   ├── ConfigManager.h          # Configuration management
│   │   └── ConfigManager.cpp
│   ├── security/
│   │   ├── SecurityManager.h        # AES-256 encryption
│   │   └── SecurityManager.cpp
│   └── resilience/
│       ├── ResilienceManager.h      # Connection health & queuing
│       └── ResilienceManager.cpp
├── data/
│   └── config.json                  # Device configuration
├── docs/
│   ├── DISPLAY_SUPPORT.md          # Multi-display guide
│   ├── INSTALLATION_GUIDE.md       # Step-by-step installation
│   └── SECURITY_NOTES.md           # Encryption details
├── platformio.ini                   # Build configuration
└── README.md                        # This file
```

## Troubleshooting

### Display Not Working

1. Verify display dimensions (width/height) match your hardware in config.json
2. Check display connections (SPI pins)
3. Try different rotation values
4. Ensure you selected the correct PlatformIO environment for your display
5. Check serial monitor for display driver initialization messages

### WiFi Connection Issues

1. Check SSID and password in config.json
2. Ensure 2.4GHz network (ESP32 doesn't support 5GHz)
3. Verify router allows IoT devices
4. Check serial monitor for error messages

### WebSocket Connection Failed

1. Verify server URL includes `wss://` protocol
2. Check auth token is valid
3. Ensure device ID is registered on server
4. Monitor serial output for authentication errors

### Upload Errors

1. Hold BOOT button while uploading starts
2. Try lower upload speed in platformio.ini
3. Check USB cable supports data (not charge-only)
4. Install correct USB drivers for your board

## Serial Monitor Output

Normal startup sequence (production mode with WARN log level):

```
========================================
ESP32 Slack Reactions Client v2.0
========================================
[INFO][POWER] Wake from deep sleep | boot=5 reason=timer
[WARNING LEVEL ONLY - Errors and warnings will be shown]
```

With INFO log level enabled:

```
[INFO][POWER] Power on reset | boot=0 reason=power_on
[INFO][SYSTEM] Boot | version=2.0
[INFO][CONFIG] Configuration loaded successfully
[INFO][DISPLAY] Initialized | driver=GxEPD2_213_B74 width=250 height=122
[INFO][SYSTEM] AES-256 encryption enabled
[INFO][RESILIENCE] Initialized | interval=30000 timeout=60000
[INFO][WIFI] Connected | ip=192.168.1.100 rssi=-50
[INFO][WS] Connected | url=/ws-stream?rpi_id=your_device_id
[INFO][WS] Sent AES registration
[INFO][POWER] Battery check | voltage=3.85V threshold=4.2V
[INFO][POWER] Battery mode sleep triggered | reason=max_runtime runtime=120s
[INFO][POWER] Entering deep sleep | minutes=5
[INFO][SYSTEM] Setup complete
```

## Development

### Adding New Display Support

1. Create new driver in `src/display/drivers/`
2. Inherit from `DisplayBase`
3. Implement all virtual methods
4. Add to `DisplayManager::createDisplay()`
5. Add environment to `platformio.ini`

### Testing

Monitor serial output:
```bash
pio device monitor -b 115200 -f esp32_exception_decoder
```

## Graceful Shutdown

The ESP32 client includes a graceful shutdown handler that properly cleans up resources before deep sleep or restart:

**Features:**
- Cleanly disconnects WebSocket connection
- Properly closes WiFi connections
- Hibernates e-paper display to preserve last image
- Shows shutdown message on display (optional)
- Ensures all log messages are transmitted before shutdown

**Automatic Triggers:**
- Deep sleep on battery power
- Device restart
- Connection failures requiring reset

**Manual Testing (Debug Mode):**
```
TEST:SHUTDOWN           - Test graceful shutdown with message
TEST:SHUTDOWN:CLEAR     - Test graceful shutdown with clear display
```

**Implementation:**
The `gracefulShutdown()` function is called automatically before deep sleep and can be manually triggered for testing. It ensures proper resource cleanup and prevents connection leaks or display artifacts.

## Lock/Unlock Icons

The client displays lock icons in the top-left corner to indicate encryption status:

**Features:**
- 🔒 **Locked icon**: Displayed when message is encrypted (AES-256)
- 🔓 **Unlocked icon**: Displayed when message is unencrypted
- **Memory efficient**: Uses XBM format (20x20 pixels, ~60 bytes in flash)
- **Professional design**: Converted from PNG assets, embedded as C arrays
- **Always visible**: Shows encryption status on every reaction display

**Testing:**
```
TEST:LOCK:ICONS - Display lock/unlock icon samples
```

**Implementation:**
Icons are stored in flash memory using XBM (X BitMap) format with PROGMEM to minimize RAM usage. The icons are rendered using GxEPD2's `drawBitmap()` function.

## Emoji Rendering

The client now supports **graphical emoji rendering** with memory-efficient PNG decoding:

**Features:**
- 🖼️ **Graphical emojis**: Downloads and renders PNG emojis from Slack CDN
- 📉 **Memory efficient**: Only ~8KB peak RAM usage during render
- 🧹 **Aggressive cleanup**: All buffers freed immediately after render
- 📊 **Heap monitoring**: Automatic leak detection and logging
- 🔄 **Auto-fallback**: Uses text emoji (`:thumbsup:`) on failure or low memory
- ⏱️ **Timeout protection**: 5s connect + 10s total timeout
- 🛡️ **Safety checks**: Won't render if <20KB heap available

**Memory Lifecycle:**
```
1. Baseline:      ~70KB used
2. Download:      ~75KB used (+5KB HTTP buffer)
3. Decode:        ~77KB used (+2KB PNG decoder)
4. Render:        Display updated
5. Cleanup:       ~70KB used (back to baseline!)
```

**Implementation:**
Uses the lightweight `PNGdec` library by bitbank2, which decodes PNG line-by-line directly to the display buffer. This avoids storing the entire decoded image in RAM.

### Rendering Flow

**High-Level Pipeline:**
```
WebSocket Message → JSON Parse → showReaction() → Emoji Download → PNG Decode → Pixel Render → Display Update
```

**Detailed Step-by-Step:**

1. **Message Arrives**
   - WebSocket delivers JSON with `emoji_url` field
   - JSON parsed to extract: `emoji`, `emoji_url`, `user`, `channel`, `message`, `timestamp`, `isEncrypted`

2. **Display Transaction Begins**
   - `display->firstPage()` starts e-paper page loop
   - Local flags initialized: `emojiDownloaded = false`, `emojiRendered = false`

3. **Emoji Download Attempt** (First page iteration only)
   ```cpp
   if (emoji_url[0] != '\0' && !emojiDownloaded) {
       emojiRendered = EmojiRenderer::renderEmojiFromURL(emoji_url, x, y);
       emojiDownloaded = true;  // Prevents retry on subsequent pages
   }
   ```

4. **Render Entry Point**
   - **Validation**: Check URL not NULL/empty, heap ≥20KB free
   - **Download** (`downloadEmoji()`):
     ```
     HTTP GET → Read content-length → Allocate buffer → Stream bytes → Verify complete
     - Connect timeout: 5 seconds
     - Total timeout: 10 seconds
     - Max size: 50KB
     ```
   - **Decode** (PNGdec):
     ```cpp
     png.openRAM(buffer, size, pngDraw);  // Setup with callback
     png.decode(NULL, 0);                  // Triggers line-by-line callbacks
     png.close();                          // Cleanup decoder
     ```

5. **Line-by-Line Callback** (`pngDraw()`)
   - Called once per horizontal line of the image
   - `pDraw->pPixels` contains **only the current line** (not full image)
   - For each pixel in the line:
     ```cpp
     Extract RGB → Convert to grayscale → Threshold to black/white → drawPixel()
     ```
   - Supports 5 PNG formats: Grayscale, RGB, Palette, Grayscale+Alpha, RGBA
   - Boundary checking prevents drawing outside display area

6. **Cleanup**
   ```cpp
   free(downloadBuffer);           // Free immediately after decode
   downloadBuffer = nullptr;       // Prevent double-free
   verify_no_memory_leak();        // Log warning if heap didn't return to baseline
   ```

7. **Text Fallback** (if render failed)
   ```cpp
   if (!emojiRendered) {
       display->print(":thumbsup:");  // Shows text version
   }
   ```

8. **Complete Display**
   - Lock/unlock icon drawn (top-left)
   - Battery indicator drawn (top-right)
   - User, channel, message text rendered
   - Timestamp displayed (bottom)
   - `display->nextPage()` completes transaction, e-paper refreshes

**Memory Flow:**
```
Start:     Heap = ~150KB
           ↓
Download:  malloc(~5KB) → Heap = 145KB
           ↓
Decode:    PNGdec temp (~2KB) → Heap = 143KB
           ↓
Render:    drawPixel() (no new allocation)
           ↓
Cleanup:   free() → Heap = ~150KB ✓
```

**Key Design Principles:**
- **Line-by-line decoding**: Never loads full image into RAM
- **One-shot download**: Flags prevent re-downloading in page loop
- **Immediate cleanup**: Buffer freed right after decode, not deferred
- **Graceful degradation**: Always falls back to text, never crashes
- **Boundary safety**: All pixel coordinates validated before drawing

**Fallback Behavior:**
- If heap < 20KB: Skip emoji, use text
- If download fails: Use text emoji
- If decode fails: Use text emoji
- Network timeout: Use text emoji

## Recent Updates

### v2.1.3 (September 2025)
- ✅ Added graphical emoji rendering with PNG download/decode
- ✅ Memory-efficient implementation (~8KB peak RAM usage)
- ✅ Aggressive garbage collection and leak detection
- ✅ Automatic fallback to text emoji on errors
- ✅ Comprehensive heap monitoring and logging

### v2.1.2 (September 2025)
- ✅ Added lock/unlock icon rendering in top-left corner
- ✅ Converted PNG assets to XBM format for minimal memory usage
- ✅ Icons show encryption status on all reaction displays
- ✅ Added TEST:LOCK:ICONS debug command

### v2.1.1 (September 2025)
- ✅ Added graceful shutdown handler for proper resource cleanup
- ✅ Improved deep sleep flow with shutdown integration
- ✅ Added debug commands for testing shutdown behavior

### v2.1.0 (September 2025)
- ✅ Fixed AES encryption format (now uses hex encoding matching server)
- ✅ Fixed battery detection (properly detects USB vs no battery via voltage thresholds)
- ✅ Fixed WebSocket reconnection timing (increased from 5s to 15s initial delay)
- ✅ Fixed field name mismatch (now uses `user`, `channel`, `message` matching server)
- ✅ Added WiFi stabilization delay (500ms before connection attempt)
- ✅ Added comprehensive battery detection with variance checking
- ✅ Improved logging with detailed power management messages

### Known Issues
- WiFi association may fail on first attempt (automatically retries)
- E-paper displays require proper hibernation timing for sleep mode

## License

See main project LICENSE file.

## Support

- Check serial monitor for debug output
- Review config.json for correct settings
- Ensure using latest firmware version
- Refer to LilyGo documentation for hardware specs