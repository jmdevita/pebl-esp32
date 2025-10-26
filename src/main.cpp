#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <qrcode.h>

// Conditional display library includes based on build flags
#ifdef DISPLAY_4G_GRAYSCALE
    // 4-level grayscale library
    #include <GxEPD2_4G_4G.h>
    #include <gdey/GxEPD2_213_GDEY0213B74.h>  // 2.13" 4G (GDEY0213B74)
    #include <epd/GxEPD2_213_flex.h>          // 2.13" 4G (GDEW0213I5F - flexible)
#else
    // 2-level black & white library
    #include <GxEPD2_BW.h>
    // Include headers for all supported BW displays
    #include <epd/GxEPD2_213_B74.h>   // 2.13" BW (DEPG0213BN)
    #include <epd/GxEPD2_213_T5D.h>   // 2.13" BW (GDEW0213T5D)
    #include <epd/GxEPD2_270.h>       // 2.7" BW (GDEY027T91)
#endif

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansOblique9pt7b.h>
#include "config/ConfigManager.h"
#include "security/SecurityManager.h"
#include "resilience/ResilienceManager.h"
#include <mbedtls/base64.h>
#include <esp_sleep.h>
#include <driver/adc.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <SPIFFS.h>
#include <time.h>  // For time_t, struct tm, gmtime()
#include "ota/OTAManager.h"

// ============================================================================
// Power Management
// ============================================================================
RTC_DATA_ATTR int bootCount = 0;  // Persists across deep sleep
RTC_DATA_ATTR unsigned long lastMessageTime = 0;

// ============================================================================
// Timezone Management (RTC memory persists across sleep)
// ============================================================================
RTC_DATA_ATTR uint16_t wakesSinceTimeSync = 0;      // Wake cycles since last timezone sync
RTC_DATA_ATTR time_t lastTimeSyncTimestamp = 0;     // Unix timestamp of last sync
RTC_DATA_ATTR int timezoneOffsetSeconds = 0;        // Timezone offset in seconds (includes DST)
RTC_DATA_ATTR time_t currentTime = 0;               // Estimated current time (updated each wake)
RTC_DATA_ATTR bool hasEverSynced = false;           // True if we've successfully synced at least once

// ============================================================================
// Debug Configuration
// ============================================================================
// #define ENABLE_DEBUG_FEATURES  // Uncomment for debugging/testing

// ============================================================================
// Log Levels and System
// ============================================================================
enum LogLevel {
    LOG_ERROR = 0,
    LOG_WARN = 1,
    LOG_INFO = 2,
    LOG_DEBUG = 3,
    LOG_TEST = 4
};

LogLevel currentLogLevel = LOG_WARN;  // Production: WARN, Debug: INFO/DEBUG

// Structured logging with AI-friendly format
void logMessage(LogLevel level, const char* module, const char* message, const char* kvPairs = nullptr) {
    if (level > currentLogLevel) return;

    const char* levelStr[] = {"ERROR", "WARN", "INFO", "DEBUG", "TEST"};

    Serial.print("[");
    Serial.print(millis());
    Serial.print("][");
    Serial.print(levelStr[level]);
    Serial.print("][");
    Serial.print(module);
    Serial.print("] ");
    Serial.print(message);

    if (kvPairs) {
        Serial.print(" | ");
        Serial.print(kvPairs);
    }

    Serial.println();
}

// Overload for ConfigManager compatibility
void logMessage(int level, const char* module, const char* message, const char* kvPairs = nullptr) {
    logMessage(static_cast<LogLevel>(level), module, message, kvPairs);
}

// ============================================================================
// Configuration - Now loaded from SPIFFS via ConfigManager
// ============================================================================
// Display text limits (keeping as constexpr since they're compile-time UI constants)
namespace DisplayLimits {
    constexpr size_t MAX_USER_LENGTH = 15;
    constexpr size_t MAX_CHANNEL_LENGTH = 15;
    constexpr size_t MAX_MESSAGE_LENGTH = 30;

    // Emoji position: leaves space for lock icon (top-left) and battery indicator (top-right)
    constexpr int16_t EMOJI_X = 10;  // 10px from left
    constexpr int16_t EMOJI_Y = 38;  // 38px from top (moved down for spacing)
}

// ============================================================================
// Battery Management Constants
// ============================================================================
namespace BatteryConstants {
    // No-battery detection thresholds
    constexpr int NO_BATTERY_VARIANCE_THRESHOLD = 500;     // Max ADC variance for valid battery
    constexpr int NO_BATTERY_MIN_ADC = 100;                // Min ADC reading (below = disconnected)
    constexpr int NO_BATTERY_MAX_ADC = 4000;               // Max ADC reading (above = floating)
    constexpr float NO_BATTERY_MIN_VOLTAGE = 2.5f;         // Min voltage for valid LiPo
    constexpr float NO_BATTERY_MAX_VOLTAGE = 5.5f;         // Max voltage (allows for USB 5V)

    // USB detection thresholds (MakerFocus 3.7V 2000mAh LiPo with TP4054 charger)
    // Hysteresis prevents oscillation: different thresholds for each direction
    constexpr float USB_HIGH_VOLTAGE_THRESHOLD = 4.05f;    // Battery→USB: Detect while charging (87%+)
    constexpr float USB_TO_BATTERY_THRESHOLD = 4.15f;      // USB→Battery: Require drop to 4.15V
    constexpr float BATTERY_LOW_VOLTAGE_THRESHOLD = 3.85f; // Deprecated - using hysteresis instead
    constexpr float VOLTAGE_STABILITY_THRESHOLD = 0.002f;  // USB has very low variance (<0.002V²)

    // LiPo voltage range (MakerFocus specs + Adafruit data)
    constexpr float LIPO_MAX_VOLTAGE = 4.2f;               // Fully charged
    constexpr float LIPO_NOMINAL_VOLTAGE = 3.7f;           // Nominal (~50% capacity)
    constexpr float LIPO_DEAD_VOLTAGE = 3.4f;              // "Dead" battery per Adafruit
    constexpr float LIPO_CUTOFF_VOLTAGE = 3.0f;            // Protection board cutoff

    // Sleep thresholds
    constexpr int LOW_BATTERY_SLEEP_THRESHOLD = 15;        // Sleep immediately below 15% (balanced runtime vs longevity)
    constexpr unsigned long GRACE_PERIOD_MS = 60000;       // 60s grace after switching to battery
    constexpr unsigned long MAX_BATTERY_RUNTIME_MS = 20000; // 20sec max runtime on battery (allows emoji downloads + WiFi variance)

    // ADC configuration
    constexpr int ADC_SAMPLE_COUNT = 10;                   // Samples for averaging
    constexpr int ADC_SAMPLE_DELAY_MS = 5;                 // Delay between samples
    constexpr float ADC_VOLTAGE_DIVIDER = 2.0f;            // 2:1 voltage divider
    constexpr float ADC_MAX_VOLTAGE = 3.6f;                // ESP32 ADC max voltage
    constexpr int ADC_RESOLUTION = 4095;                   // 12-bit ADC (0-4095)
}

// ============================================================================
// Global Objects - Using proper initialization
// ============================================================================
namespace {  // Anonymous namespace for internal linkage
    // Display instance - will be initialized after config is loaded
    // Type determined by DISPLAY_WIDTH, DISPLAY_HEIGHT, and DISPLAY_4G_GRAYSCALE build flags
    //
    // Display Type Selection Matrix:
    // - 212px + DISPLAY_4G_GRAYSCALE + DISPLAY_GDEW0213I5F -> 2.13" flexible 4-level (GxEPD2_213_flex)
    // - 212px + DISPLAY_4G_GRAYSCALE                       -> 2.13" 4-level (GxEPD2_213_GDEY0213B74)
    // - 264px                                              -> 2.7" BW (GxEPD2_270)
    // - 212px + DISPLAY_GDEW0213T5D                        -> 2.13" BW UC8151D (GxEPD2_213_T5D)
    // - 212px (default)                                    -> 2.13" BW (GxEPD2_213_B74)
    //
    #ifdef DISPLAY_4G_GRAYSCALE
        #if defined(DISPLAY_WIDTH) && DISPLAY_WIDTH == 212
            #ifdef DISPLAY_GDEW0213I5F
                GxEPD2_4G_4G<GxEPD2_213_flex, GxEPD2_213_flex::HEIGHT>* display = nullptr;
            #else
                GxEPD2_4G_4G<GxEPD2_213_GDEY0213B74, GxEPD2_213_GDEY0213B74::HEIGHT>* display = nullptr;
            #endif
        #endif
    #else
        #if defined(DISPLAY_WIDTH) && DISPLAY_WIDTH == 264
            GxEPD2_BW<GxEPD2_270, GxEPD2_270::HEIGHT>* display = nullptr;
        #elif defined(DISPLAY_WIDTH) && DISPLAY_WIDTH == 212
            #ifdef DISPLAY_GDEW0213T5D
                GxEPD2_BW<GxEPD2_213_T5D, GxEPD2_213_T5D::HEIGHT>* display = nullptr;
            #else
                GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT>* display = nullptr;
            #endif
        #else
            // Default to 2.13" if dimensions not specified
            GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT>* display = nullptr;
        #endif
    #endif

    // WebSocket client
    WebSocketsClient webSocket;

    // Connection state - volatile for ISR safety
    volatile bool wsConnected = false;

    // Timing variables
    uint32_t lastHeartbeat = 0;
    uint32_t lastReconnect = 0;
    uint32_t reconnectDelay = 5000; // Will be updated from config
    uint32_t lastWiFiReconnect = 0;

    // Connection metrics
    struct ConnectionMetrics {
        uint32_t startTime = 0;
        uint32_t totalConnections = 0;
        uint32_t failedConnections = 0;
        uint32_t messagesReceived = 0;
        uint32_t heartbeatsReceived = 0;
    } metrics;

    #ifdef ENABLE_DEBUG_FEATURES
    // Serial command buffer
    String commandBuffer = "";
    #endif
}

// ============================================================================
// RTC Memory - Survives Deep Sleep
// ============================================================================
// Store last reaction data in RTC memory so we can redraw it after deep sleep
RTC_DATA_ATTR struct {
    bool hasReaction = false;
    char emoji[32] = "";
    char emojiUrl[128] = "";
    char user[64] = "";
    char channel[64] = "";
    char message[128] = "";
    bool isEncrypted = false;
} lastReaction;

// Track what the display is showing (survives deep sleep)
RTC_DATA_ATTR bool displayShowingBattery = false;

// Track if device woke from sleep on battery (used for Bug #2 fix - different timeouts)
RTC_DATA_ATTR bool wokeOnBattery = false;

// Power management state (survives deep sleep)
RTC_DATA_ATTR unsigned long rtcBatteryModeStartTime = 0;
RTC_DATA_ATTR bool rtcWasPreviouslyOnBattery = false;

// WiFi adaptive power management state (survives deep sleep)
RTC_DATA_ATTR struct {
    wifi_power_t currentPower;       // Current TX power level
    uint8_t consecutiveFailures;     // Consecutive failures at current level
    uint8_t totalFailedWakes;        // Total failed wake cycles
    bool wifiDisabledMode;           // True if in low-power fallback
    uint8_t fallbackWakeCount;       // Wakes since entering fallback
} wifiPowerState = {
    WIFI_POWER_11dBm,  // Start at LOW for battery savings
    0,
    0,
    false,
    0
};

// ============================================================================
// Display State Tracking
// ============================================================================
// Track whether a full refresh has happened since wake (reset on each wake)
bool fullRefreshSinceWake = false;

// ============================================================================
// OTA Update State
// ============================================================================
OTAManager* otaManager = nullptr;  // Initialized in setup() after config loaded
bool pendingUpdate = false;
String pendingVersion = "";
unsigned long lastReactionTime = 0;  // Track when last reaction displayed
unsigned long lastOTACheckMillis = 0;  // millis() when last OTA check performed (NOT RTC - resets on boot)

// Forward declarations
void handleWebSocketMessage(const uint8_t* payload, size_t length);
void gracefulShutdown(const char* reason, bool clearDisplay);
float getBatteryVoltage();
int getBatteryPercentage();
int getBatteryLevel();
bool isUSBPowered();
void injectTestMessage(const String& jsonPayload);
void checkForFirmwareUpdate();
void performOTAUpdate();
bool isDeviceIdle();

// ============================================================================
// Display State Helper
// ============================================================================
// Helper function to update display state after any full refresh
void updateDisplayStateAfterFullRefresh() {
    fullRefreshSinceWake = true;
    displayShowingBattery = !isUSBPowered();

    char stateBuf[64];
    snprintf(stateBuf, sizeof(stateBuf), "fullRefresh=true displayShowingBattery=%s",
             displayShowingBattery ? "true" : "false");
    logMessage(LOG_DEBUG, "DISPLAY", "Display state updated after full refresh", stateBuf);
}

// ============================================================================
// Emoji Renderer - Memory-efficient PNG emoji rendering
// ============================================================================
class EmojiRenderer {
private:
    // Memory and size constraints
    static constexpr size_t MIN_HEAP_FOR_RENDER = 20000;  // 20KB minimum free heap
    static constexpr size_t MAX_EMOJI_SIZE = 128000;      // 128KB maximum (Slack's limit)
    static constexpr int LEAK_TOLERANCE_BYTES = 2500;     // HTTPS + heap fragmentation tolerance

    static PNG png;
    static uint8_t* downloadBuffer;
    static size_t downloadSize;
    static size_t downloadCapacity;
    static int16_t renderX, renderY;

    // PNG decoder callback - called once per line of decoded image
    // pDraw->pPixels contains ONE LINE of pixels (not the full image)
    static int pngDraw(PNGDRAW *pDraw) {
        // Safety checks
        if (!display || !pDraw || !pDraw->pPixels) return 1;

        // Determine bytes per pixel based on PNG type
        // Type 0=grayscale, 2=RGB, 3=palette, 4=grayscale+alpha, 6=RGBA
        int bytesPerPixel = 1;
        switch (pDraw->iPixelType) {
            case 0: bytesPerPixel = 1; break;  // Grayscale
            case 2: bytesPerPixel = 3; break;  // RGB
            case 3: bytesPerPixel = 1; break;  // Palette (index)
            case 4: bytesPerPixel = 2; break;  // Grayscale+Alpha
            case 6: bytesPerPixel = 4; break;  // RGBA
            default:
                char logBuf[64];
                snprintf(logBuf, sizeof(logBuf), "type=%d", pDraw->iPixelType);
                logMessage(LOG_ERROR, "EMOJI", "Unknown PNG type", logBuf);
                return 1;  // Don't process unknown formats
        }

        // Loop through pixels in this line only
        for (int x = 0; x < pDraw->iWidth; x++) {
            uint8_t r, g, b;
            uint8_t* pPixel = pDraw->pPixels + (x * bytesPerPixel);

            // Extract RGB based on PNG type
            if (pDraw->iPixelType == 3 && pDraw->pPalette) {
                // Palette - look up RGB from palette
                uint8_t index = pPixel[0];
                uint8_t* pal = pDraw->pPalette + (index * 3);
                r = pal[0];
                g = pal[1];
                b = pal[2];
            } else if (pDraw->iPixelType == 0 || (pDraw->iPixelType == 3 && !pDraw->pPalette)) {
                // Grayscale
                r = g = b = pPixel[0];
            } else if (pDraw->iPixelType == 4) {
                // Grayscale+Alpha
                r = g = b = pPixel[0];
            } else if (pDraw->iPixelType == 6) {
                // RGBA
                r = pPixel[0];
                g = pPixel[1];
                b = pPixel[2];
            } else {
                // RGB or default
                r = pPixel[0];
                g = pPixel[1];
                b = pPixel[2];
            }

            // Convert to grayscale
            uint16_t gray = ((uint16_t)r + (uint16_t)g + (uint16_t)b) / 3;

            // Map grayscale to display color depth
            #ifdef DISPLAY_4G_GRAYSCALE
                // 4-level grayscale mapping (0-255 → 4 levels)
                uint16_t color;
                if (gray < 64) color = GxEPD_BLACK;         // 0-63: Black
                else if (gray < 128) color = GxEPD_DARKGREY;  // 64-127: Dark gray
                else if (gray < 192) color = GxEPD_LIGHTGREY; // 128-191: Light gray
                else color = GxEPD_WHITE;                     // 192-255: White
            #else
                // 2-level black & white threshold
                uint16_t color = (gray > 128) ? GxEPD_WHITE : GxEPD_BLACK;
            #endif

            // Draw pixel with boundary check
            int16_t px = renderX + x;
            int16_t py = renderY + pDraw->y;
            if (px >= 0 && px < display->width() && py >= 0 && py < display->height()) {
                display->drawPixel(px, py, color);
            }
        }
        return 1;  // Continue decoding
    }

public:
    static bool renderEmojiFromURL(const char* url, int16_t x, int16_t y) {
        // Validate URL
        if (!url || url[0] == '\0') {
            logMessage(LOG_ERROR, "EMOJI", "Invalid URL (NULL or empty)");
            return false;
        }

        // Safety check: ensure minimum free heap
        size_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < MIN_HEAP_FOR_RENDER) {
            char logBuf[64];
            snprintf(logBuf, sizeof(logBuf), "heap=%zu min=%zu", freeHeap, MIN_HEAP_FOR_RENDER);
            logMessage(LOG_WARN, "EMOJI", "Low memory, skipping emoji", logBuf);
            return false;
        }

