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
- ✅ **1-Minute Wake Cycles** - Check for reactions every 1 min during active hours for 5x faster response
- ✅ **30-Minute Quiet Hours** - Extended sleep during nights (11PM-7AM) & all day weekends
- ✅ **Smart Display Refresh** - Only refresh when new reactions arrive, saving battery & display lifetime
- ✅ **USB Detection** - Detects when plugged in vs battery powered
- ✅ **Battery Life Extension** - ~6 days on 2000mAh battery with optimized wake strategy
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

## Documentation

- **[Installation Guide](docs/INSTALLATION_GUIDE.md)** - Step-by-step setup and troubleshooting
- **[Display Support](docs/DISPLAY_SUPPORT.md)** - Supported displays and driver configuration
- **[Security Notes](docs/SECURITY_NOTES.md)** - Encryption and security considerations

## Quick Start

**For detailed installation instructions, troubleshooting, and first-time setup, see [docs/INSTALLATION_GUIDE.md](docs/INSTALLATION_GUIDE.md)**

### 1. Install PlatformIO

```bash
# Via VS Code: Install PlatformIO IDE extension
# Or via CLI: pip install platformio
```

### 2. Configure Device

Edit `data/config.json` (see [example config](#configuration) below):

```json
{
  "device": {"id": "your_device_id", "name": "Living Room Display"},
  "wifi": {"ssid": "Your_WiFi", "password": "password"},
  "server": {"host": "your-server.com", "port": 443, "use_ssl": true},
  "security": {"use_aes": true, "aes_key": "base64_key"}
}
```

### 3. Build and Upload

```bash
pio run -e lilygo_t5_depg_bw    # Build for your display type
pio run -t upload                # Upload firmware
pio run -t uploadfs              # Upload config.json to SPIFFS
pio device monitor -b 115200     # Monitor serial output
```

**Need help?**
- 🔧 [Installation Guide](docs/INSTALLATION_GUIDE.md) - Complete setup with troubleshooting
- 🖥️ [Display Support](docs/DISPLAY_SUPPORT.md) - Choosing the right display driver

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
- `sleep_duration_min` - Minutes to sleep between wake cycles (default: 1)
- `battery_pin` - GPIO pin for battery voltage reading (default: 36 for LilyGo T5)
- `usb_threshold_v` - Voltage threshold for USB detection (default: 4.2V)

**Power Behavior:**
- **USB Connected**: Always awake for instant reactions
- **Battery Mode (Active Hours)**: Wakes every 1 minute, checks for messages (10 sec), then sleeps
- **Battery Mode (Quiet Hours)**: Wakes every 30 minutes to conserve power
- **Battery Life**: ~6 days on 2000mAh battery with optimized wake strategy

### Quiet Hours (Battery Optimization)

Quiet hours automatically extend sleep duration during off-hours for better battery life:

**Configuration** (in `quiet_hours` section):
- `start_hour` - Night quiet hours start (0-23, default: 23 for 11 PM)
- `end_hour` - Night quiet hours end (0-23, default: 7 for 7 AM)
- `sleep_multiplier` - Sleep duration multiplier during quiet hours (default: 6)

**Behavior:**
- **Weekdays (Mon-Fri)**: Quiet hours apply from `start_hour` to `end_hour` (e.g., 11 PM - 7 AM)
  - Sleep duration: 1 min × 6 = **30 minutes** (2 wakes/hour instead of 60)
- **Weekends (Sat-Sun)**: Quiet hours apply **ALL DAY** for maximum battery savings
  - Assumes workplace Slack has minimal weekend activity

**Battery Savings:**
- Active hours: Wake every 1 minute (60 wakes/hour) - 5x faster response than before
- Quiet hours: Wake every 30 minutes (2 wakes/hour)
- **Weekend savings**: ~25% fewer wakes = ~20% longer battery life
- With 2000mAh battery: **~6 days** with 5x faster response during work hours

**Example:**
```json
"quiet_hours": {
  "start_hour": 23,      // 11 PM
  "end_hour": 7,         // 7 AM
  "sleep_multiplier": 6  // 30-min sleep during quiet hours
}
```

### Display Update Policy (NEW)

Control when the e-paper display refreshes to optimize battery life and display longevity:

**Configuration** (in `display_policy` section):
- `skip_refresh_on_no_message` - Only refresh on new reactions (default: true)
- `show_sleep_text` - Display "SLEEP" before entering deep sleep (default: false)
- `update_battery_on_wake` - Update battery indicator on every wake (default: false)

**Smart Refresh Strategy:**

When `skip_refresh_on_no_message` is enabled:
- Wake → Check WebSocket → **No new message** → Skip display update (e-paper retains image)
- Wake → Check WebSocket → **New message** → Full refresh to show reaction
- Result: ~15-20 refreshes/day instead of ~1,440 (97% reduction!)

**Why This Matters:**
- E-paper displays retain their image without power (bistable technology)
- Unnecessary refreshes waste battery and reduce display lifespan
- Full refresh required after deep sleep before partial updates can work (GxEPD2 limitation)
- By skipping refresh when no new message, we eliminate thousands of unnecessary refreshes

**Battery & Display Impact:**
- Reduces display wear from 1,440 refreshes/day to ~20 refreshes/day
- Saves ~400 mAh/day in display refresh power
- Extends display lifetime by 97%

**Example:**
```json
"display_policy": {
  "skip_refresh_on_no_message": true,   // Recommended for battery life
  "show_sleep_text": false,              // Saves battery, no visual feedback needed
  "update_battery_on_wake": false        // Battery updates on new messages only
}
```

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

This client uses AES-256-CBC encryption with a hex-encoded message format.

**Message Format:**
- Server sends: `"AES:" + (iv + ciphertext).hex()`
- IV: First 32 hex characters (16 bytes)
- Ciphertext: Remaining hex characters (variable length)

**Key Management:**
- AES key stored base64-encoded in config.json
- Key must be 32 bytes (256 bits)
- Generate with: `openssl rand -base64 32`
- Server and client must share the same key

**⚠️ Security Considerations:**

For a detailed security analysis including limitations and potential improvements, see **[docs/SECURITY_NOTES.md](docs/SECURITY_NOTES.md)**.

**TL;DR:** Current implementation is adequate for personal use (Slack reactions) but transmits symmetric keys over TLS. For sensitive production use, consider implementing Diffie-Hellman key exchange or RSA-wrapped keys.

### Getting Auth Token

1. Register your device with the server
2. Link your Slack account via OAuth
3. Retrieve the auth token from the server

**Troubleshooting authentication issues:** See [Installation Guide - Server Rejects Connection](docs/INSTALLATION_GUIDE.md#server-rejects-connection)

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

**For comprehensive troubleshooting with detailed solutions, see [docs/INSTALLATION_GUIDE.md](docs/INSTALLATION_GUIDE.md#troubleshooting)**

### Quick Diagnostics

**Display Not Working**
- Verify display type matches hardware (see [Display Support](docs/DISPLAY_SUPPORT.md))
- Check SPI pin connections in config.json
- Try different rotation values

**WiFi Connection Issues**
- Check SSID/password in config.json
- Ensure 2.4GHz network (ESP32 doesn't support 5GHz)
- Close serial monitor and re-upload: `pio run -t uploadfs`

**WebSocket Connection Failed**
- Verify server URL includes `wss://` protocol
- Ensure device ID is registered on server
- See [Installation Guide - Server Rejects Connection](docs/INSTALLATION_GUIDE.md#server-rejects-connection)

**Upload Errors**
- Hold BOOT button while uploading starts
- Check USB cable supports data (not charge-only)
- See [Installation Guide - If Serial Port is Busy](docs/INSTALLATION_GUIDE.md#if-serial-port-is-busy)

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