        // Download PNG from URL
        if (!downloadEmoji(url)) {
            logMessage(LOG_ERROR, "EMOJI", "Failed to download emoji");
            cleanup();
            return false;
        }

        // Decode and render PNG
        renderX = x;
        renderY = y;

        int rc = png.openRAM(downloadBuffer, downloadSize, pngDraw);
        if (rc != PNG_SUCCESS) {
            char logBuf[32];
            snprintf(logBuf, sizeof(logBuf), "error=%d", rc);
            logMessage(LOG_ERROR, "EMOJI", "PNG open failed", logBuf);
            cleanup();
            return false;
        }

        rc = png.decode(NULL, 0);
        png.close();

        if (rc != PNG_SUCCESS) {
            char logBuf[32];
            snprintf(logBuf, sizeof(logBuf), "error=%d", rc);
            logMessage(LOG_ERROR, "EMOJI", "PNG decode failed", logBuf);
            cleanup();
            return false;
        }

        cleanup();
        logMessage(LOG_DEBUG, "EMOJI", "Render complete");
        return true;
    }

private:
    static bool downloadEmoji(const char* url) {
        HTTPClient http;
        http.setConnectTimeout(5000);  // 5 second timeout
        http.setTimeout(10000);         // 10 second total timeout
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        if (!http.begin(url)) {
            logMessage(LOG_ERROR, "EMOJI", "HTTP begin failed");
            return false;
        }

        int httpCode = http.GET();
        if (httpCode != HTTP_CODE_OK) {
            char logBuf[64];
            snprintf(logBuf, sizeof(logBuf), "http_code=%d", httpCode);
            logMessage(LOG_ERROR, "EMOJI", "HTTP GET failed", logBuf);
            http.end();
            return false;
        }

        int len = http.getSize();
        if (len <= 0 || len > MAX_EMOJI_SIZE) {
            char logBuf[64];
            snprintf(logBuf, sizeof(logBuf), "invalid_size=%d", len);
            logMessage(LOG_ERROR, "EMOJI", "Invalid content size", logBuf);
            http.end();
            return false;
        }

        // Allocate buffer for download
        downloadCapacity = len;
        downloadBuffer = (uint8_t*)malloc(downloadCapacity);
        if (!downloadBuffer) {
            logMessage(LOG_ERROR, "EMOJI", "Failed to allocate download buffer");
            http.end();
            return false;
        }

        // Read response into buffer with timeout protection
        WiFiClient* stream = http.getStreamPtr();
        if (!stream) {
            logMessage(LOG_ERROR, "EMOJI", "Failed to get stream pointer");
            free(downloadBuffer);
            downloadBuffer = nullptr;
            http.end();
            return false;
        }

        downloadSize = 0;
        unsigned long downloadStart = millis();

        while (http.connected() && downloadSize < downloadCapacity) {
            // Timeout check: 10 second maximum for download
            if ((unsigned long)(millis() - downloadStart) > 10000) {
                char logBuf[64];
                snprintf(logBuf, sizeof(logBuf), "downloaded=%zu/%zu bytes", downloadSize, downloadCapacity);
                logMessage(LOG_ERROR, "EMOJI", "Download timeout", logBuf);
                free(downloadBuffer);
                downloadBuffer = nullptr;
                http.end();
                return false;  // Timeout = failed download
            }

            size_t available = stream->available();
            if (available) {
                size_t toRead = min(available, downloadCapacity - downloadSize);
                size_t bytesRead = stream->readBytes(downloadBuffer + downloadSize, toRead);
                downloadSize += bytesRead;
            } else {
                delay(1);
            }
        }

        http.end();

        // Verify we got complete download
        if (downloadSize != downloadCapacity) {
            char logBuf[128];
            snprintf(logBuf, sizeof(logBuf), "expected=%zu got=%zu", downloadCapacity, downloadSize);
            logMessage(LOG_ERROR, "EMOJI", "Incomplete download", logBuf);
            free(downloadBuffer);
            downloadBuffer = nullptr;
            return false;  // Don't process incomplete data
        }

        return true;
    }

    static void cleanup() {
        if (downloadBuffer) {
            free(downloadBuffer);
            downloadBuffer = nullptr;
        }
        downloadSize = 0;
        downloadCapacity = 0;
    }
};

// Static member initialization
PNG EmojiRenderer::png;
uint8_t* EmojiRenderer::downloadBuffer = nullptr;
size_t EmojiRenderer::downloadSize = 0;
size_t EmojiRenderer::downloadCapacity = 0;
int16_t EmojiRenderer::renderX = 0;
int16_t EmojiRenderer::renderY = 0;

// ============================================================================
// Lock Icon Bitmaps (XBM format - 20x20 pixels)
// ============================================================================
namespace LockIcons {
    // Locked padlock icon (shackle rows 4-8, body rows 9-18)
    constexpr int locked_width = 20;
    constexpr int locked_height = 20;
    const unsigned char locked_bits[] PROGMEM = {
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,0x3f,0x00,
        0xe0,0x79,0x00,0x70,0xe0,0x00,0x70,0xe0,0x00,0x70,0xe0,0x00,0xf8,0xff,0x01,
        0xf8,0xff,0x01,0xf8,0xff,0x01,0xf8,0xff,0x01,0xf8,0xff,0x01,0xf8,0xff,0x01,
        0xf8,0xff,0x01,0xf8,0xff,0x01,0xf8,0xff,0x01,0xf8,0xff,0x01,0x00,0x00,0x00
    };

    // Unlocked padlock icon (shackle rows 4-8 open on right, body rows 9-18)
    constexpr int unlocked_width = 20;
    constexpr int unlocked_height = 20;
    const unsigned char unlocked_bits[] PROGMEM = {
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,0x03,0x00,
        0xe0,0xc1,0x01,0x70,0x80,0x03,0x70,0x00,0x07,0x70,0x00,0x0e,0xf8,0xff,0x01,
        0xf8,0xff,0x01,0xf8,0xff,0x01,0xf8,0xff,0x01,0xf8,0xff,0x01,0xf8,0xff,0x01,
        0xf8,0xff,0x01,0xf8,0xff,0x01,0xf8,0xff,0x01,0xf8,0xff,0x01,0x00,0x00,0x00
    };
}

// ============================================================================
// Display Helper Class - Modern C++ encapsulation
// ============================================================================
class DisplayManager {
public:
    // Draw lock icon (locked) in top-left corner
    static void drawLockIcon(int16_t x = 5, int16_t y = 5) {
        if (!display) return;
        display->drawXBitmap(x, y, LockIcons::locked_bits,
                            LockIcons::locked_width, LockIcons::locked_height,
                            GxEPD_BLACK);
    }

    // Draw unlock icon in top-left corner
    static void drawUnlockIcon(int16_t x = 5, int16_t y = 5) {
        if (!display) return;
        display->drawXBitmap(x, y, LockIcons::unlocked_bits,
                            LockIcons::unlocked_width, LockIcons::unlocked_height,
                            GxEPD_BLACK);
    }

    // Draw battery indicator in top-right corner
    static void drawPowerStatusIndicator() {
        // Show battery status text in top center when on battery power
        // Status varies by battery percentage: BATTERY / LOW BATT / CHARGE NOW

        // TODO: Optimize - currently calls getBatteryStatus() twice (isUSBPowered + getBatteryPercentage)
        // Should cache BatteryStatus and pass to both drawPowerStatusIndicator and drawBatteryIndicator

        if (!isUSBPowered()) {
            int batteryPercentage = getBatteryPercentage();

            // Determine status text based on battery percentage
            const char* statusText;
            if (batteryPercentage < 5) {
                statusText = "CHARGE NOW";  // Critical (<5%)
            } else if (batteryPercentage < 10) {
                statusText = "LOW BATT";    // Low (<10%)
            } else if (batteryPercentage < 20) {
                statusText = "LOW BATT";    // Warning (<20%)
            } else {
                statusText = "BATTERY";     // Normal (>=20%)
            }

            display->setFont(nullptr);  // Small font for status text
            int16_t centerX = display->width() / 2;

            // Calculate text width to center it
            int16_t x1, y1;
            uint16_t w, h;
            display->getTextBounds(statusText, 0, 0, &x1, &y1, &w, &h);

            display->setCursor(centerX - (w / 2), 8);  // Top center
            display->print(statusText);

            display->setFont(&FreeSans9pt7b);  // Restore font for caller
        }
        // If isUSBPowered() returns true (voltage >= usb_threshold_v from config), show nothing
        // Could be USB or just freshly unplugged battery - we can't tell
    }

    static void drawBatteryIndicator() {
        int batteryLevel = getBatteryLevel();

        // Don't show indicator if no battery detected
        if (batteryLevel < 0) return;

        // Position for top-right corner
        // Display is 250x122, so put circles at top-right
        const int16_t circleRadius = 4;
        const int16_t circleSpacing = 12;
        const int16_t xStart = display->width() - 50;  // 50 pixels from right edge
        const int16_t y = 10;  // 10 pixels from top

        // Always draw 3 circles, fill them based on battery level
        for (int i = 0; i < 3; i++) {
            int16_t x = xStart + (i * circleSpacing);

            if (i < batteryLevel) {
                // Filled circle for charged portion
                display->fillCircle(x, y, circleRadius, GxEPD_BLACK);
            } else {
                // Empty circle for discharged portion
                display->drawCircle(x, y, circleRadius, GxEPD_BLACK);
            }
        }

        // No charging indicator needed - "USB" text in top center shows charging status

        // Optional: Show percentage text below circles for debugging
        #ifdef ENABLE_DEBUG_FEATURES
        int percentage = getBatteryPercentage();
        if (percentage >= 0) {
            display->setFont(nullptr);  // Use default small font
            display->setCursor(xStart, y + 12);
            display->print(percentage);
            display->print("%");
            display->setFont(&FreeSans9pt7b);  // Switch back to normal font
        }
        #endif
    }

    static void showMessage(const String& line1,
                           const String& line2 = "",
                           const String& line3 = "",
                           const String& line4 = "",
                           bool showLockIcon = false) {
        char logBuf[128];
        snprintf(logBuf, sizeof(logBuf), "lines=%d text=\"%s\"",
                 (line1.isEmpty() ? 0 : 1) + (line2.isEmpty() ? 0 : 1) +
                 (line3.isEmpty() ? 0 : 1) + (line4.isEmpty() ? 0 : 1),
                 line1.c_str());
        logMessage(LOG_DEBUG, "DISPLAY", "Showing message", logBuf);

        if (!display) return; // Safety check

        display->setFullWindow();
        display->firstPage();
        do {
            display->fillScreen(GxEPD_WHITE);
            display->setTextColor(GxEPD_BLACK);
            display->setFont(&FreeSans9pt7b);

            // Draw battery indicator and power status in top-right
            drawBatteryIndicator();
            drawPowerStatusIndicator();  // Show "BATTERY" in top center when on battery

            // Draw lock icon if encryption is enabled
            if (showLockIcon) {
                drawLockIcon();
            }

            constexpr int16_t x = 10;
            constexpr int16_t yStart = 40;  // Moved down from 25 to 40
            constexpr int16_t ySpacing = 20;

            const String* lines[] = {&line1, &line2, &line3, &line4};

            for (size_t i = 0; i < 4; ++i) {
                if (!lines[i]->isEmpty()) {
                    display->setCursor(x, yStart + (i * ySpacing));
                    display->print(*lines[i]);
                }
            }
        } while (display->nextPage());

        // Update display state flags after full refresh
        updateDisplayStateAfterFullRefresh();
    }

    static void showReaction(const JsonObject& reaction) {
        // Extract data with safe defaults (matching server field names)
        const char* emoji = reaction["emoji"] | "?";
        const char* emoji_url = reaction["emoji_url"] | "";
        const char* user = reaction["user"] | "Unknown";
        const char* channel = reaction["channel"] | "Unknown";
        const char* message = reaction["message"] | "";
        const char* timestamp = reaction["timestamp"] | "";
        bool isEncrypted = reaction["encrypted"] | false;

        char logBuf[256];
        snprintf(logBuf, sizeof(logBuf), "emoji=%s user=%s channel=%s encrypted=%s has_url=%s",
                 emoji, user, channel, isEncrypted ? "true" : "false", emoji_url[0] ? "yes" : "no");
        logMessage(LOG_INFO, "DISPLAY", "Showing reaction", logBuf);

        // Save reaction data to RTC memory for redraw after deep sleep
        lastReaction.hasReaction = true;
        strncpy(lastReaction.emoji, emoji, sizeof(lastReaction.emoji) - 1);
        lastReaction.emoji[sizeof(lastReaction.emoji) - 1] = '\0';
        strncpy(lastReaction.emojiUrl, emoji_url, sizeof(lastReaction.emojiUrl) - 1);
        lastReaction.emojiUrl[sizeof(lastReaction.emojiUrl) - 1] = '\0';
        strncpy(lastReaction.user, user, sizeof(lastReaction.user) - 1);
        lastReaction.user[sizeof(lastReaction.user) - 1] = '\0';
        strncpy(lastReaction.channel, channel, sizeof(lastReaction.channel) - 1);
        lastReaction.channel[sizeof(lastReaction.channel) - 1] = '\0';
        strncpy(lastReaction.message, message, sizeof(lastReaction.message) - 1);
        lastReaction.message[sizeof(lastReaction.message) - 1] = '\0';
        lastReaction.isEncrypted = isEncrypted;
        logMessage(LOG_INFO, "RTC", "Saved reaction to RTC memory for deep sleep recovery");

        if (!display) return; // Safety check

        // Step 1: Start display transaction
        display->setFullWindow();
        display->firstPage();

        // Step 2: Render emoji ONCE (download happens only on first page iteration)
        // Emoji renders once on first page (memory constraints prevent caching for multi-page displays)
        bool emojiDownloaded = false;
        bool emojiRendered = false;

        do {
            // Clear display buffer
            display->fillScreen(GxEPD_WHITE);
            display->setTextColor(GxEPD_BLACK);

            // Draw battery indicator in top-right
            drawBatteryIndicator();
            drawPowerStatusIndicator();  // Show "BATTERY" in top center when on battery

            // Encryption indicator at top left corner using lock icon
            if (isEncrypted) {
                drawLockIcon();
            }

            // Render emoji (download only on first page iteration)
            if (emoji_url[0] != '\0' && !emojiDownloaded) {
                // Download and render emoji (only happens once per showReaction call)
                emojiRendered = EmojiRenderer::renderEmojiFromURL(emoji_url,
                                                                   DisplayLimits::EMOJI_X,
                                                                   DisplayLimits::EMOJI_Y);
                emojiDownloaded = true;  // Mark as downloaded for this reaction
            }

            // Text fallback for emoji if not rendered graphically
            if (!emojiRendered) {
                display->setFont(&FreeSans12pt7b);
                display->setCursor(10, 63);
                display->print(":");
                display->print(emoji);
                display->print(":");
            }

            // Reaction details - using smart truncation to prevent text wrapping
            // Calculate available width: display width - x position - right margin
            const int16_t rightMargin = 5;  // 5px margin from right edge

            // User name at top (bold font)
            display->setFont(&FreeSansBold9pt7b);
            int16_t userX = 60;
            int16_t userMaxWidth = display->width() - userX - rightMargin;
            String userStr = truncateToFit(user, &FreeSansBold9pt7b, userX, userMaxWidth);
            display->setCursor(userX, 48);
            display->print(userStr);

            // Message text below user name (regular font, with small whitespace)
            if (message[0] != '\0') {
                int16_t messageX = 60;
                int16_t messageMaxWidth = display->width() - messageX - rightMargin;
                String messageStr = truncateToFit(message, &FreeSans9pt7b, messageX, messageMaxWidth);
                display->setCursor(messageX, 68);  // Moved from 63 to 68 for small whitespace
                display->print(messageStr);
            }

            // "From: {channel}" at bottom right, italicized (with extra spacing)
            display->setFont(&FreeSansOblique9pt7b);
            String channelLabel = "From: " + String(channel);

            // Calculate text width to right-align
            int16_t x1, y1;
            uint16_t w, h;
            display->getTextBounds(channelLabel, 0, 0, &x1, &y1, &w, &h);

            int16_t channelX = display->width() - w - rightMargin;
            display->setCursor(channelX, 100);  // Moved down to 100 for extra whitespace
            display->print(channelLabel);

            // Timestamp at bottom left with smaller font
            display->setFont(nullptr);  // Default small font
            display->setCursor(10, display->height() - 5);  // Dynamic bottom position
            display->print(timestamp);

        } while (display->nextPage());

        // Update display state flags after full refresh
        updateDisplayStateAfterFullRefresh();
    }

    // Show WiFi provisioning mode with QR code
    static void showProvisioningMode(const String& ssid, const String& ip) {
        logMessage(LOG_INFO, "DISPLAY", "Showing provisioning mode");

        if (!display) return;  // Safety check

        // Generate WiFi QR code for iOS 18+/Android 10+ auto-connect
        String qrData = "WIFI:T:nopass;S:" + ssid + ";P:;;";

        // Initialize QR code (version 3 = 29x29 modules, good for WiFi strings)
        QRCode qrcode;
        uint8_t qrcodeData[qrcode_getBufferSize(3)];
        qrcode_initText(&qrcode, qrcodeData, 3, ECC_LOW, qrData.c_str());

        display->setFullWindow();
        display->firstPage();
        do {
            display->fillScreen(GxEPD_WHITE);
            display->setTextColor(GxEPD_BLACK);

            // Draw battery indicator and power status
            drawBatteryIndicator();
            drawPowerStatusIndicator();

            // Title
            display->setFont(&FreeSansBold9pt7b);
            display->setCursor(10, 28);
            display->print("WiFi Setup");

            // QR Code - positioned on the left side
            // Scale factor: 2 pixels per module for 29x29 = 58x58 total
            const uint8_t scale = 2;
            const uint8_t qrPixelSize = qrcode.size * scale;
            const int16_t qrX = 10;   // Left margin
            const int16_t qrY = 43;   // Below title

            // Draw each QR code module
            for (uint8_t y = 0; y < qrcode.size; y++) {
                for (uint8_t x = 0; x < qrcode.size; x++) {
                    uint16_t color = qrcode_getModule(&qrcode, x, y) ? GxEPD_BLACK : GxEPD_WHITE;
                    display->fillRect(qrX + (x * scale), qrY + (y * scale), scale, scale, color);
                }
            }

            // Instructions on the right side of QR code
            display->setFont(nullptr);  // Small default font
            const int16_t textX = qrX + qrPixelSize + 7;  // 7px gap from QR code
            int16_t textY = qrY;  // Align with top of QR code

            display->setCursor(textX, textY);
            display->print("Network:");

            textY += 10;
            display->setCursor(textX, textY);
            display->print(ssid.c_str());

            textY += 15;
            display->setCursor(textX, textY);
            display->print("Open browser:");

            textY += 10;
            display->setCursor(textX, textY);
            display->print(ip.c_str());

            textY += 15;
            display->setCursor(textX, textY);
            display->print("Scan QR or connect");

            textY += 10;
            display->setCursor(textX, textY);
            display->print("manually to configure");

        } while (display->nextPage());

        // Update display state flags after full refresh
        updateDisplayStateAfterFullRefresh();

        logMessage(LOG_INFO, "DISPLAY", "Provisioning mode displayed");
    }

private:
    // Smart truncation that accounts for actual rendered text width
    static String truncateToFit(const char* str, const GFXfont* font, int16_t x, int16_t maxWidth) {
        if (!display) return String(str);

        String result(str);
        if (result.isEmpty()) return result;

        // Set font to measure correctly
        display->setFont(font);

        // Check if text fits
        int16_t x1, y1;
        uint16_t w, h;
        display->getTextBounds(result.c_str(), x, 0, &x1, &y1, &w, &h);

        // If it fits, return as-is
        if (w <= maxWidth) {
            return result;
        }

        // Truncate character by character until it fits (with ellipsis)
        while (w > maxWidth && result.length() > 3) {
            result = result.substring(0, result.length() - 1);
            String withEllipsis = result + "...";
            display->getTextBounds(withEllipsis.c_str(), x, 0, &x1, &y1, &w, &h);

            if (w <= maxWidth) {
                return withEllipsis;
            }
        }

        // Fallback: just return "..." if even 3 chars don't fit
        return "...";
    }

    // Legacy truncation by character count (kept for backward compatibility)
    static String truncateString(const char* str, size_t maxLength) {
        String result(str);
        if (result.length() > maxLength) {
            result = result.substring(0, maxLength) + "...";
        }
        return result;
    }
};

// ============================================================================
// Power Management - Consolidated Battery Status
// ============================================================================
struct BatteryStatus {
    float voltage;      // Battery voltage (2.5-4.2V valid range, 5.0 = no battery)
    int percentage;     // 0-100%, -1 = no battery
    int level;          // 0-3 circles for display, -1 = no battery
    bool isUSBPowered;  // true if USB connected (voltage >= usb_threshold_v from config)
    bool hasBattery;    // true if valid battery detected
};

BatteryStatus getBatteryStatus() {
    using namespace BatteryConstants;
    BatteryStatus status = {5.0, -1, -1, false, false};

    // Read battery voltage via ADC (LilyGo T5 uses GPIO 35 or 36)
    adc1_config_width(ADC_WIDTH_BIT_12);

    // Test both pins, use whichever has higher reading
    adc1_config_channel_atten(ADC1_CHANNEL_7, ADC_ATTEN_DB_12);
    delay(10);
    int test_35 = adc1_get_raw(ADC1_CHANNEL_7);

    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_12);
    delay(10);
    int test_36 = adc1_get_raw(ADC1_CHANNEL_0);

    // Reject floating pins (reading >= 4000 = likely floating/invalid)
    // Pick the pin with valid battery range (100-3900)
    bool pin35_valid = (test_35 >= 100 && test_35 < 4000);
    bool pin36_valid = (test_36 >= 100 && test_36 < 4000);

    adc1_channel_t channel;
    if (pin35_valid && !pin36_valid) {
        channel = ADC1_CHANNEL_7;  // Only GPIO35 valid
    } else if (pin36_valid && !pin35_valid) {
        channel = ADC1_CHANNEL_0;  // Only GPIO36 valid
    } else {
        // Both valid or both invalid - pick higher reading
        channel = (test_35 > test_36) ? ADC1_CHANNEL_7 : ADC1_CHANNEL_0;
    }

    // Sample voltage multiple times for stability and USB detection
    // Store raw ADC values to reuse for both percentage and USB detection
    int rawSamples[ADC_SAMPLE_COUNT];
    float voltageSamples[ADC_SAMPLE_COUNT];
    int total = 0, min_val = ADC_RESOLUTION, max_val = 0;

    for (int i = 0; i < ADC_SAMPLE_COUNT; i++) {
        rawSamples[i] = adc1_get_raw(channel);
        voltageSamples[i] = (rawSamples[i] / float(ADC_RESOLUTION)) * ADC_MAX_VOLTAGE * ADC_VOLTAGE_DIVIDER;

        total += rawSamples[i];
        if (rawSamples[i] < min_val) min_val = rawSamples[i];
        if (rawSamples[i] > max_val) max_val = rawSamples[i];

        delay(ADC_SAMPLE_DELAY_MS);
    }

    int adc_avg = total / ADC_SAMPLE_COUNT;
    int variance = max_val - min_val;

    // Convert to voltage using averaged ADC reading
    status.voltage = (adc_avg / float(ADC_RESOLUTION)) * ADC_MAX_VOLTAGE * ADC_VOLTAGE_DIVIDER;

    // Detect no-battery conditions using constants
    if (variance >= NO_BATTERY_VARIANCE_THRESHOLD ||
        adc_avg < NO_BATTERY_MIN_ADC ||
        adc_avg > NO_BATTERY_MAX_ADC ||
        status.voltage < NO_BATTERY_MIN_VOLTAGE ||
        status.voltage > NO_BATTERY_MAX_VOLTAGE) {
        status.voltage = 5.0;  // Sentinel value for no battery
        logMessage(LOG_WARN, "POWER", "No battery detected", "");
        return status;
    }

    // Valid battery detected
    status.hasBattery = true;

    // Calculate percentage using accurate LiPo discharge curve
    // Based on MakerFocus 3.7V 2000mAh specs + Adafruit LiPo data
    if (status.voltage >= 4.2) status.percentage = 100;
    else if (status.voltage >= 4.1) status.percentage = 95;
    else if (status.voltage >= 4.0) status.percentage = 85;
    else if (status.voltage >= 3.9) status.percentage = 75;
    else if (status.voltage >= 3.8) status.percentage = 60;
    else if (status.voltage >= 3.7) status.percentage = 50;  // NOMINAL - half capacity
    else if (status.voltage >= 3.6) status.percentage = 35;
    else if (status.voltage >= 3.5) status.percentage = 20;
    else if (status.voltage >= 3.4) status.percentage = 10;  // "Dead" per Adafruit
    else if (status.voltage >= 3.2) status.percentage = 5;   // Near protection cutoff
    else status.percentage = 0;

    // Convert to display level (circles: 3=full, 2=medium, 1=low, 0=empty)
    // More balanced thresholds: 75%/50%/25% boundaries
    if (status.percentage >= 75) status.level = 3;      // 75-100%
    else if (status.percentage >= 50) status.level = 2; // 50-74%
    else if (status.percentage >= 25) status.level = 1; // 25-49%
    else status.level = 0;                              // 0-24%

    // Calculate average voltage and variance from existing samples (reuse!)
    float pattern_avg = 0;
    for (int i = 0; i < ADC_SAMPLE_COUNT; i++) {
        pattern_avg += voltageSamples[i];
    }
    pattern_avg /= ADC_SAMPLE_COUNT;

    float pattern_variance = 0;
    for (int i = 0; i < ADC_SAMPLE_COUNT; i++) {
        float diff = voltageSamples[i] - pattern_avg;
        pattern_variance += diff * diff;
    }
    pattern_variance /= ADC_SAMPLE_COUNT;

    // USB detection using voltage pattern analysis
    // TP4054 charging IC has no status pins - must detect via voltage behavior
    bool highVoltage = (pattern_avg >= USB_HIGH_VOLTAGE_THRESHOLD);
    bool stableVoltage = (pattern_variance < VOLTAGE_STABILITY_THRESHOLD);

    // Static state with hysteresis to prevent rapid toggling
    static bool lastUSBState = false;
    static unsigned long lastUSBCheckTime = 0;
    static unsigned long lastUSBLogTime = 0;
    unsigned long now = millis();

    // Only update USB state every 3 seconds (allows voltage to stabilize)
    if ((unsigned long)(now - lastUSBCheckTime) >= 3000) {
        lastUSBCheckTime = now;

        if (lastUSBState) {
            // Currently USB - switch to battery if voltage drops below threshold AND becomes unstable
            // In hysteresis zone (4.0-4.15V): stability determines state (USB=stable, Battery=unstable)
            bool inHysteresisZone = (pattern_avg >= USB_HIGH_VOLTAGE_THRESHOLD && pattern_avg < USB_TO_BATTERY_THRESHOLD);
            if (inHysteresisZone) {
                // In zone: stay USB if stable (charging), switch to battery if unstable (unplugged)
                lastUSBState = stableVoltage;
            } else {
                // Outside zone: simple voltage check
                lastUSBState = (pattern_avg >= USB_HIGH_VOLTAGE_THRESHOLD);
            }
        } else {
            // Currently battery - require BOTH high voltage AND stability for USB
            // Stability check works because getBatteryStatus() is called during WiFi activity
            // Battery voltage sags under WiFi load (unstable), USB stays regulated (stable)
            lastUSBState = (highVoltage && stableVoltage);
        }
    }


    status.isUSBPowered = lastUSBState;

    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "v=%.2f pct=%d%% lvl=%d usb=%s",
             status.voltage, status.percentage, status.level,
             status.isUSBPowered ? "yes" : "no");
    logMessage(LOG_DEBUG, "POWER", "Battery status", logBuf);

    return status;
}

// Legacy wrapper functions for backward compatibility
float getBatteryVoltage() { return getBatteryStatus().voltage; }
int getBatteryPercentage() { return getBatteryStatus().percentage; }
int getBatteryLevel() { return getBatteryStatus().level; }
bool isUSBPowered() { return getBatteryStatus().isUSBPowered; }

// ============================================================================
// Timezone Management
// ============================================================================

/**
 * Fetch timezone information from IPGeolocation.io API
 * Returns true if successful, false otherwise
 */
bool fetchTimezoneFromAPI() {
    const AppConfig& cfg = ConfigManager::getConfig();

    if (cfg.timezone.api_key.isEmpty()) {
        logMessage(LOG_WARN, "TIME", "No API key configured, skipping timezone sync");
        return false;
    }

    HTTPClient http;
    http.setTimeout(5000);  // 5 second timeout

    // Build URL with API key
    String url = cfg.timezone.api_url + "?apiKey=" + cfg.timezone.api_key;
    http.begin(url);

    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "url=%s", cfg.timezone.api_url.c_str());
    logMessage(LOG_INFO, "TIME", "Fetching timezone from API", logBuf);

    int httpCode = http.GET();

    if (httpCode == 200) {
        String response = http.getString();
        http.end();

        // Parse JSON response
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, response);

        if (error) {
            snprintf(logBuf, sizeof(logBuf), "error=%s", error.c_str());
            logMessage(LOG_ERROR, "TIME", "JSON parse error", logBuf);
            return false;
        }

        // Extract timezone data
        // IPGeolocation.io response format:
        // {
        //   "date_time_unix": 1761273385.454,
        //   "timezone": "America/New_York",
        //   "timezone_offset": -5,
        //   "timezone_offset_with_dst": -4,
        //   "is_dst": true
        // }

        currentTime = doc["date_time_unix"].as<time_t>();
        timezoneOffsetSeconds = doc["timezone_offset_with_dst"].as<int>() * 3600;  // Convert hours to seconds
        lastTimeSyncTimestamp = currentTime;
        wakesSinceTimeSync = 0;

        const char* tzName = doc["timezone"] | "Unknown";
        bool isDST = doc["is_dst"] | false;

        snprintf(logBuf, sizeof(logBuf),
                "tz=%s offset=%ds unix=%ld dst=%s",
                tzName,
                timezoneOffsetSeconds,
                currentTime,
                isDST ? "true" : "false");
        logMessage(LOG_INFO, "TIME", "Timezone synced successfully", logBuf);

        // Mark that we've successfully synced at least once
        hasEverSynced = true;

        return true;
    } else {
        snprintf(logBuf, sizeof(logBuf), "http_code=%d", httpCode);
        logMessage(LOG_WARN, "TIME", "API request failed", logBuf);
        http.end();
        return false;
    }
}

/**
 * Check if timezone sync is needed
 * Sync on power-on boot or based on interval (1 hour until first success, then 24 hours)
 */
bool shouldSyncTimezone(bool isPowerOnBoot) {
    const AppConfig& cfg = ConfigManager::getConfig();

    // Always sync on power-on boot
    if (isPowerOnBoot) {
        return true;
    }

    // Use shorter interval (1 hour) until we successfully sync for the first time
    // Then switch to normal interval (24 hours by default)
    uint16_t wakesPerSyncInterval;
    if (!hasEverSynced) {
        // Never synced - try every hour (12 wakes × 5min = 60min)
        wakesPerSyncInterval = 12;
    } else {
        // Previously synced - use configured interval (default 24 hours = 288 wakes)
        wakesPerSyncInterval = cfg.timezone.sync_interval_hours * 12;
    }

    return (wakesSinceTimeSync >= wakesPerSyncInterval);
}

/**
 * Update estimated current time based on sleep duration
 */
void updateEstimatedTime() {
    const AppConfig& cfg = ConfigManager::getConfig();
    currentTime += (cfg.power.sleep_duration_min * 60);
}

/**
 * Get current hour in local timezone (0-23)
 */
int getCurrentLocalHour() {
    if (currentTime == 0) {
        return -1;  // Unknown time
    }

    time_t localTime = currentTime + timezoneOffsetSeconds;
    struct tm* timeinfo = gmtime(&localTime);

    // Check for gmtime() failure (unlikely but defensive programming)
    if (timeinfo == nullptr) {
        logMessage(LOG_ERROR, "TIME", "gmtime() returned NULL - invalid time value");
        return -1;
    }

    return timeinfo->tm_hour;
}

/**
 * Check if current time is during quiet hours
 * Returns true if in quiet hours, false otherwise
 * Weekends (Saturday/Sunday) are quiet hours all day for battery savings
 */
bool isQuietHours() {
    const AppConfig& cfg = ConfigManager::getConfig();

    // Check if time is known
    if (currentTime == 0) {
        // Time unknown - not in quiet hours (fallback to normal sleep)
        return false;
    }

    // Get local time info including day of week
    time_t localTime = currentTime + timezoneOffsetSeconds;
    struct tm* timeinfo = gmtime(&localTime);

    // Check for gmtime() failure (defensive programming)
    if (timeinfo == nullptr) {
        logMessage(LOG_ERROR, "TIME", "gmtime() failed in isQuietHours()");
        return false;
    }

    // Check if it's weekend (Saturday=6 or Sunday=0)
    // Weekends are quiet hours all day for battery savings (~16% reduction)
    bool isWeekend = (timeinfo->tm_wday == 0 || timeinfo->tm_wday == 6);
    if (isWeekend) {
        return true;  // All day quiet hours on weekends
    }

    // Weekday: Check night-time quiet hours (e.g., 23:00 - 07:00)
    int hour = timeinfo->tm_hour;

    // Handle quiet hours that span midnight
    if (cfg.quiet_hours.start_hour > cfg.quiet_hours.end_hour) {
        // e.g., 23:00 - 07:00
        return (hour >= cfg.quiet_hours.start_hour || hour < cfg.quiet_hours.end_hour);
    } else {
        // e.g., 01:00 - 05:00 (unusual but supported)
        return (hour >= cfg.quiet_hours.start_hour && hour < cfg.quiet_hours.end_hour);
    }
}

/**
 * Calculate sleep duration in minutes, applying quiet hours multiplier if needed
 * Returns the sleep duration in minutes
 */
uint32_t calculateSleepDuration() {
    const AppConfig& cfg = ConfigManager::getConfig();

    // Override: WiFi fallback mode uses 60-minute sleep
    if (wifiPowerState.wifiDisabledMode) {
        logMessage(LOG_INFO, "POWER", "WiFi fallback mode - using 60-minute sleep");
        return 60;
    }

    uint32_t baseSleepMin = cfg.power.sleep_duration_min;

    if (isQuietHours()) {
        uint32_t quietSleepMin = baseSleepMin * cfg.quiet_hours.sleep_multiplier;
        char logBuf[64];
        snprintf(logBuf, sizeof(logBuf),
                "base=%dmin multiplier=%dx quiet=%dmin",
                baseSleepMin,
                cfg.quiet_hours.sleep_multiplier,
                quietSleepMin);
        logMessage(LOG_INFO, "POWER", "Quiet hours active - extended sleep", logBuf);
        return quietSleepMin;
    } else {
        return baseSleepMin;
    }
}

// ============================================================================
// Graceful Shutdown Handler
// ============================================================================
// Global flags for sleep state management
static bool enteringSleep = false;      // Set when entering deep sleep
static bool justWokeFromSleep = false;  // Set on wake, cleared after first connection

void gracefulShutdown(const char* reason, bool clearDisplay = false) {
    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "reason=%s", reason);
    logMessage(LOG_INFO, "SHUTDOWN", "Starting graceful shutdown", logBuf);

    // Stop accepting new messages
    wsConnected = false;

    // Disconnect WebSocket cleanly
    if (WiFi.status() == WL_CONNECTED) {
        logMessage(LOG_INFO, "SHUTDOWN", "Disconnecting WebSocket");
        webSocket.disconnect();
        delay(100);  // Give time for disconnect to complete
    }

    // Disconnect WiFi
    if (WiFi.status() == WL_CONNECTED) {
        logMessage(LOG_INFO, "SHUTDOWN", "Disconnecting WiFi");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(100);
    }

    // Handle display cleanup
    if (display) {
        if (clearDisplay) {
            logMessage(LOG_INFO, "SHUTDOWN", "Clearing display");
            display->clearScreen();
            // Update display state after clear (full refresh happened)
            updateDisplayStateAfterFullRefresh();
            delay(100);
        } else if (!enteringSleep) {
            // Only show shutdown message if NOT entering sleep (preserve reaction display during sleep)
            logMessage(LOG_INFO, "SHUTDOWN", "Showing shutdown message");
            DisplayManager::showMessage("System", "Shutting down...", reason, "");
            // Note: showMessage() already calls updateDisplayStateAfterFullRefresh()
            delay(100);
        } else {
            // Entering sleep - preserve current display state (reaction or previous screen)
            logMessage(LOG_INFO, "SHUTDOWN", "Preserving display for deep sleep");
        }

        // Hibernate display to preserve last image and save power
        logMessage(LOG_INFO, "SHUTDOWN", "Hibernating display");
        display->hibernate();
        delay(100);
    }

    // Cleanup security resources
    if (SecurityManager::isEnabled()) {
        logMessage(LOG_INFO, "SHUTDOWN", "Cleaning up security resources");
        // SecurityManager will clean up automatically on restart
    }

    logMessage(LOG_INFO, "SHUTDOWN", "Graceful shutdown complete");
    delay(100);  // Final delay to ensure log messages are transmitted
}

void enterDeepSleep(uint32_t sleep_minutes) {
    const AppConfig& cfg = ConfigManager::getConfig();

    logMessage(LOG_INFO, "POWER", "Entering deep sleep",
               String("minutes=" + String(sleep_minutes)).c_str());

    // Set flag to suppress disconnect/shutdown messages
    enteringSleep = true;

    // Perform graceful shutdown first (this disconnects WebSocket/WiFi)
    // IMPORTANT: Don't clear display - preserve last reaction on screen during sleep
    gracefulShutdown("deep_sleep", false);  // clearDisplay=false to preserve display state

    // Display will hibernate and preserve current image (reaction or status message)
    // E-paper displays retain their image without power - this is the key advantage!

    // Configure wake up timer
    uint64_t sleep_time_us = sleep_minutes * 60 * 1000000ULL;
    esp_sleep_enable_timer_wakeup(sleep_time_us);

    // Configure wake up on GPIO 39 button press (active LOW)
    // GPIO 39 is the built-in button on LilyGo T5 V2.3
    // The button pulls the pin LOW when pressed
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_39, 0);  // Wake when GPIO 39 goes LOW

    // Enter deep sleep
    logMessage(LOG_INFO, "POWER", "Entering deep sleep now", "wake_sources=timer,button_gpio39");
    delay(50);
    esp_deep_sleep_start();
}

// ============================================================================
// WebSocket Event Handler - Using modern C++ features
// ============================================================================
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED: {
            logMessage(LOG_WARN, "WS", "Disconnected");
            wsConnected = false;
            metrics.failedConnections++;
            ResilienceManager::markConnectionLost();  // Track connection loss

            // Only show disconnect message after 10+ failed attempts (real problem, not temporary disconnect)
            // This preserves the reaction display during normal reconnections and sleep cycles
            if (!enteringSleep && metrics.failedConnections >= 10) {
                DisplayManager::showMessage("Connection Lost", "Check WiFi/Server");
            }
            break;
        }

        case WStype_CONNECTED: {
            char logBuf[128];
            snprintf(logBuf, sizeof(logBuf), "url=%s", reinterpret_cast<char*>(payload));
            logMessage(LOG_INFO, "WS", "Connected", logBuf);
            wsConnected = true;
            lastHeartbeat = millis();  // Initialize heartbeat timer on connection
            metrics.totalConnections++;
            metrics.failedConnections = 0;  // Reset failed counter on successful connection
            ResilienceManager::markConnectionRestored();  // Track restoration
            ResilienceManager::recordHeartbeat();  // Record initial heartbeat
            const AppConfig& cfg = ConfigManager::getConfig();

            // Send registration message with AES key if encryption is enabled
            if (cfg.security.use_aes && !cfg.security.aes_key.isEmpty()) {
                // Convert base64 key to hex for server
                String hexKey = "";
                size_t keyLen = 32;
                unsigned char decodedKey[32];
                size_t outLen;

                // Decode base64 to bytes
                int ret = mbedtls_base64_decode(decodedKey, keyLen, &outLen,
                                               (const unsigned char*)cfg.security.aes_key.c_str(),
                                               cfg.security.aes_key.length());

                if (ret == 0 && outLen == 32) {
                    // Convert bytes to hex string
                    char hexBuf[3];
                    for (int i = 0; i < 32; i++) {
                        snprintf(hexBuf, sizeof(hexBuf), "%02x", decodedKey[i]);
                        hexKey += hexBuf;
                    }

                    // Send registration message
                    JsonDocument regDoc;
                    regDoc["device_type"] = "esp32_eink";
                    regDoc["aes_key"] = hexKey;

                    String regMsg;
                    serializeJson(regDoc, regMsg);
                    webSocket.sendTXT(regMsg);

                    logMessage(LOG_INFO, "WS", "Sent AES registration");
                } else {
                    logMessage(LOG_WARN, "WS", "Failed to decode AES key for registration");
                }
            }

            // Only show "Connected!" message if NOT waking from sleep (preserve reaction display)
            if (!justWokeFromSleep) {
                DisplayManager::showMessage("", "Connected!", "Waiting for Reactions..", "", cfg.security.use_aes);
            } else {
                logMessage(LOG_INFO, "WS", "Connected after wake - preserving display");
                justWokeFromSleep = false;  // Clear flag after first connection
            }
            break;
        }

        case WStype_TEXT: {
            handleWebSocketMessage(payload, length);
            break;
        }

        case WStype_ERROR: {
            char logBuf[128];
            snprintf(logBuf, sizeof(logBuf), "error=%s", reinterpret_cast<char*>(payload));
            logMessage(LOG_ERROR, "WS", "Error occurred", logBuf);
            break;
        }

        case WStype_PING:
        case WStype_PONG:
            logMessage(LOG_DEBUG, "WS", "Ping/Pong");
            break;

        default:
            char logBuf[64];
            snprintf(logBuf, sizeof(logBuf), "type=%d", type);
            logMessage(LOG_WARN, "WS", "Unhandled event", logBuf);
            break;
    }
}

// ============================================================================
// Message Processing - Separated for clarity
// ============================================================================
void handleWebSocketMessage(const uint8_t* payload, size_t length) {
    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf), "len=%d", length);
    logMessage(LOG_DEBUG, "WS", "Message received", logBuf);

    // Update last message time for any received message
    lastHeartbeat = millis();

    JsonDocument doc;

    // Check if message starts with "AES:" prefix (encrypted hex format)
    String message((const char*)payload, length);
    if (message.startsWith("AES:") && SecurityManager::isEnabled()) {
        logMessage(LOG_DEBUG, "SECURITY", "Received AES encrypted hex message");

        // Remove "AES:" prefix
        String hexData = message.substring(4);

        // Hex string should be: IV (32 hex chars = 16 bytes) + ciphertext
        if (hexData.length() < 32) {
            logMessage(LOG_ERROR, "SECURITY", "Encrypted message too short");
            return;
        }

        // Extract IV (first 32 hex chars)
        String ivHex = hexData.substring(0, 32);
        // Extract ciphertext (remaining hex chars)
        String ciphertextHex = hexData.substring(32);

        snprintf(logBuf, sizeof(logBuf), "iv_len=%d cipher_len=%d", ivHex.length(), ciphertextHex.length());
        logMessage(LOG_DEBUG, "SECURITY", "Parsing encrypted data", logBuf);

        // Decrypt the message using hex format
        String decrypted = SecurityManager::decryptHex(ciphertextHex, ivHex);
        if (decrypted.isEmpty()) {
            logMessage(LOG_ERROR, "SECURITY", "Failed to decrypt message");
            return;
        }

        logMessage(LOG_DEBUG, "SECURITY", "Message decrypted successfully");

        // Parse decrypted JSON
        DeserializationError error = deserializeJson(doc, decrypted);
        if (error) {
            snprintf(logBuf, sizeof(logBuf), "error=%s", error.c_str());
            logMessage(LOG_ERROR, "JSON", "Parse error in decrypted data", logBuf);
            return;
        }
    } else {
        // Parse as unencrypted JSON
        DeserializationError error = deserializeJson(doc, payload, length);
        if (error) {
            snprintf(logBuf, sizeof(logBuf), "error=%s", error.c_str());
            logMessage(LOG_ERROR, "JSON", "Parse error", logBuf);
            return;
        }
    }

    // Process the message content
    const char* msgType = doc["type"];
    if (!msgType) {
        logMessage(LOG_WARN, "WS", "Message missing type field");
        return;
    }

    // Use string comparison with early returns for efficiency
    if (strcmp(msgType, "heartbeat") == 0) {
        metrics.heartbeatsReceived++;
        ResilienceManager::recordHeartbeat();  // Track for health monitoring
        logMessage(LOG_DEBUG, "WS", "Heartbeat received");
        return;
    }

    if (strcmp(msgType, "reaction") == 0) {
        metrics.messagesReceived++;
        logMessage(LOG_INFO, "WS", "Reaction received");

        // Extract message ID for ACK
        const char* messageId = doc["message_id"];

        // Display the reaction
        JsonObject reaction = doc.as<JsonObject>();
        DisplayManager::showReaction(reaction);
        lastReactionTime = millis();  // Track for idle detection

        // Send ACK back to server
        if (messageId && wsConnected) {
            JsonDocument ackDoc;
            ackDoc["type"] = "ack";
            ackDoc["id"] = messageId;

            String ackJson;
            serializeJson(ackDoc, ackJson);
            webSocket.sendTXT(ackJson);

            snprintf(logBuf, sizeof(logBuf), "id=%s", messageId);
            logMessage(LOG_DEBUG, "WS", "ACK sent", logBuf);
        } else if (!messageId) {
            logMessage(LOG_WARN, "WS", "Message missing message_id field");
        }

        return;
    }

    if (strcmp(msgType, "firmware_update") == 0) {
        logMessage(LOG_INFO, "OTA", "Firmware update notification received");
        const char* version = doc["version"];
        bool required = doc["required"] | false;

        if (required) {
            logMessage(LOG_WARN, "OTA", "Required update - installing immediately");
            performOTAUpdate();
        } else {
            logMessage(LOG_INFO, "OTA", "Optional update - will install when idle");
            pendingUpdate = true;
            pendingVersion = String(version ? version : "");
        }
        return;
    }

    if (strcmp(msgType, "error") == 0) {
        const char* errorCode = doc["code"] | "";
        const char* errorMsg = doc["message"] | "Unknown error";
        const char* deviceId = doc["device_id"] | "";

        snprintf(logBuf, sizeof(logBuf), "code=%s message=\"%s\"", errorCode, errorMsg);
        logMessage(LOG_ERROR, "WS", "Server error", logBuf);

        // Show specific screen for registration errors
        if (strcmp(errorCode, "DEVICE_NOT_REGISTERED") == 0 || strcmp(errorCode, "DEVICE_NOT_LINKED") == 0) {
            String line1 = "Device not registered";
            String line2 = deviceId[0] ? String("ID: ") + deviceId : "";
            String line3 = "Please register via";
            String line4 = "Slack app or server";
            DisplayManager::showMessage(line1, line2, line3, line4);

            // Slow down reconnect attempts for unregistered devices
            delay(10000);  // Wait 10 seconds before reconnecting
        } else {
            // Generic error screen
            DisplayManager::showMessage("Error:", errorMsg);
        }
        return;
    }

    snprintf(logBuf, sizeof(logBuf), "type=%s", msgType);
    logMessage(LOG_WARN, "WS", "Unknown message type", logBuf);
}

// ============================================================================
// Test Command Processing
// ============================================================================
#ifdef ENABLE_DEBUG_FEATURES
void processSerialCommand(const String& command) {
    logMessage(LOG_TEST, "CMD", command.c_str());

    if (command.startsWith("TEST:WIFI")) {
        if (WiFi.status() == WL_CONNECTED) {
            char buf[128];
            snprintf(buf, sizeof(buf), "ip=%s rssi=%d",
                     WiFi.localIP().toString().c_str(), WiFi.RSSI());
            logMessage(LOG_TEST, "WIFI", "Connected", buf);
        } else {
            const char* status = "unknown";
            switch(WiFi.status()) {
                case WL_NO_SHIELD: status = "no_shield"; break;
                case WL_IDLE_STATUS: status = "idle"; break;
                case WL_NO_SSID_AVAIL: status = "no_ssid"; break;
                case WL_SCAN_COMPLETED: status = "scan_completed"; break;
                case WL_CONNECT_FAILED: status = "connect_failed"; break;
                case WL_CONNECTION_LOST: status = "connection_lost"; break;
                case WL_DISCONNECTED: status = "disconnected"; break;
            }
            char buf[64];
            snprintf(buf, sizeof(buf), "status=%s", status);
            logMessage(LOG_TEST, "WIFI", "Not connected", buf);
        }
    }
    else if (command.startsWith("TEST:WS")) {
        char buf[128];
        snprintf(buf, sizeof(buf), "connected=%s device_id=%s",
                 wsConnected ? "true" : "false",
                 ConfigManager::getConfig().device.id.c_str());
        logMessage(LOG_TEST, "WS", "Status", buf);
    }
    else if (command.startsWith("TEST:MSG:")) {
        String json = command.substring(9);
        logMessage(LOG_TEST, "MSG", "Injecting message");
        injectTestMessage(json);
    }
    else if (command.startsWith("TEST:DISPLAY:")) {
        String text = command.substring(13);
        if (text.startsWith("emoji:")) {
            String emoji = text.substring(6);
            JsonDocument doc;
            doc["type"] = "reaction";
            doc["emoji"] = emoji;
            doc["user"] = "TestUser";
            doc["channel"] = "test-channel";
            doc["message"] = "Test message";
            doc["encrypted"] = false;
            DisplayManager::showReaction(doc.as<JsonObject>());
        } else {
            DisplayManager::showMessage(text, "Line 2", "Line 3", "Line 4");
        }
    }
    else if (command == "TEST:CONFIG") {
        logMessage(LOG_TEST, "CONFIG", "Current configuration:");

        if (ConfigManager::isLoaded()) {
            const AppConfig& cfg = ConfigManager::getConfig();
            char buf[256];

            snprintf(buf, sizeof(buf), "id=%s name=%s",
                     cfg.device.id.c_str(), cfg.device.name.c_str());
            logMessage(LOG_TEST, "CONFIG", "Device", buf);

            snprintf(buf, sizeof(buf), "ssid=%s timeout=%lu",
                     cfg.wifi.ssid.c_str(), cfg.wifi.timeout_ms);
            logMessage(LOG_TEST, "CONFIG", "WiFi", buf);

            snprintf(buf, sizeof(buf), "host=%s port=%d ssl=%s",
                     cfg.server.host.c_str(), cfg.server.port, cfg.server.use_ssl ? "true" : "false");
            logMessage(LOG_TEST, "CONFIG", "Server", buf);

            snprintf(buf, sizeof(buf), "width=%d height=%d rotation=%d",
                     cfg.display.width, cfg.display.height, cfg.display.rotation);
            logMessage(LOG_TEST, "CONFIG", "Display", buf);
        } else {
            logMessage(LOG_TEST, "CONFIG", "Configuration not loaded!");
        }
    }
    else if (command.startsWith("TEST:CONFIG:SET:")) {
        String remainder = command.substring(16);
        int colonPos = remainder.indexOf(':');
        if (colonPos > 0) {
            String key = remainder.substring(0, colonPos);
            String value = remainder.substring(colonPos + 1);

            AppConfig& cfg = ConfigManager::getMutableConfig();
            bool updated = false;

            if (key == "wifi.ssid") {
                cfg.wifi.ssid = value;
                updated = true;
            } else if (key == "wifi.password") {
                cfg.wifi.password = value;
                updated = true;
            } else if (key == "device.id") {
                cfg.device.id = value;
                updated = true;
            } else if (key == "device.name") {
                cfg.device.name = value;
                updated = true;
            } else if (key == "server.host") {
                cfg.server.host = value;
                updated = true;
            } else if (key == "security.use_aes") {
                cfg.security.use_aes = (value == "true" || value == "1");
                updated = true;
            } else if (key == "security.aes_key") {
                cfg.security.aes_key = value;
                updated = true;
            }

            if (updated) {
                char buf[128];
                snprintf(buf, sizeof(buf), "key=%s value=%s", key.c_str(), value.c_str());
                logMessage(LOG_TEST, "CONFIG", "Updated", buf);
            } else {
                char buf[64];
                snprintf(buf, sizeof(buf), "key=%s", key.c_str());
                logMessage(LOG_TEST, "CONFIG", "Unknown key", buf);
            }
        }
    }
    else if (command.startsWith("TEST:CONFIG:SAVE")) {
        if (ConfigManager::save()) {
            logMessage(LOG_TEST, "CONFIG", "Configuration saved to SPIFFS");

            File f = SPIFFS.open("/config.json", "r");
            if (f) {
                char buf[64];
                snprintf(buf, sizeof(buf), "size=%d bytes", f.size());
                logMessage(LOG_TEST, "CONFIG", "Config file created", buf);
                f.close();
            }
        } else {
            logMessage(LOG_ERROR, "CONFIG", "Failed to save configuration");
        }
    }
    else if (command.startsWith("TEST:CONFIG:RELOAD")) {
        if (ConfigManager::load()) {
            const AppConfig& cfg = ConfigManager::getConfig();
            char buf[256];
            snprintf(buf, sizeof(buf), "device_id=%s wifi_ssid=%s",
                     cfg.device.id.c_str(), cfg.wifi.ssid.c_str());
            logMessage(LOG_TEST, "CONFIG", "Configuration reloaded", buf);
        } else {
            logMessage(LOG_ERROR, "CONFIG", "Failed to reload configuration");
        }
    }
    else if (command.startsWith("TEST:AES:ENCRYPT:")) {
        String plaintext = command.substring(17);

        if (!SecurityManager::isEnabled()) {
            logMessage(LOG_TEST, "AES", "Not configured");
            return;
        }

        String ivB64;
        String encrypted = SecurityManager::encrypt(plaintext, ivB64);

        if (!encrypted.isEmpty()) {
            char buf[256];
            snprintf(buf, sizeof(buf), "plain_len=%d enc_len=%d",
                     plaintext.length(), encrypted.length());
            logMessage(LOG_TEST, "AES", "Encrypted", buf);

            logMessage(LOG_TEST, "AES", "IV", ivB64.c_str());
            logMessage(LOG_TEST, "AES", "Data", encrypted.c_str());
        } else {
            logMessage(LOG_ERROR, "AES", "Encryption failed");
        }
    }
    else if (command.startsWith("TEST:AES:DECRYPT:")) {
        String params = command.substring(17);
        int colonPos = params.indexOf(':');

        if (colonPos < 1) {
            logMessage(LOG_ERROR, "AES", "Invalid format, use TEST:AES:DECRYPT:iv:data");
            return;
        }

        String iv = params.substring(0, colonPos);
        String data = params.substring(colonPos + 1);

        if (!SecurityManager::isEnabled()) {
            logMessage(LOG_TEST, "AES", "Not configured");
            return;
        }

        String decrypted = SecurityManager::decrypt(data, iv);
        if (!decrypted.isEmpty()) {
            char buf[128];
            snprintf(buf, sizeof(buf), "dec_len=%d", decrypted.length());
            logMessage(LOG_TEST, "AES", "Decrypted", buf);
            logMessage(LOG_TEST, "AES", "Plaintext", decrypted.c_str());
        } else {
            logMessage(LOG_ERROR, "AES", "Decryption failed");
        }
    }
    else if (command.startsWith("TEST:AES:SELF")) {
        if (!SecurityManager::isEnabled()) {
            logMessage(LOG_TEST, "AES", "Not configured");
            return;
        }

        bool passed = SecurityManager::selfTest();
        logMessage(LOG_TEST, "AES", passed ? "Self-test passed" : "Self-test failed");
    }
    else if (command.startsWith("TEST:AES:MSG:")) {
        String json = command.substring(13);

        if (!SecurityManager::isEnabled()) {
            logMessage(LOG_TEST, "AES", "Not configured, injecting unencrypted");
            injectTestMessage(json);
            return;
        }

        String ivB64;
        String encrypted = SecurityManager::encrypt(json, ivB64);

        if (encrypted.isEmpty()) {
            logMessage(LOG_ERROR, "AES", "Failed to encrypt test message");
            return;
        }

        JsonDocument wrapper;
        wrapper["encrypted"] = true;
        wrapper["iv"] = ivB64;
        wrapper["data"] = encrypted;

        String wrappedJson;
        serializeJson(wrapper, wrappedJson);

        logMessage(LOG_TEST, "AES", "Injecting encrypted message");
        injectTestMessage(wrappedJson);
    }
    else if (command.startsWith("TEST:RESILIENCE")) {
        String status = ResilienceManager::getHealthStatus();
        logMessage(LOG_TEST, "RESILIENCE", "Status", status.c_str());

        char buf[256];
        snprintf(buf, sizeof(buf), "healthy=%s queue_size=%zu heartbeat_timeout=%lums",
                 ResilienceManager::isConnectionHealthy() ? "true" : "false",
                 ResilienceManager::getQueueSize(),
                 ResilienceManager::getTimeSinceLastHeartbeat());
        logMessage(LOG_TEST, "RESILIENCE", "Health", buf);
    }
    else if (command.startsWith("TEST:QUEUE:")) {
        String json = command.substring(11);
        if (ResilienceManager::queueMessage(json)) {
            char buf[64];
            snprintf(buf, sizeof(buf), "queue_size=%zu", ResilienceManager::getQueueSize());
            logMessage(LOG_TEST, "RESILIENCE", "Message queued", buf);
        } else {
            logMessage(LOG_ERROR, "RESILIENCE", "Failed to queue message");
        }
    }
    else if (command == "TEST:QUEUE:PROCESS") {
        if (ResilienceManager::hasQueuedMessages()) {
            bool processed = ResilienceManager::processQueuedMessages();
            logMessage(LOG_TEST, "RESILIENCE", processed ? "Queue processed" : "Queue processing failed");
        } else {
            logMessage(LOG_TEST, "RESILIENCE", "Queue empty");
        }
    }
    else if (command == "TEST:HEARTBEAT:TIMEOUT") {
        logMessage(LOG_TEST, "RESILIENCE", "Simulating heartbeat timeout");
    }
    else if (command.startsWith("TEST:METRICS")) {
        uint32_t uptime = (millis() - metrics.startTime) / 1000;
        float successRate = metrics.totalConnections > 0 ?
                           (float)(metrics.totalConnections - metrics.failedConnections) / metrics.totalConnections : 0;

        char buf[256];
        snprintf(buf, sizeof(buf),
                 "uptime_s=%lu connections=%lu failed=%lu messages=%lu heartbeats=%lu success_rate=%.2f",
                 uptime, metrics.totalConnections, metrics.failedConnections,
                 metrics.messagesReceived, metrics.heartbeatsReceived, successRate);
        logMessage(LOG_TEST, "METRICS", "Stats", buf);
    }
    else if (command.startsWith("LOG:LEVEL:")) {
        String level = command.substring(10);
        if (level == "ERROR") currentLogLevel = LOG_ERROR;
        else if (level == "WARN") currentLogLevel = LOG_WARN;
        else if (level == "INFO") currentLogLevel = LOG_INFO;
        else if (level == "DEBUG") currentLogLevel = LOG_DEBUG;
        else if (level == "TEST") currentLogLevel = LOG_TEST;

        char buf[32];
        snprintf(buf, sizeof(buf), "level=%d", currentLogLevel);
        logMessage(LOG_INFO, "LOG", "Level changed", buf);
    }
    else if (command == "TEST:SHUTDOWN") {
        logMessage(LOG_TEST, "SHUTDOWN", "Testing graceful shutdown");
        gracefulShutdown("test_command", false);
        logMessage(LOG_TEST, "SHUTDOWN", "Graceful shutdown complete - device will restart in 3s");
        delay(3000);
        ESP.restart();
    }
    else if (command == "TEST:SHUTDOWN:CLEAR") {
        logMessage(LOG_TEST, "SHUTDOWN", "Testing graceful shutdown with clear display");
        gracefulShutdown("test_command_clear", true);
        logMessage(LOG_TEST, "SHUTDOWN", "Graceful shutdown complete - device will restart in 3s");
        delay(3000);
        ESP.restart();
    }
    else if (command == "TEST:LOCK:ICONS") {
        logMessage(LOG_TEST, "DISPLAY", "Testing lock icons");
        display->setFullWindow();
        display->firstPage();
        do {
            display->fillScreen(GxEPD_WHITE);
            display->setTextColor(GxEPD_BLACK);
            display->setFont(&FreeSans9pt7b);

            DisplayManager::drawBatteryIndicator();

            DisplayManager::drawLockIcon(5, 5);
            display->setCursor(30, 20);
            display->print("Locked");

            DisplayManager::drawUnlockIcon(5, 40);
            display->setCursor(30, 55);
            display->print("Unlocked");

            DisplayManager::drawLockIcon(5, 75);
            DisplayManager::drawUnlockIcon(30, 75);
            display->setCursor(60, 90);
            display->print("Comparison");

        } while (display->nextPage());
        logMessage(LOG_TEST, "DISPLAY", "Lock icons displayed");
    }
    else {
        logMessage(LOG_WARN, "CMD", "Unknown command", command.c_str());
    }
}

void injectTestMessage(const String& jsonPayload) {
    handleWebSocketMessage((uint8_t*)jsonPayload.c_str(), jsonPayload.length());
}
#endif

// ============================================================================
// Network Management
// ============================================================================
class NetworkManager {
public:
    static bool connectWiFi(bool silent = false) {
        const AppConfig& cfg = ConfigManager::getConfig();
        char logBuf[128];

        // If no WiFi credentials configured, go straight to provisioning
        if (cfg.wifi.ssid.isEmpty()) {
            logMessage(LOG_WARN, "WIFI", "No credentials configured, entering provisioning mode");
            return startProvisioning();
        }

        // Check WiFi fallback mode (low-power mode after repeated failures)
        if (wifiPowerState.wifiDisabledMode) {
            wifiPowerState.fallbackWakeCount++;

            // Retry every 6 hours (6 wakes at 60-min sleep = 6 hours)
            if (wifiPowerState.fallbackWakeCount >= 6) {
                // Reset and retry
                wifiPowerState.wifiDisabledMode = false;
                wifiPowerState.fallbackWakeCount = 0;
                wifiPowerState.currentPower = WIFI_POWER_11dBm;  // Reset to LOW
                wifiPowerState.consecutiveFailures = 0;
                wifiPowerState.totalFailedWakes = 0;
                logMessage(LOG_INFO, "WIFI", "Exiting fallback mode - resetting to LOW power");
            } else {
                // Still in fallback - skip WiFi attempt
                snprintf(logBuf, sizeof(logBuf), "wake_count=%d/6 next_retry_in=%dh",
                         wifiPowerState.fallbackWakeCount, 6 - wifiPowerState.fallbackWakeCount);
                logMessage(LOG_INFO, "WIFI", "In fallback mode - skipping WiFi", logBuf);
                return false;
            }
        }

        // Try connecting 3 times before entering provisioning mode
        const int maxRetries = 3;
        const uint32_t retryDelay = 5000;  // 5 seconds between retries

        for (int attempt = 1; attempt <= maxRetries; attempt++) {
            snprintf(logBuf, sizeof(logBuf), "attempt=%d/%d ssid=%s", attempt, maxRetries, cfg.wifi.ssid.c_str());
            logMessage(LOG_INFO, "WIFI", "Connecting", logBuf);

            // Only show WiFi connection screens on first boot, not on wake from sleep
            if (!silent) {
                String message = "Connecting";
                if (attempt > 1) {
                    message += " (" + String(attempt) + "/" + String(maxRetries) + ")";
                }
                DisplayManager::showMessage(message, cfg.wifi.ssid);
            }

            WiFi.mode(WIFI_STA);

            // Apply TX power (priority: forced > adaptive > default)
            wifi_power_t txPower = wifiPowerState.currentPower;
            const char* powerName = "UNKNOWN";

            if (!cfg.wifi.force_tx_power.isEmpty()) {
                // User forced a specific power level (takes priority, works with or without adaptive)
                if (cfg.wifi.force_tx_power == "LOW") {
                    txPower = WIFI_POWER_11dBm;
                    powerName = "LOW";
                } else if (cfg.wifi.force_tx_power == "MEDIUM") {
                    txPower = WIFI_POWER_15dBm;
                    powerName = "MEDIUM";
                } else if (cfg.wifi.force_tx_power == "HIGH") {
                    txPower = WIFI_POWER_19_5dBm;
                    powerName = "HIGH";
                }
            } else if (cfg.wifi.adaptive_tx_power) {
                // Use adaptive power from RTC memory
                if (txPower == WIFI_POWER_11dBm) {
                    powerName = "LOW";
                } else if (txPower == WIFI_POWER_15dBm) {
                    powerName = "MEDIUM";
                } else {
                    powerName = "HIGH";
                }
            } else {
                // Adaptive disabled and no force - use default HIGH
                txPower = WIFI_POWER_19_5dBm;
                powerName = "HIGH";
            }

            WiFi.setTxPower(txPower);
            snprintf(logBuf, sizeof(logBuf), "tx_power=%s", powerName);
            logMessage(LOG_DEBUG, "WIFI", "Setting TX power", logBuf);

            WiFi.begin(cfg.wifi.ssid.c_str(), cfg.wifi.password.c_str());

            const uint32_t startTime = millis();
            while (WiFi.status() != WL_CONNECTED) {
                if ((unsigned long)(millis() - startTime) > cfg.wifi.timeout_ms) {
                    logMessage(LOG_WARN, "WIFI", "Connection timeout", logBuf);
                    break;  // Exit inner loop, will retry
                }
                delay(500);
                Serial.print('.');
            }

            // Check if connected
            if (WiFi.status() == WL_CONNECTED) {
                int rssi = WiFi.RSSI();
                snprintf(logBuf, sizeof(logBuf), "ip=%s rssi=%d tx_power=%s",
                         WiFi.localIP().toString().c_str(), rssi, powerName);
                logMessage(LOG_INFO, "WIFI", "Connected", logBuf);

                // Reset adaptive power failures on successful connection
                if (cfg.wifi.adaptive_tx_power) {
                    wifiPowerState.consecutiveFailures = 0;
                    wifiPowerState.totalFailedWakes = 0;
                    // Keep current power level (sticky behavior)
                }

                // Give DNS servers time to be ready (prevents early OTA check failures)
                delay(2000);

                // Only show WiFi connected screen on first boot
                if (!silent) {
                    DisplayManager::showMessage("WiFi Connected",
                                               WiFi.localIP().toString(),
                                               "",
                                               "Connecting to server...");
                }
                return true;
            }

            // Failed attempt - handle adaptive power escalation
            if (cfg.wifi.adaptive_tx_power && cfg.wifi.force_tx_power.isEmpty()) {
                wifiPowerState.consecutiveFailures++;
                wifiPowerState.totalFailedWakes++;

                // Check for power escalation
                if (wifiPowerState.consecutiveFailures >= cfg.wifi.escalation_threshold) {
                    if (wifiPowerState.currentPower == WIFI_POWER_11dBm) {
                        // Escalate LOW → MEDIUM
                        wifiPowerState.currentPower = WIFI_POWER_15dBm;
                        wifiPowerState.consecutiveFailures = 0;
                        logMessage(LOG_WARN, "WIFI", "Connection failed 3 times at LOW, escalating to MEDIUM power");
                    } else if (wifiPowerState.currentPower == WIFI_POWER_15dBm) {
                        // Escalate MEDIUM → HIGH
                        wifiPowerState.currentPower = WIFI_POWER_19_5dBm;
                        wifiPowerState.consecutiveFailures = 0;
                        logMessage(LOG_WARN, "WIFI", "Connection failed 3 times at MEDIUM, escalating to HIGH power");
                    } else {
                        // Already at HIGH - can't escalate further
                        logMessage(LOG_ERROR, "WIFI", "Connection failed at HIGH power - not a TX power issue");
                    }
                }

                // Check for fallback mode entry
                if (wifiPowerState.totalFailedWakes >= cfg.wifi.max_failed_wakes) {
                    wifiPowerState.wifiDisabledMode = true;
                    wifiPowerState.fallbackWakeCount = 0;
                    snprintf(logBuf, sizeof(logBuf), "total_failures=%d threshold=%d",
                             wifiPowerState.totalFailedWakes, cfg.wifi.max_failed_wakes);
                    logMessage(LOG_ERROR, "WIFI", "Entering low-power fallback mode (60min sleep, retry in 6h)", logBuf);
                    return false;  // Don't continue retrying in this wake cycle
                }
            }

            // Wait before retry (unless it's the last attempt)
            if (attempt < maxRetries) {
                snprintf(logBuf, sizeof(logBuf), "retry_in=%dms", retryDelay);
                logMessage(LOG_WARN, "WIFI", "Retrying", logBuf);
                if (!silent) {
                    DisplayManager::showMessage("Connection failed", "Retrying...");
                }
                delay(retryDelay);
            }
        }

        // All retries failed - enter provisioning mode
        logMessage(LOG_ERROR, "WIFI", "All connection attempts failed, entering provisioning mode");
        if (!silent) {
            DisplayManager::showMessage("WiFi Setup", "Starting portal...");
        }
        return startProvisioning();
    }

    static bool startProvisioning() {
        logMessage(LOG_INFO, "WIFI", "Starting WiFi provisioning mode");

        WiFiManager wm;

        // Set custom AP name
        const char* apName = "SlackReact-Setup";

        // Configure callback to show provisioning UI on e-paper
        wm.setAPCallback([](WiFiManager* myWM) {
            logMessage(LOG_INFO, "WIFI", "Entered provisioning mode");
            String ssid = myWM->getConfigPortalSSID();
            String ip = WiFi.softAPIP().toString();
            DisplayManager::showProvisioningMode(ssid, ip);
        });

        // Configure callback when WiFi credentials are saved
        wm.setSaveConfigCallback([]() {
            logMessage(LOG_INFO, "WIFI", "Credentials saved");
            DisplayManager::showMessage("WiFi Saved!", "Restarting...");
        });

        // Set timeout for config portal (3 minutes)
        wm.setConfigPortalTimeout(180);

        // Try to connect or start AP
        if (wm.autoConnect(apName)) {
            // Connected! Save credentials to ConfigManager
            AppConfig& cfg = ConfigManager::getMutableConfig();
            cfg.wifi.ssid = WiFi.SSID();
            cfg.wifi.password = WiFi.psk();
            ConfigManager::save();

            char logBuf[128];
            snprintf(logBuf, sizeof(logBuf), "ip=%s ssid=%s",
                     WiFi.localIP().toString().c_str(), WiFi.SSID().c_str());
            logMessage(LOG_INFO, "WIFI", "Provisioned and connected", logBuf);

            DisplayManager::showMessage("WiFi Connected!",
                                       WiFi.localIP().toString(),
                                       "",
                                       "Restarting...");
            delay(2000);
            ESP.restart();
            return true;
        } else {
            // Timeout or user cancelled
            logMessage(LOG_ERROR, "WIFI", "Provisioning timeout or cancelled");
            DisplayManager::showMessage("Setup timeout", "Restarting...");
            delay(2000);
            ESP.restart();
            return false;
        }
    }

    static void connectWebSocket() {
        const AppConfig& cfg = ConfigManager::getConfig();
        char logBuf[256];
        snprintf(logBuf, sizeof(logBuf), "server=%s:%d device_id=%s",
                 cfg.server.host.c_str(), cfg.server.port, cfg.device.id.c_str());
        logMessage(LOG_INFO, "WS", "Connecting", logBuf);

        // Build WebSocket path
        String path = cfg.server.path + "?rpi_id=";
        path += cfg.device.id;

        // Use beginSSL for secure WebSocket connection if configured
        if (cfg.server.use_ssl) {
            webSocket.beginSSL(cfg.server.host.c_str(), cfg.server.port, path);
        } else {
            webSocket.begin(cfg.server.host.c_str(), cfg.server.port, path);
        }

        webSocket.onEvent(webSocketEvent);

        // Disable library auto-reconnect (manual handling) and internal heartbeat (conflicts with server heartbeats)
        webSocket.setReconnectInterval(0);

        // Update reconnect delay from config
        reconnectDelay = cfg.timing.ws_initial_reconnect_ms;
    }

    static void handleReconnection() {
        if (WiFi.status() != WL_CONNECTED) {
            handleWiFiReconnection();
        } else {
            handleWebSocketReconnection();
            // Removed client-side heartbeat timeout checking
            // Let server and WebSocket library handle connection health
            // checkHeartbeatTimeout();
        }
    }

private:
    static void handleWiFiReconnection() {
        const AppConfig& cfg = ConfigManager::getConfig();
        const uint32_t now = millis();
        if (now - lastWiFiReconnect > cfg.wifi.reconnect_interval_ms) {
            lastWiFiReconnect = now;
            logMessage(LOG_WARN, "WIFI", "Reconnecting");
            WiFi.reconnect();
        }
    }

    static void handleWebSocketReconnection() {
        const AppConfig& cfg = ConfigManager::getConfig();
        if (!wsConnected) {
            const uint32_t now = millis();
            if (now - lastReconnect > reconnectDelay) {
                lastReconnect = now;

                // Ensure we're fully disconnected before attempting reconnection
                // This prevents duplicate connections
                webSocket.disconnect();
                delay(100);  // Small delay to ensure disconnect completes

                char logBuf[64];
                snprintf(logBuf, sizeof(logBuf), "delay_ms=%lu", reconnectDelay);
                logMessage(LOG_INFO, "WS", "Reconnection attempt", logBuf);
                connectWebSocket();

                // Exponential backoff with maximum
                reconnectDelay = min(reconnectDelay * 2, cfg.timing.ws_max_reconnect_ms);
            }
        } else {
            // Reset reconnect delay on successful connection
            reconnectDelay = cfg.timing.ws_initial_reconnect_ms;
        }
    }

    static void checkHeartbeatTimeout() {
        const AppConfig& cfg = ConfigManager::getConfig();
        if (wsConnected && lastHeartbeat > 0) {
            uint32_t timeSinceHeartbeat = (unsigned long)(millis() - lastHeartbeat);
            // Only disconnect if we haven't received ANY messages (not just heartbeats) for double the timeout
            // The WebSocketsClient has its own ping/pong mechanism that should keep the connection alive
            if (timeSinceHeartbeat > (cfg.timing.heartbeat_timeout_ms * 2)) {
                char logBuf[64];
                snprintf(logBuf, sizeof(logBuf), "last_seen_ms=%lu", timeSinceHeartbeat);
                logMessage(LOG_WARN, "WS", "Connection appears stale, forcing reconnect", logBuf);

                // Force reconnection
                wsConnected = false;
                webSocket.disconnect();
                lastHeartbeat = 0;
            } else if (timeSinceHeartbeat > cfg.timing.heartbeat_timeout_ms) {
                // Just log a warning but don't disconnect yet
                char logBuf[64];
                snprintf(logBuf, sizeof(logBuf), "last_heartbeat_ms=%lu", timeSinceHeartbeat);
                logMessage(LOG_DEBUG, "WS", "No heartbeat received", logBuf);
            }
        }
    }
};

// ============================================================================
// Arduino Setup Function
// ============================================================================
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) {
        // Wait for serial port to connect (timeout after 3 seconds)
    }

    metrics.startTime = millis();

    Serial.println(F("\n\n========================================"));
    Serial.println(F("ESP32 Slack Reactions Client v2.0"));
    Serial.println(F("========================================"));

    // Check wake reason
    ++bootCount;
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    // Track if this is a wake from deep sleep (to skip boot screens)
    bool isWakeFromSleep = false;

    char bootInfo[128];
    switch(wakeup_reason) {
        case ESP_SLEEP_WAKEUP_TIMER:
            snprintf(bootInfo, sizeof(bootInfo), "boot=%d reason=timer", bootCount);
            logMessage(LOG_INFO, "POWER", "Wake from deep sleep", bootInfo);
            isWakeFromSleep = true;
            break;
        case ESP_SLEEP_WAKEUP_EXT0:
            snprintf(bootInfo, sizeof(bootInfo), "boot=%d reason=button_gpio39", bootCount);
            logMessage(LOG_INFO, "POWER", "Wake from button press", bootInfo);
            isWakeFromSleep = true;
            break;
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            snprintf(bootInfo, sizeof(bootInfo), "boot=%d reason=power_on", bootCount);
            logMessage(LOG_INFO, "POWER", "Power on reset", bootInfo);
            bootCount = 0;  // Reset counter on power on
            isWakeFromSleep = false;
            break;
        default:
            snprintf(bootInfo, sizeof(bootInfo), "boot=%d reason=other(%d)", bootCount, wakeup_reason);
            logMessage(LOG_INFO, "POWER", "Wake reason", bootInfo);
            isWakeFromSleep = false;
            break;
    }

    logMessage(LOG_INFO, "SYSTEM", "Boot", "version=2.0 debug=enabled");

    // Initialize ConfigManager FIRST
    logMessage(LOG_INFO, "CONFIG", "Initializing configuration");
    if (!ConfigManager::begin()) {
        logMessage(LOG_ERROR, "CONFIG", "Failed to load configuration");
        // Continue with defaults
    }

    const AppConfig& cfg = ConfigManager::getConfig();

    // Set log level from config
    if (cfg.logging.default_level == "ERROR") currentLogLevel = LOG_ERROR;
    else if (cfg.logging.default_level == "WARN") currentLogLevel = LOG_WARN;
    else if (cfg.logging.default_level == "INFO") currentLogLevel = LOG_INFO;
    else if (cfg.logging.default_level == "DEBUG") currentLogLevel = LOG_DEBUG;

    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "device_id=%s log_level=%s",
             cfg.device.id.c_str(), cfg.logging.default_level.c_str());
    logMessage(LOG_INFO, "SYSTEM", "Configuration loaded", logBuf);

    // Initialize display
    logMessage(LOG_INFO, "DISPLAY", "Initializing");

    // Configure SPI pins based on configuration
    SPI.begin(cfg.display.pins.sclk, -1, cfg.display.pins.mosi, cfg.display.pins.cs);

    // Create display instance based on build environment dimensions
    // Automatically selects driver based on DISPLAY_WIDTH and DISPLAY_HEIGHT
    #ifdef DISPLAY_4G_GRAYSCALE
        // 4-level grayscale displays
        #if defined(DISPLAY_WIDTH) && DISPLAY_WIDTH == 212
            #ifdef DISPLAY_GDEW0213I5F
                // 2.13" 4G (GDEW0213I5F) - UC8151/IL0373 controller, flexible
                display = new GxEPD2_4G_4G<GxEPD2_213_flex, GxEPD2_213_flex::HEIGHT>(
                    GxEPD2_213_flex(cfg.display.pins.cs, cfg.display.pins.dc,
                                    cfg.display.pins.rst, cfg.display.pins.busy)
                );
                logMessage(LOG_INFO, "DISPLAY", "Driver: 2.13\" 4G grayscale (GDEW0213I5F)");
            #else
                // 2.13" 4G (GDEY0213B74 / GDEM0213B74)
                display = new GxEPD2_4G_4G<GxEPD2_213_GDEY0213B74, GxEPD2_213_GDEY0213B74::HEIGHT>(
                    GxEPD2_213_GDEY0213B74(cfg.display.pins.cs, cfg.display.pins.dc,
                                            cfg.display.pins.rst, cfg.display.pins.busy)
                );
                logMessage(LOG_INFO, "DISPLAY", "Driver: 2.13\" 4G grayscale (GDEY0213B74)");
            #endif
        #endif
    #else
        // 2-level black & white displays
        #if defined(DISPLAY_WIDTH) && DISPLAY_WIDTH == 264 && defined(DISPLAY_HEIGHT) && DISPLAY_HEIGHT == 176
            // 2.7" BW (GDEY027T91)
            display = new GxEPD2_BW<GxEPD2_270, GxEPD2_270::HEIGHT>(
                GxEPD2_270(cfg.display.pins.cs, cfg.display.pins.dc,
                           cfg.display.pins.rst, cfg.display.pins.busy)
            );
            logMessage(LOG_INFO, "DISPLAY", "Driver: 2.7\" BW (GxEPD2_270)");
        #elif defined(DISPLAY_WIDTH) && DISPLAY_WIDTH == 212
            #ifdef DISPLAY_GDEW0213T5D
                // 2.13" BW (GDEW0213T5D) - UC8151D controller
                display = new GxEPD2_BW<GxEPD2_213_T5D, GxEPD2_213_T5D::HEIGHT>(
                    GxEPD2_213_T5D(cfg.display.pins.cs, cfg.display.pins.dc,
                                   cfg.display.pins.rst, cfg.display.pins.busy)
                );
                logMessage(LOG_INFO, "DISPLAY", "Driver: 2.13\" BW (GxEPD2_213_T5D)");
            #else
                // 2.13" BW (DEPG0213BN)
                display = new GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT>(
                    GxEPD2_213_B74(cfg.display.pins.cs, cfg.display.pins.dc,
                                   cfg.display.pins.rst, cfg.display.pins.busy)
                );
                logMessage(LOG_INFO, "DISPLAY", "Driver: 2.13\" BW (GxEPD2_213_B74)");
            #endif
        #else
            // Default to 2.13" BW if dimensions not specified
            display = new GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT>(
                GxEPD2_213_B74(cfg.display.pins.cs, cfg.display.pins.dc,
                               cfg.display.pins.rst, cfg.display.pins.busy)
            );
            logMessage(LOG_INFO, "DISPLAY", "Driver: 2.13\" BW default (GxEPD2_213_B74)");
        #endif
    #endif

    // Initialize display
    // Parameters: baud, initial (true after deep sleep), reset_duration, pulldown_rst_mode
    // 'initial' parameter is important for re-init after deep sleep
    bool fromDeepSleep = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);
    display->init(115200, !fromDeepSleep, 2, false);  // initial=true on power-on, false on wake from sleep
    display->setRotation(cfg.display.rotation);

    char buf[128];
    snprintf(buf, sizeof(buf), "width=%d height=%d rotation=%d",
             cfg.display.width, cfg.display.height, cfg.display.rotation);
    logMessage(LOG_INFO, "DISPLAY", "Initialized", buf);

    // Show boot screen only on power-on, not on wake from deep sleep
    if (!isWakeFromSleep) {
        DisplayManager::showMessage("Slack Reactions", "Starting...", cfg.device.name, "v2.0");
    } else {
        // On wake from sleep, preserve display state (e-paper retains image without power)
        logMessage(LOG_INFO, "DISPLAY", "Preserving display state on wake from sleep");
    }

    // Initialize security if AES key is configured
    if (cfg.security.use_aes && !cfg.security.aes_key.isEmpty()) {
        if (SecurityManager::init(cfg.security.aes_key)) {
            logMessage(LOG_INFO, "SYSTEM", "AES-256 encryption enabled");

            // Run self-test
            if (SecurityManager::selfTest()) {
                logMessage(LOG_INFO, "SYSTEM", "AES self-test passed");
            } else {
                logMessage(LOG_WARN, "SYSTEM", "AES self-test failed");
            }
        } else {
            logMessage(LOG_ERROR, "SYSTEM", "Failed to initialize AES encryption");
        }
    } else {
        logMessage(LOG_INFO, "SYSTEM", "AES encryption disabled");
    }

    // Initialize resilience manager with heartbeat settings
    ResilienceManager::init(cfg.timing.heartbeat_interval_ms, cfg.timing.heartbeat_timeout_ms);
    logMessage(LOG_INFO, "SYSTEM", "Resilience manager initialized");

    // Check power source at startup
    logMessage(LOG_INFO, "POWER", "Checking power source at startup");
    BatteryStatus startupBattery = getBatteryStatus();
    bool usbPowered = startupBattery.isUSBPowered;
    char powerBuf[128];
    snprintf(powerBuf, sizeof(powerBuf), "startup_power_source=%s battery=%d%% voltage=%.2fV sleep_enabled=%s",
             usbPowered ? "USB" : "BATTERY",
             startupBattery.percentage,
             startupBattery.voltage,
             cfg.power.sleep_enabled ? "true" : "false");
    logMessage(LOG_INFO, "POWER", "Startup power status", powerBuf);

    // CPU frequency scaling for battery savings
    // 160MHz provides good balance: ~30% power savings vs 240MHz, minimal performance impact
    if (cfg.power.sleep_enabled && !usbPowered) {
        setCpuFrequencyMhz(160);
        logMessage(LOG_INFO, "POWER", "CPU frequency set to 160MHz for battery savings");
    } else {
        // USB powered or sleep disabled - use full speed
        setCpuFrequencyMhz(240);
        logMessage(LOG_INFO, "POWER", "CPU frequency set to 240MHz (USB powered or sleep disabled)");
    }

    // Verify CPU frequency was set correctly
    char cpuBuf[64];
    snprintf(cpuBuf, sizeof(cpuBuf), "actual=%dMHz", getCpuFrequencyMhz());
    logMessage(LOG_INFO, "POWER", "CPU frequency verified", cpuBuf);

    // CRITICAL: Check for low battery on wake BEFORE WiFi connection
    // WiFi is the most power-hungry operation (~10s × 180mA = 0.5mAh per wake cycle)
    // If battery is critically low, skip WiFi entirely and go back to sleep
    if (isWakeFromSleep && cfg.power.sleep_enabled && !usbPowered) {
        using namespace BatteryConstants;
        if (startupBattery.percentage >= 0 && startupBattery.percentage < LOW_BATTERY_SLEEP_THRESHOLD) {
            char logBuf[128];
            snprintf(logBuf, sizeof(logBuf), "battery=%d%% voltage=%.2fV - skipping WiFi, sleeping immediately",
                     startupBattery.percentage, startupBattery.voltage);
            logMessage(LOG_WARN, "POWER", "Critical low battery on wake", logBuf);

            // Sleep immediately without connecting WiFi (saves ~0.5mAh per cycle)
            enterDeepSleep(calculateSleepDuration());
            // Never returns, but for clarity:
            return;
        }
    }

    // Small delay before WiFi connection to let radio stabilize
    logMessage(LOG_INFO, "WIFI", "Waiting for WiFi radio to stabilize");
    delay(500);

    // Connect to WiFi (silent mode on wake from sleep to skip connection screens)
    if (NetworkManager::connectWiFi(isWakeFromSleep)) {
        // On wake from sleep: conditionally refresh display based on config
        if (isWakeFromSleep) {
            logMessage(LOG_INFO, "DISPLAY", "Wake from sleep - checking display update policy");

            // Reset sleep flag (we're awake now)
            enteringSleep = false;

            // Set flag to suppress "Connected!" message when WebSocket reconnects
            justWokeFromSleep = true;

            // Reset full refresh flag (we haven't done one yet this wake cycle)
            fullRefreshSinceWake = false;

            // Determine if we should refresh the display
            bool shouldRefreshDisplay = false;

            // Check if power state changed during sleep
            BatteryStatus batteryStatus = getBatteryStatus();
            bool isOnBatteryNow = !batteryStatus.isUSBPowered;
            bool powerStateChanged = (displayShowingBattery != isOnBatteryNow);

            // Set flag if we woke on battery (used for Bug #2 fix - different timeouts)
            wokeOnBattery = isOnBatteryNow;
            if (wokeOnBattery) {
                // Initialize battery mode timing - start counting from wake (millis() = 0)
                rtcBatteryModeStartTime = 0;
                rtcWasPreviouslyOnBattery = true;
                logMessage(LOG_DEBUG, "POWER", "Woke on battery - initialized timing, will use MAX_BATTERY_RUNTIME_MS (20s) timeout");
            } else {
                // Woke on USB - clear battery mode state
                rtcBatteryModeStartTime = 0;
                rtcWasPreviouslyOnBattery = false;
                logMessage(LOG_DEBUG, "POWER", "Woke on USB - no timeout applied");
            }

            if (cfg.display_policy.skip_refresh_on_no_message) {
                // Smart refresh strategy
                if (!lastReaction.hasReaction) {
                    // First boot - always refresh
                    shouldRefreshDisplay = true;
                    logMessage(LOG_INFO, "DISPLAY", "First boot - will show blank screen with status bar");
                } else if (powerStateChanged) {
                    // Power state changed during sleep - refresh to update display
                    shouldRefreshDisplay = true;
                    char logBuf[128];
                    snprintf(logBuf, sizeof(logBuf), "power_changed from_%s to_%s",
                             displayShowingBattery ? "battery" : "usb",
                             isOnBatteryNow ? "battery" : "usb");
                    logMessage(LOG_INFO, "DISPLAY", "Power state changed - updating display", logBuf);
                } else {
                    // No change - skip refresh (optimization)
                    logMessage(LOG_INFO, "DISPLAY", "Preserving display - will update on new message only");
                }
            } else {
                // Legacy behavior: Always refresh on wake
                shouldRefreshDisplay = true;
                logMessage(LOG_INFO, "DISPLAY", "Legacy mode - always refresh on wake");
            }

            // Perform display refresh if needed
            if (shouldRefreshDisplay && display) {
                if (lastReaction.hasReaction) {
                    // Redraw last reaction from RTC memory
                    logMessage(LOG_INFO, "RTC", "Redrawing last reaction from RTC memory");

                    JsonDocument doc;
                    doc["emoji"] = lastReaction.emoji;
                    doc["emoji_url"] = lastReaction.emojiUrl;
                    doc["user"] = lastReaction.user;
                    doc["channel"] = lastReaction.channel;
                    doc["message"] = lastReaction.message;
                    doc["timestamp"] = "";  // No timestamp on redraw
                    doc["encrypted"] = lastReaction.isEncrypted;

                    DisplayManager::showReaction(doc.as<JsonObject>());
                    logMessage(LOG_INFO, "DISPLAY", "Successfully restored reaction after wake");
                } else {
                    // No saved reaction - show blank screen with just top bar
                    logMessage(LOG_INFO, "RTC", "No saved reaction - showing blank screen with status bar");

                    display->setFullWindow();
                    display->firstPage();
                    do {
                        // Clear entire screen
                        display->fillScreen(GxEPD_WHITE);

                        // Draw top bar elements
                        display->setTextColor(GxEPD_BLACK);

                        // Draw battery indicator in top-right
                        DisplayManager::drawBatteryIndicator();

                        // Draw "BATTERY" text in center if on battery power
                        DisplayManager::drawPowerStatusIndicator();

                        // Draw lock icon in top-left (unlocked state by default)
                        DisplayManager::drawUnlockIcon();

                    } while (display->nextPage());

                    logMessage(LOG_INFO, "DISPLAY", "Blank screen with status bar displayed");
                }

                // Mark that full refresh has happened and update display state
                fullRefreshSinceWake = true;
                displayShowingBattery = isOnBatteryNow;

                char stateBuf[64];
                snprintf(stateBuf, sizeof(stateBuf), "fullRefresh=true displayShowingBattery=%s",
                         displayShowingBattery ? "true" : "false");
                logMessage(LOG_DEBUG, "DISPLAY", "Display state updated after refresh", stateBuf);
            }
        }

        // Timezone sync logic (after WiFi connected)
        bool isPowerOnBoot = (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED);

        if (shouldSyncTimezone(isPowerOnBoot)) {
            char syncBuf[128];
            if (!hasEverSynced) {
                snprintf(syncBuf, sizeof(syncBuf), "first_sync_attempt wakes=%d", wakesSinceTimeSync);
                logMessage(LOG_INFO, "TIME", "Timezone sync needed (never synced)", syncBuf);
            } else {
                snprintf(syncBuf, sizeof(syncBuf), "wakes=%d", wakesSinceTimeSync);
                logMessage(LOG_INFO, "TIME", "Timezone sync needed (scheduled)", syncBuf);
            }

            if (fetchTimezoneFromAPI()) {
                // Sync successful
                char timeBuf[64];
                int hour = getCurrentLocalHour();
                snprintf(timeBuf, sizeof(timeBuf), "current_hour=%d quiet_hours=%s",
                        hour, isQuietHours() ? "yes" : "no");
                logMessage(LOG_INFO, "TIME", "Timezone sync complete", timeBuf);
            } else {
                // Sync failed - increment counter anyway so we don't retry every wake
                wakesSinceTimeSync++;

                if (!hasEverSynced) {
                    snprintf(syncBuf, sizeof(syncBuf), "next_retry_in=%d_wakes (~%d_min)",
                            12 - (wakesSinceTimeSync % 12),
                            (12 - (wakesSinceTimeSync % 12)) * cfg.power.sleep_duration_min);
                    logMessage(LOG_WARN, "TIME", "CRITICAL: First timezone sync failed - quiet hours disabled until success", syncBuf);
                } else {
                    logMessage(LOG_WARN, "TIME", "Timezone sync failed, will retry at next interval");
                }
            }
        } else {
            // Update estimated time and increment wake counter
            updateEstimatedTime();
            wakesSinceTimeSync++;

            char timeBuf[64];
            int hour = getCurrentLocalHour();
            snprintf(timeBuf, sizeof(timeBuf),
                    "wakes_since_sync=%d next_sync_in=%d current_hour=%d",
                    wakesSinceTimeSync,
                    (cfg.timezone.sync_interval_hours * 12) - wakesSinceTimeSync,
                    hour);
            logMessage(LOG_DEBUG, "TIME", "Using estimated time", timeBuf);
        }

        // Small delay before WebSocket connection
        delay(2000);
        NetworkManager::connectWebSocket();

        // Initialize OTA manager (requires network connectivity)
        logMessage(LOG_INFO, "OTA", "Initializing OTA manager");

        // Build server URL from config components
        String serverUrl = String(cfg.server.use_ssl ? "https://" : "http://") +
                          cfg.server.host + ":" + String(cfg.server.port);

        otaManager = new OTAManager(serverUrl, cfg.device.id);

        // Check boot validation (mark new firmware as valid if just updated)
        if (otaManager->checkBootValidation()) {
            logMessage(LOG_INFO, "OTA", "Boot validation successful - new firmware marked as valid");
        }

        // Check for firmware updates on power-on boot only (not wake from sleep)
        // This prevents unnecessary battery drain from checking on every wake cycle
        // Updates are primarily delivered via WebSocket push notifications
        if (!isWakeFromSleep) {
            logMessage(LOG_INFO, "OTA", "Power-on boot - checking for firmware updates");
            checkForFirmwareUpdate();
            lastOTACheckMillis = millis();
        } else {
            logMessage(LOG_INFO, "OTA", "Wake from sleep - skipping OTA check (updates delivered via WebSocket)");
        }
    }

    logMessage(LOG_INFO, "SYSTEM", "Setup complete");

    #ifdef ENABLE_DEBUG_FEATURES
    Serial.println(F("\n=== Debug Commands Available ==="));
    Serial.println(F("TEST:WIFI - Test WiFi status"));
    Serial.println(F("TEST:WS - Test WebSocket status"));
    Serial.println(F("TEST:MSG:{json} - Inject test message"));
    Serial.println(F("TEST:DISPLAY:text - Display test"));
    Serial.println(F("TEST:CONFIG - Show configuration"));
    Serial.println(F("TEST:METRICS - Show metrics"));
    Serial.println(F("TEST:LOCK:ICONS - Test lock/unlock icons"));
    Serial.println(F("TEST:SHUTDOWN - Test graceful shutdown"));
    Serial.println(F("TEST:SHUTDOWN:CLEAR - Test shutdown with clear"));
    Serial.println(F("LOG:LEVEL:ERROR/WARN/INFO/DEBUG/TEST - Set log level"));
    Serial.println(F("================================\n"));
    #endif
}

// ============================================================================
// Arduino Main Loop
// ============================================================================
void loop() {
    // Handle WebSocket communication
    if (WiFi.status() == WL_CONNECTED) {
        webSocket.loop();
    }

    // Handle reconnection logic
    NetworkManager::handleReconnection();

    // Check connection health and process queued messages
    ResilienceManager::checkHealth();

    // Process queued messages if connection is restored
    if (wsConnected && ResilienceManager::hasQueuedMessages()) {
        ResilienceManager::processQueuedMessages();
    }

    // OTA Update Management
    // Periodic check every 24 hours (only for devices that stay awake continuously on USB)
    // For devices using deep sleep, updates are delivered via WebSocket push or on power-on boot
    // millis() overflow-safe: subtraction works correctly even after 49 days
    if (otaManager && (millis() - lastOTACheckMillis) > 24UL * 60UL * 60UL * 1000UL) {
        logMessage(LOG_INFO, "OTA", "24 hours since last check - checking for updates");
        checkForFirmwareUpdate();
        lastOTACheckMillis = millis();
    }

    // Install pending update when device is idle
    if (otaManager && pendingUpdate && isDeviceIdle()) {
        performOTAUpdate();
        pendingUpdate = false;
    }

    #ifdef ENABLE_DEBUG_FEATURES
    // Process serial commands
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (commandBuffer.length() > 0) {
                processSerialCommand(commandBuffer);
                commandBuffer = "";
            }
        } else {
            commandBuffer += c;
        }
    }
    #endif

    // Power management - check if we should sleep
    using namespace BatteryConstants;
    const AppConfig& cfg = ConfigManager::getConfig();
    if (cfg.power.sleep_enabled) {
        static unsigned long lastPowerCheck = 0;
        unsigned long now = millis();

        // Use RTC memory for battery state (survives deep sleep)
        unsigned long& batteryModeStartTime = rtcBatteryModeStartTime;
        bool& wasPreviouslyOnBattery = rtcWasPreviouslyOnBattery;

        // Check power status every 10 seconds (rollover-safe comparison)
        if ((unsigned long)(now - lastPowerCheck) > 10000) {
            lastPowerCheck = now;

            // Call getBatteryStatus() ONCE and reuse the result (avoid multiple 50ms ADC samplings)
            BatteryStatus batteryStatus = getBatteryStatus();
            bool isOnBattery = !batteryStatus.isUSBPowered;
            int batteryPct = batteryStatus.percentage;

            // Check for critically low battery - sleep immediately (bypass all other logic)
            if (isOnBattery && batteryPct >= 0 && batteryPct < LOW_BATTERY_SLEEP_THRESHOLD) {
                char logBuf[128];
                snprintf(logBuf, sizeof(logBuf), "battery=%d%% voltage=%.2fV - sleeping immediately",
                         batteryPct, batteryStatus.voltage);  // Use cached voltage
                logMessage(LOG_WARN, "POWER", "Critical low battery", logBuf);
                enterDeepSleep(calculateSleepDuration());
                return;  // Never reached, but for clarity
            }

            // Track when we first switched to battery mode
            if (isOnBattery && !wasPreviouslyOnBattery) {
                batteryModeStartTime = now;
                wasPreviouslyOnBattery = true;
                // Note: Don't modify wokeOnBattery here - it's only set during wake from sleep
                logMessage(LOG_INFO, "POWER", "Switched to battery mode (USB unplugged)", "grace_period=60s");

                // Update display to show "BATTERY" text at top
                if (fullRefreshSinceWake && display) {
                    // Safe for partial refresh - controller RAM is synced
                    display->setPartialWindow(0, 0, display->width(), 15);
                    display->firstPage();
                    do {
                        display->fillRect(0, 0, display->width(), 15, GxEPD_WHITE);  // Clear top bar
                        DisplayManager::drawBatteryIndicator();
                        DisplayManager::drawPowerStatusIndicator();
                        // Redraw lock icon if encryption enabled
                        if (cfg.security.use_aes && !cfg.security.aes_key.isEmpty()) {
                            DisplayManager::drawLockIcon();
                        }
                    } while (display->nextPage());
                    displayShowingBattery = true;
                    logMessage(LOG_DEBUG, "DISPLAY", "Partial refresh: added BATTERY text");
                } else if (display) {
                    // NOT safe for partial - need full refresh
                    logMessage(LOG_INFO, "DISPLAY", "Full refresh required - redrawing saved reaction with BATTERY");
                    if (lastReaction.hasReaction) {
                        JsonDocument doc;
                        doc["emoji"] = lastReaction.emoji;
                        doc["emoji_url"] = lastReaction.emojiUrl;
                        doc["user"] = lastReaction.user;
                        doc["channel"] = lastReaction.channel;
                        doc["message"] = lastReaction.message;
                        doc["timestamp"] = "";
                        doc["encrypted"] = lastReaction.isEncrypted;
                        DisplayManager::showReaction(doc.as<JsonObject>());
                    } else {
                        // Show blank screen with BATTERY
                        display->setFullWindow();
                        display->firstPage();
                        do {
                            display->fillScreen(GxEPD_WHITE);
                            display->setTextColor(GxEPD_BLACK);
                            DisplayManager::drawBatteryIndicator();
                            DisplayManager::drawPowerStatusIndicator();
                            DisplayManager::drawUnlockIcon();
                        } while (display->nextPage());
                    }
                    fullRefreshSinceWake = true;
                    displayShowingBattery = true;
                    logMessage(LOG_DEBUG, "DISPLAY", "Full refresh: now showing BATTERY text");
                }

            } else if (!isOnBattery && wasPreviouslyOnBattery) {
                wasPreviouslyOnBattery = false;
                batteryModeStartTime = 0;  // Reset timer when switching to USB
                logMessage(LOG_INFO, "POWER", "Switched to USB mode", "");

                // Update display to remove "BATTERY" text (top bar shows nothing on USB)
                if (fullRefreshSinceWake && display) {
                    // Safe for partial refresh - controller RAM is synced
                    display->setPartialWindow(0, 0, display->width(), 15);
                    display->firstPage();
                    do {
                        display->fillRect(0, 0, display->width(), 15, GxEPD_WHITE);  // Clear top bar
                        DisplayManager::drawBatteryIndicator();
                        DisplayManager::drawPowerStatusIndicator();  // Shows nothing on USB
                        // Redraw lock icon if encryption enabled
                        if (cfg.security.use_aes && !cfg.security.aes_key.isEmpty()) {
                            DisplayManager::drawLockIcon();
                        }
                    } while (display->nextPage());
                    displayShowingBattery = false;
                    logMessage(LOG_DEBUG, "DISPLAY", "Partial refresh: removed BATTERY text");
                } else if (display) {
                    // NOT safe for partial - need full refresh
                    logMessage(LOG_INFO, "DISPLAY", "Full refresh required - redrawing saved reaction without BATTERY");
                    if (lastReaction.hasReaction) {
                        JsonDocument doc;
                        doc["emoji"] = lastReaction.emoji;
                        doc["emoji_url"] = lastReaction.emojiUrl;
                        doc["user"] = lastReaction.user;
                        doc["channel"] = lastReaction.channel;
                        doc["message"] = lastReaction.message;
                        doc["timestamp"] = "";
                        doc["encrypted"] = lastReaction.isEncrypted;
                        DisplayManager::showReaction(doc.as<JsonObject>());
                    } else {
                        // Show blank screen without BATTERY
                        display->setFullWindow();
                        display->firstPage();
                        do {
                            display->fillScreen(GxEPD_WHITE);
                            display->setTextColor(GxEPD_BLACK);
                            DisplayManager::drawBatteryIndicator();
                            DisplayManager::drawPowerStatusIndicator();
                            DisplayManager::drawUnlockIcon();
                        } while (display->nextPage());
                    }
                    fullRefreshSinceWake = true;
                    displayShowingBattery = false;
                    logMessage(LOG_DEBUG, "DISPLAY", "Full refresh: now NOT showing BATTERY text");
                }
            }

            if (isOnBattery) {
                // Calculate time on battery (rollover-safe)
                unsigned long timeOnBattery = (unsigned long)(now - batteryModeStartTime);

                // Bug #2 Fix: Different timeouts for different scenarios
                // - Woke on battery (batteryModeStartTime was 0): Use short timeout (20s)
                // - USB unplugged (batteryModeStartTime was set during runtime): Use grace period (60s)
                unsigned long sleepTimeout;
                const char* timeoutReason;

                // Check if we woke on battery: batteryModeStartTime will be 0 and wokeOnBattery true
                // After USB unplug: batteryModeStartTime will be > 0 (set to current millis)
                bool isWakeOnBattery = (wokeOnBattery && wasPreviouslyOnBattery && batteryModeStartTime == 0);

                if (isWakeOnBattery) {
                    // Woke from sleep on battery - use short timeout for battery optimization
                    sleepTimeout = MAX_BATTERY_RUNTIME_MS;  // 20s
                    timeoutReason = "wake_on_battery";
                } else {
                    // USB was unplugged while awake - use grace period for stability
                    sleepTimeout = GRACE_PERIOD_MS;  // 60s
                    timeoutReason = "usb_unplugged";
                }

                if (timeOnBattery < sleepTimeout) {
                    char logBuf[128];
                    snprintf(logBuf, sizeof(logBuf), "scenario=%s timeout=%lus time_on_battery=%lus",
                             timeoutReason, sleepTimeout / 1000, timeOnBattery / 1000);
                    logMessage(LOG_DEBUG, "POWER", "Battery mode - within timeout", logBuf);
                } else {
                    // Timeout exceeded, apply sleep logic
                    bool shouldSleep = false;
                    const char* sleepReason = nullptr;

                    // Sleep if no WebSocket connection (already past timeout)
                    if (!wsConnected) {
                        shouldSleep = true;
                        sleepReason = "no_connection";
                    }
                    // Sleep if timeout exceeded
                    else {
                        shouldSleep = true;
                        sleepReason = timeoutReason;
                    }

                    if (shouldSleep) {
                        char logBuf[128];
                        snprintf(logBuf, sizeof(logBuf), "reason=%s time_on_battery=%lus battery=%d%% wsConnected=%s",
                                 sleepReason, timeOnBattery / 1000, batteryPct, wsConnected ? "true" : "false");
                        logMessage(LOG_INFO, "POWER", "Battery mode sleep triggered", logBuf);
                        enterDeepSleep(calculateSleepDuration());
                    }
                }
            } else {
                logMessage(LOG_DEBUG, "POWER", "USB powered - staying awake");
            }
        }
    } else {
        static bool loggedOnce = false;
        if (!loggedOnce) {
            logMessage(LOG_INFO, "POWER", "Sleep disabled in config");
            loggedOnce = true;
        }
    }

    // Small delay to prevent tight looping
    delay(10);
}

// ============================================================================
// OTA Helper Functions
// ============================================================================

/**
 * Check for firmware updates from server
 * Called on boot and periodically every 24 hours
 */
void checkForFirmwareUpdate() {
    if (!otaManager) {
        logMessage(LOG_ERROR, "OTA", "OTA manager not initialized");
        return;
    }

    logMessage(LOG_INFO, "OTA", "Checking for firmware updates");

    OTAManager::FirmwareInfo info;
    if (otaManager->checkForUpdate(info)) {
        char logBuf[256];
        snprintf(logBuf, sizeof(logBuf), "version=%s size=%d required=%s",
                 info.version.c_str(), info.size, info.required ? "true" : "false");
        logMessage(LOG_INFO, "OTA", "Update available", logBuf);

        if (info.required) {
            logMessage(LOG_WARN, "OTA", "Required update - installing immediately");
            performOTAUpdate();
        } else {
            logMessage(LOG_INFO, "OTA", "Optional update - will install when idle");
            pendingUpdate = true;
            pendingVersion = info.version;
        }
    } else {
        if (otaManager->getStatus() == OTAManager::OTAStatus::FAILED) {
            logMessage(LOG_ERROR, "OTA", "Update check failed", otaManager->getLastError().c_str());
        } else {
            logMessage(LOG_INFO, "OTA", "No updates available");
        }
    }
}

/**
 * Perform OTA update
 * Downloads and installs firmware, then reboots device
 */
void performOTAUpdate() {
    if (!otaManager) {
        logMessage(LOG_ERROR, "OTA", "OTA manager not initialized");
        return;
    }

    logMessage(LOG_INFO, "OTA", "Starting firmware update");

    // Show update message on display
    if (display) {
        DisplayManager::showMessage("Firmware Update", "Downloading...", "Please wait");
    }

    // Get firmware info
    OTAManager::FirmwareInfo info;
    if (!otaManager->checkForUpdate(info)) {
        logMessage(LOG_ERROR, "OTA", "Failed to get firmware info", otaManager->getLastError().c_str());
        if (display) {
            DisplayManager::showMessage("Update Failed", "Cannot get firmware info");
        }
        return;
    }

    // Download and install with progress callback
    bool success = otaManager->downloadAndInstall(info, [](size_t current, size_t total) {
        // Progress callback - update display every 10%
        static int lastPercent = -1;
        int percent = (current * 100) / total;
        if (percent != lastPercent && percent % 10 == 0) {
            lastPercent = percent;
            char buf[32];
            snprintf(buf, sizeof(buf), "%d%%", percent);
            logMessage(LOG_INFO, "OTA", "Download progress", buf);

            if (display) {
                char progressMsg[64];
                snprintf(progressMsg, sizeof(progressMsg), "Installing... %d%%", percent);
                DisplayManager::showMessage("Firmware Update", progressMsg, "Please wait");
            }
        }
    });

    if (success) {
        logMessage(LOG_INFO, "OTA", "Firmware update successful - rebooting");
        if (display) {
            DisplayManager::showMessage("Update Complete", "Rebooting...");
        }
        delay(2000);
        ESP.restart();  // Reboot to new firmware
    } else {
        logMessage(LOG_ERROR, "OTA", "Firmware update failed", otaManager->getLastError().c_str());
        if (display) {
            DisplayManager::showMessage("Update Failed", otaManager->getLastError().c_str());
        }
    }
}

/**
 * Check if device is idle (no reactions for 5 minutes)
 * Used to determine when it's safe to perform optional updates
 */
bool isDeviceIdle() {
    const unsigned long IDLE_THRESHOLD_MS = 5UL * 60UL * 1000UL;  // 5 minutes

    // If no reactions yet, consider idle
    if (lastReactionTime == 0) {
        return true;
    }

    // Check if enough time has passed since last reaction
    unsigned long timeSinceReaction = millis() - lastReactionTime;
    return timeSinceReaction > IDLE_THRESHOLD_MS;
}