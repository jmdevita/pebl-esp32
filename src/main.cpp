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
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <LittleFS.h>
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
RTC_DATA_ATTR time_t lastTimeSyncTimestamp = 0;     // Unix timestamp of last sync
RTC_DATA_ATTR int timezoneOffsetSeconds = 0;        // Timezone offset in seconds (includes DST)
RTC_DATA_ATTR time_t currentTime = 0;               // Current time (read from ESP32 RTC each wake)
RTC_DATA_ATTR bool hasEverSynced = false;           // True if we've successfully synced at least once

// ============================================================================
// Debug Configuration
// ============================================================================
// #define ENABLE_DEBUG_FEATURES  // Uncomment for debugging/testing

// ============================================================================
// Connection Timing Constants
// ============================================================================
// These are hardcoded to match server protocol and prevent user misconfiguration
namespace ConnectionTiming {
    constexpr uint32_t HEARTBEAT_INTERVAL_MS = 15000;      // Expect heartbeat every 15s
    constexpr uint32_t HEARTBEAT_TIMEOUT_MS = 30000;       // Consider connection dead after 30s
    constexpr uint32_t WS_INITIAL_RECONNECT_MS = 15000;    // Wait 15s before first retry
    constexpr uint32_t WS_MAX_RECONNECT_MS = 60000;        // Max 60s between retries (exponential backoff)
}

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
// Uses single Serial.println() for atomic output to prevent corruption from interrupts
void logMessage(LogLevel level, const char* module, const char* message, const char* kvPairs = nullptr) {
    if (level > currentLogLevel) return;

    const char* levelStr[] = {"ERROR", "WARN", "INFO", "DEBUG", "TEST"};

    // Pre-format entire log message into buffer for atomic output
    // This prevents UART corruption when interrupts (ADC, SPI, WiFi) fire mid-transmission
    char logBuf[256];

    if (kvPairs) {
        snprintf(logBuf, sizeof(logBuf), "[%lu][%s][%s] %s | %s",
                millis(), levelStr[level], module, message, kvPairs);
    } else {
        snprintf(logBuf, sizeof(logBuf), "[%lu][%s][%s] %s",
                millis(), levelStr[level], module, message);
    }

    // Single atomic write prevents corruption from concurrent interrupts
    Serial.println(logBuf);

    // Flush output buffer to ensure transmission completes before potential interrupt
    Serial.flush();
}

// Overload for ConfigManager compatibility
void logMessage(int level, const char* module, const char* message, const char* kvPairs = nullptr) {
    logMessage(static_cast<LogLevel>(level), module, message, kvPairs);
}

// ============================================================================
// Configuration - Now loaded from LittleFS via ConfigManager
// ============================================================================
// Display text limits (keeping as constexpr since they're compile-time UI constants)
namespace DisplayLimits {
    constexpr size_t MAX_USER_LENGTH = 15;
    constexpr size_t MAX_CHANNEL_LENGTH = 15;
    constexpr size_t MAX_MESSAGE_LENGTH = 30;

    // Emoji position: leaves space for lock icon (top-left) and battery indicator (top-right)
    constexpr int16_t EMOJI_X = 10;  // 10px from left
    constexpr int16_t EMOJI_Y = 32;  // 32px from top (optimized for balanced layout)
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

    // Registration error retry tracking (resets on boot/wake, NOT persisted in RTC)
    uint8_t registrationErrorCount = 0;
    const uint8_t MAX_REGISTRATION_RETRIES = 3;
    bool registrationFailedPermanently = false;  // Stop reconnecting after max retries

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
RTC_DATA_ATTR bool hasShownBlankScreen = false;  // Track if we've shown blank screen at least once
RTC_DATA_ATTR bool hasShownLowBatteryWarning = false;  // Track if low battery warning was shown (resets on USB)

// Track if device woke from sleep on battery (survives deep sleep)
// Used to differentiate between two battery scenarios with different timeout behaviors:
// - Wake on battery: Device was already unplugged before sleep (use 20s timeout for efficiency)
// - USB unplugged: Transitioned from USB→battery while awake (use 60s grace period for stability)
RTC_DATA_ATTR bool wokeOnBattery = false;

// ADC calibration handle for accurate battery voltage readings
// Uses factory-burned eFuse calibration data to correct ESP32 ADC non-linearity
adc_cali_handle_t adc_cali_handle = nullptr;

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

        // Server handles GIF→PNG conversion, client just downloads whatever URL provided
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
        // Log the URL being downloaded for debugging certificate issues
        char urlBuf[128];
        snprintf(urlBuf, sizeof(urlBuf), "url=%.120s", url);
        logMessage(LOG_DEBUG, "EMOJI", "Downloading emoji", urlBuf);

        // Allocate WiFiClientSecure on heap to avoid stack overflow
        // SSL contexts consume ~5-6KB which exceeds default Arduino loop stack (8KB)
        WiFiClientSecure* secureClient = new WiFiClientSecure();
        if (!secureClient) {
            logMessage(LOG_ERROR, "EMOJI", "Failed to allocate SSL client");
            return false;
        }

        // Determine if URL is from Slack CDN or our server
        bool isSlackCDN = (strstr(url, "slack-edge.com") != nullptr ||
                          strstr(url, "slack.com") != nullptr);

        if (isSlackCDN) {
            // Configure HTTPS with Mozilla CA certificate bundle (covers all Slack CDN domains)
            extern const uint8_t rootca_crt_bundle_start[] asm("_binary_certs_x509_crt_bundle_bin_start");
            extern const uint8_t rootca_crt_bundle_end[] asm("_binary_certs_x509_crt_bundle_bin_end");
            size_t bundle_size = rootca_crt_bundle_end - rootca_crt_bundle_start;
            secureClient->setCACertBundle(rootca_crt_bundle_start, bundle_size);
            logMessage(LOG_DEBUG, "EMOJI", "Using CA bundle for Slack CDN");
        } else {
            // WORKAROUND: Skip certificate validation for server domain (same as OTA/WebSocket)
            // See docs/SSL_CERT_VALIDATION_ISSUE.md for details on ECDSA validation failure
            //
            // Security rationale:
            // - Connection is still encrypted with TLS 1.3 (not plaintext)
            // - Server validates emoji source from authenticated Slack CDN
            // - Client validates PNG magic bytes after download (prevents malformed data)
            // - Low-value data (emoji images) on trusted home network
            //
            // This matches the security model used for OTA updates and WebSocket connections.
            secureClient->setInsecure();
            logMessage(LOG_DEBUG, "EMOJI", "Skipping cert check (encrypted + validated, trusted network)");
        }

        // Allocate HTTPClient on heap to avoid stack overflow
        HTTPClient* http = new HTTPClient();
        if (!http) {
            logMessage(LOG_ERROR, "EMOJI", "Failed to allocate HTTP client");
            delete secureClient;
            return false;
        }

        http->setConnectTimeout(5000);  // 5 second timeout
        http->setTimeout(10000);         // 10 second total timeout
        http->setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        if (!http->begin(*secureClient, url)) {
            logMessage(LOG_ERROR, "EMOJI", "HTTPS begin failed");
            delete http;
            delete secureClient;
            return false;
        }

        int httpCode = http->GET();
        if (httpCode != HTTP_CODE_OK) {
            char logBuf[64];
            snprintf(logBuf, sizeof(logBuf), "http_code=%d", httpCode);
            logMessage(LOG_ERROR, "EMOJI", "HTTP GET failed", logBuf);
            http->end();
            delete http;
            delete secureClient;
            return false;
        }

        int len = http->getSize();
        if (len <= 0 || len > MAX_EMOJI_SIZE) {
            char logBuf[64];
            snprintf(logBuf, sizeof(logBuf), "invalid_size=%d", len);
            logMessage(LOG_ERROR, "EMOJI", "Invalid content size", logBuf);
            http->end();
            delete http;
            delete secureClient;
            return false;
        }

        // Allocate buffer for download
        downloadCapacity = len;
        downloadBuffer = (uint8_t*)malloc(downloadCapacity);
        if (!downloadBuffer) {
            logMessage(LOG_ERROR, "EMOJI", "Failed to allocate download buffer");
            http->end();
            delete http;
            delete secureClient;
            return false;
        }

        // Read response into buffer with timeout protection
        WiFiClient* stream = http->getStreamPtr();
        if (!stream) {
            logMessage(LOG_ERROR, "EMOJI", "Failed to get stream pointer");
            free(downloadBuffer);
            downloadBuffer = nullptr;
            http->end();
            delete http;
            delete secureClient;
            return false;
        }

        downloadSize = 0;
        unsigned long downloadStart = millis();

        while (http->connected() && downloadSize < downloadCapacity) {
            // Timeout check: 10 second maximum for download
            if ((unsigned long)(millis() - downloadStart) > 10000) {
                char logBuf[64];
                snprintf(logBuf, sizeof(logBuf), "downloaded=%zu/%zu bytes", downloadSize, downloadCapacity);
                logMessage(LOG_ERROR, "EMOJI", "Download timeout", logBuf);
                free(downloadBuffer);
                downloadBuffer = nullptr;
                http->end();
                delete http;
                delete secureClient;
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

        http->end();
        delete http;
        delete secureClient;

        // Verify we got complete download
        if (downloadSize != downloadCapacity) {
            char logBuf[128];
            snprintf(logBuf, sizeof(logBuf), "expected=%zu got=%zu", downloadCapacity, downloadSize);
            logMessage(LOG_ERROR, "EMOJI", "Incomplete download", logBuf);
            free(downloadBuffer);
            downloadBuffer = nullptr;
            return false;  // Don't process incomplete data
        }

        // Validate PNG file format (magic bytes: 0x89 PNG \r \n 0x1a \n)
        // Protects against malformed or malicious files from compromised CDN
        if (downloadSize < 8 ||
            downloadBuffer[0] != 0x89 || downloadBuffer[1] != 0x50 ||
            downloadBuffer[2] != 0x4E || downloadBuffer[3] != 0x47 ||
            downloadBuffer[4] != 0x0D || downloadBuffer[5] != 0x0A ||
            downloadBuffer[6] != 0x1A || downloadBuffer[7] != 0x0A) {
            logMessage(LOG_ERROR, "EMOJI", "Invalid PNG file (bad magic bytes)");
            free(downloadBuffer);
            downloadBuffer = nullptr;
            return false;
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

        // OPTIMIZATION OPPORTUNITY: Currently calls getBatteryStatus() multiple times per display update
        // (once here in drawPowerStatusIndicator, again in drawBatteryIndicator)
        // Better approach: Call getBatteryStatus() once in showReaction(), pass struct to both functions
        // Impact: Minor (~10ms savings on ESP32), but cleaner architecture

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

            // Text fallback: display emoji text as-is (server already formats with colons)
            // Server sends emoji field as ":emoji_name:" so no need to add extra colons
            if (!emojiRendered) {
                display->setFont(&FreeSans12pt7b);
                display->setCursor(10, 57);  // Aligned with balanced layout
                display->print(emoji);
            }

            // Reaction details - using smart truncation to prevent text wrapping
            // Calculate available width: display width - x position - right margin
            const int16_t rightMargin = 5;  // 5px margin from right edge

            // User name at top (bold font)
            display->setFont(&FreeSansBold9pt7b);
            int16_t userX = 60;
            int16_t userMaxWidth = display->width() - userX - rightMargin;
            String userStr = truncateToFit(user, &FreeSansBold9pt7b, userX, userMaxWidth);
            display->setCursor(userX, 42);  // Positioned for balanced layout
            display->print(userStr);

            // Message text below user name (italicized font, with small whitespace)
            if (message[0] != '\0') {
                display->setFont(&FreeSansOblique9pt7b);  // Use italicized font for message text
                int16_t messageX = 60;
                int16_t messageMaxWidth = display->width() - messageX - rightMargin;
                String messageStr = truncateToFit(message, &FreeSansOblique9pt7b, messageX, messageMaxWidth);
                display->setCursor(messageX, 62);  // Positioned for balanced layout with spacing
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
            display->setCursor(channelX, 92);  // Positioned closer to message content
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

    /**
     * Display low battery warning screen.
     * Shows large centered text asking user to charge the device.
     * Called when battery is critically low (<15%) before entering deep sleep.
     */
    static void showLowBatteryWarning() {
        if (!display) return;

        logMessage(LOG_WARN, "DISPLAY", "Showing low battery warning screen");

        // Full window refresh for critical warning
        display->setFullWindow();
        display->firstPage();

        do {
            display->fillScreen(GxEPD_WHITE);
            display->setTextColor(GxEPD_BLACK);

            // Draw battery indicator in top-right (showing low level)
            drawBatteryIndicator();

            // Main warning text: "LOW BATTERY" (large, centered)
            display->setFont(&FreeSans12pt7b);
            const char* line1 = "LOW BATTERY";

            int16_t x1, y1;
            uint16_t w1, h1;
            display->getTextBounds(line1, 0, 0, &x1, &y1, &w1, &h1);
            int16_t x1_pos = (display->width() - w1) / 2;
            int16_t y1_pos = (display->height() / 2) - 10;  // Slightly above center

            display->setCursor(x1_pos, y1_pos);
            display->print(line1);

            // Secondary text: "PLEASE CHARGE" (regular font, centered)
            display->setFont(&FreeSans9pt7b);
            const char* line2 = "PLEASE CHARGE";

            uint16_t w2, h2;
            display->getTextBounds(line2, 0, 0, &x1, &y1, &w2, &h2);
            int16_t x2_pos = (display->width() - w2) / 2;
            int16_t y2_pos = y1_pos + 25;  // 25px below first line

            display->setCursor(x2_pos, y2_pos);
            display->print(line2);

        } while (display->nextPage());

        // Update display state
        updateDisplayStateAfterFullRefresh();

        logMessage(LOG_INFO, "DISPLAY", "Low battery warning displayed");
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
};

// ============================================================================
// Display Hardware Initialization Helper
// ============================================================================

/**
 * Initialize display hardware (SPI + display driver)
 * Can be called multiple times safely (checks if display already initialized)
 * Used by both normal setup() and long-press config mode entry
 */
void initializeDisplayHardware() {
    if (display != nullptr) {
        logMessage(LOG_DEBUG, "DISPLAY", "Already initialized - skipping");
        return;  // Already initialized
    }

    const AppConfig& cfg = ConfigManager::getConfig();

    // Pin definitions
    constexpr uint8_t PIN_DISPLAY_SCLK = 18;
    constexpr uint8_t PIN_DISPLAY_MOSI = 23;
    constexpr uint8_t PIN_DISPLAY_CS = 5;
    constexpr uint8_t PIN_DISPLAY_DC = 17;
    constexpr uint8_t PIN_DISPLAY_RST = 16;
    constexpr uint8_t PIN_DISPLAY_BUSY = 4;

    // Configure SPI pins
    SPI.begin(PIN_DISPLAY_SCLK, -1, PIN_DISPLAY_MOSI, PIN_DISPLAY_CS);

    // Create display instance based on build flags
    #ifdef DISPLAY_4G_GRAYSCALE
        #if defined(DISPLAY_WIDTH) && DISPLAY_WIDTH == 212
            #ifdef DISPLAY_GDEW0213I5F
                display = new GxEPD2_4G_4G<GxEPD2_213_flex, GxEPD2_213_flex::HEIGHT>(
                    GxEPD2_213_flex(PIN_DISPLAY_CS, PIN_DISPLAY_DC, PIN_DISPLAY_RST, PIN_DISPLAY_BUSY)
                );
                logMessage(LOG_INFO, "DISPLAY", "Driver: 2.13\" 4G grayscale (GDEW0213I5F)");
            #else
                display = new GxEPD2_4G_4G<GxEPD2_213_GDEY0213B74, GxEPD2_213_GDEY0213B74::HEIGHT>(
                    GxEPD2_213_GDEY0213B74(PIN_DISPLAY_CS, PIN_DISPLAY_DC, PIN_DISPLAY_RST, PIN_DISPLAY_BUSY)
                );
                logMessage(LOG_INFO, "DISPLAY", "Driver: 2.13\" 4G grayscale (GDEY0213B74)");
            #endif
        #endif
    #else
        // 2-level black & white displays
        #if defined(DISPLAY_WIDTH) && DISPLAY_WIDTH == 264 && defined(DISPLAY_HEIGHT) && DISPLAY_HEIGHT == 176
            display = new GxEPD2_BW<GxEPD2_270, GxEPD2_270::HEIGHT>(
                GxEPD2_270(PIN_DISPLAY_CS, PIN_DISPLAY_DC, PIN_DISPLAY_RST, PIN_DISPLAY_BUSY)
            );
            logMessage(LOG_INFO, "DISPLAY", "Driver: 2.7\" BW (GxEPD2_270)");
        #elif defined(DISPLAY_WIDTH) && DISPLAY_WIDTH == 212
            #ifdef DISPLAY_GDEW0213T5D
                display = new GxEPD2_BW<GxEPD2_213_T5D, GxEPD2_213_T5D::HEIGHT>(
                    GxEPD2_213_T5D(PIN_DISPLAY_CS, PIN_DISPLAY_DC, PIN_DISPLAY_RST, PIN_DISPLAY_BUSY)
                );
                logMessage(LOG_INFO, "DISPLAY", "Driver: 2.13\" BW (GxEPD2_213_T5D)");
            #else
                display = new GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT>(
                    GxEPD2_213_B74(PIN_DISPLAY_CS, PIN_DISPLAY_DC, PIN_DISPLAY_RST, PIN_DISPLAY_BUSY)
                );
                logMessage(LOG_INFO, "DISPLAY", "Driver: 2.13\" BW (GxEPD2_213_B74)");
            #endif
        #else
            // Default to 2.13" BW if dimensions not specified
            display = new GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT>(
                GxEPD2_213_B74(PIN_DISPLAY_CS, PIN_DISPLAY_DC, PIN_DISPLAY_RST, PIN_DISPLAY_BUSY)
            );
            logMessage(LOG_INFO, "DISPLAY", "Driver: 2.13\" BW (default)");
        #endif
    #endif

    // Initialize display
    bool fromDeepSleep = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);
    display->init(115200, !fromDeepSleep, 2, false);
    display->setRotation(cfg.display.rotation);

    char buf[128];
    snprintf(buf, sizeof(buf), "rotation=%d", cfg.display.rotation);
    logMessage(LOG_INFO, "DISPLAY", "Initialized", buf);
}

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

/**
 * Initialize ADC calibration using factory eFuse data
 *
 * Corrects ESP32 ADC non-linearity for accurate battery voltage readings.
 * ESP32 ADC has significant non-linearity (up to 5-8% error at certain voltages)
 * especially in the 3.0-4.2V battery range. Factory calibration data stored in
 * eFuse corrects this using a curve-fitting algorithm.
 *
 * Impact: Improves battery % accuracy from ±10% to ±2-3%, particularly at
 * critical thresholds (low battery detection, USB detection).
 *
 * Should be called once during setup() before first battery reading.
 */
void setupADCCalibration() {
    // Prevent double initialization (defensive programming)
    // Handle should be nullptr on boot/wake, but check anyway
    if (adc_cali_handle != nullptr) {
        logMessage(LOG_DEBUG, "ADC", "Calibration already initialized - skipping");
        return;
    }

    // Create calibration configuration for ADC1, 12dB attenuation (0-3.9V range)
    // Matches the attenuation used in getBatteryStatus() for battery monitoring
    // Note: ESP32 (original) uses line_fitting, ESP32-S2/S3/C3 use curve_fitting
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,              // Using ADC1 (GPIO 35/36)
        .atten = ADC_ATTEN_DB_12,           // 12dB attenuation (0-3.9V range)
        .bitwidth = ADC_BITWIDTH_12,        // 12-bit resolution (0-4095)
    };

    // Create calibration scheme using factory eFuse data
    esp_err_t ret = adc_cali_create_scheme_line_fitting(&cali_config, &adc_cali_handle);

    if (ret == ESP_OK) {
        logMessage(LOG_INFO, "ADC", "Calibration initialized using eFuse data");
    } else if (ret == ESP_ERR_NOT_SUPPORTED) {
        // Some ESP32 chips don't have eFuse calibration data burned
        logMessage(LOG_WARN, "ADC", "Calibration not supported - using linear calculation");
        adc_cali_handle = nullptr;  // Will fall back to linear scaling
    } else {
        logMessage(LOG_ERROR, "ADC", "Calibration init failed - using linear calculation");
        adc_cali_handle = nullptr;
    }
}

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

        // Convert raw ADC to voltage using calibration if available
        if (adc_cali_handle != nullptr) {
            int voltage_mv;
            esp_err_t ret = adc_cali_raw_to_voltage(adc_cali_handle, rawSamples[i], &voltage_mv);
            if (ret == ESP_OK) {
                voltageSamples[i] = (voltage_mv / 1000.0) * ADC_VOLTAGE_DIVIDER;  // mV → V, apply divider
            } else {
                // Calibration failed - fall back to linear calculation for this sample
                voltageSamples[i] = (rawSamples[i] / float(ADC_RESOLUTION)) * ADC_MAX_VOLTAGE * ADC_VOLTAGE_DIVIDER;
            }
        } else {
            // Fallback to linear calculation if calibration unavailable
            voltageSamples[i] = (rawSamples[i] / float(ADC_RESOLUTION)) * ADC_MAX_VOLTAGE * ADC_VOLTAGE_DIVIDER;
        }

        total += rawSamples[i];
        if (rawSamples[i] < min_val) min_val = rawSamples[i];
        if (rawSamples[i] > max_val) max_val = rawSamples[i];

        delay(ADC_SAMPLE_DELAY_MS);
    }

    int adc_avg = total / ADC_SAMPLE_COUNT;
    int variance = max_val - min_val;

    // Convert to voltage using averaged ADC reading with calibration
    if (adc_cali_handle != nullptr) {
        int voltage_mv;
        esp_err_t ret = adc_cali_raw_to_voltage(adc_cali_handle, adc_avg, &voltage_mv);
        if (ret == ESP_OK) {
            status.voltage = (voltage_mv / 1000.0) * ADC_VOLTAGE_DIVIDER;  // mV → V, apply 2:1 divider
        } else {
            // Calibration failed - fall back to linear calculation
            status.voltage = (adc_avg / float(ADC_RESOLUTION)) * ADC_MAX_VOLTAGE * ADC_VOLTAGE_DIVIDER;
        }
    } else {
        // Fallback to linear calculation if calibration unavailable
        status.voltage = (adc_avg / float(ADC_RESOLUTION)) * ADC_MAX_VOLTAGE * ADC_VOLTAGE_DIVIDER;
    }

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

// Single-field accessor wrappers for getBatteryStatus()
// Use these when you only need one field to avoid unpacking the full struct
int getBatteryPercentage() { return getBatteryStatus().percentage; }
int getBatteryLevel() { return getBatteryStatus().level; }
bool isUSBPowered() { return getBatteryStatus().isUSBPowered; }

/**
 * Handle critical low battery warning display.
 * Shows warning screen once per discharge cycle (resets on USB reconnect).
 * Adds 5-second delay to ensure e-paper refresh completes.
 *
 * Should be called when battery drops below LOW_BATTERY_SLEEP_THRESHOLD (15%).
 * Caller is responsible for entering deep sleep afterward (if sleep_enabled).
 */
void handleCriticalLowBatteryWarning() {
    if (!hasShownLowBatteryWarning) {
        DisplayManager::showLowBatteryWarning();
        hasShownLowBatteryWarning = true;

        // Delay to ensure display completes refresh before sleep
        // E-paper refresh takes ~2 seconds, add buffer for safety
        delay(5000);  // 5 second delay for display completion
        logMessage(LOG_INFO, "POWER", "Low battery warning displayed");
    } else {
        logMessage(LOG_INFO, "POWER", "Low battery warning already shown, skipping display update");
    }
}

// ============================================================================
// Timezone Management
// ============================================================================

/**
 * Fetch timezone from server's GeoIP endpoint
 * Uses two-factor authentication (X-Device-ID + X-Auth-Token)
 * No API key needed - uses MaxMind GeoLite2 database on server
 * Returns true if successful, false otherwise
 */
bool fetchTimezoneFromServer() {
    const AppConfig& cfg = ConfigManager::getConfig();

    // Configure HTTPS client
    WiFiClientSecure secureClient;

    // Skip SSL certificate validation for Cloudflare tunnels (matches OTAManager behavior)
    // If server uses standard SSL certificate, this still works but is less secure
    secureClient.setInsecure();

    HTTPClient http;
    http.setTimeout(10000);  // 10 second timeout (increased from 5s)

    // Build server URL from configuration
    String protocol = cfg.server.use_ssl ? "https://" : "http://";
    String url = protocol + cfg.server.host;

    // Add port if non-standard (not 80 for HTTP or 443 for HTTPS)
    if ((cfg.server.use_ssl && cfg.server.port != 443) ||
        (!cfg.server.use_ssl && cfg.server.port != 80)) {
        url += ":" + String(cfg.server.port);
    }

    // Append timezone lookup endpoint path
    url += "/api/timezone/lookup";

    // Conditionally add update_db parameter to sync timezone to server
    // This enables local time display on messages (e.g., "sent at 19:15 EST" vs "sent at 00:15 UTC")
    if (cfg.timezone.update_server) {
        url += "?update_db=true";
    }

    http.begin(secureClient, url);

    // Add two-factor authentication headers (same as WebSocket connection)
    http.addHeader("X-Device-ID", cfg.device.id);
    http.addHeader("X-Auth-Token", cfg.security.auth_token);

    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "url=%s", url.c_str());
    logMessage(LOG_INFO, "TIME", "Fetching timezone from server GeoIP", logBuf);

    int httpCode = http.GET();

    if (httpCode == 200) {
        String response = http.getString();
        http.end();

        // Parse JSON response
        // Server returns IPGeolocation.io-compatible format
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, response);

        if (error) {
            snprintf(logBuf, sizeof(logBuf), "error=%s", error.c_str());
            logMessage(LOG_ERROR, "TIME", "JSON parse error", logBuf);
            return false;
        }

        // Extract timezone data
        // Response format:
        // {
        //   "date_time_unix": 1761273385.454,
        //   "timezone": "America/New_York",
        //   "timezone_offset": -5,
        //   "timezone_offset_with_dst": -4,
        //   "is_dst": true
        // }

        // API returns a double, extract as double first to preserve precision
        double timestamp_double = doc["date_time_unix"].as<double>();
        time_t timestamp = static_cast<time_t>(timestamp_double);

        // Validate timestamp (must be after Jan 1, 2020 and before year 2100)
        // Jan 1, 2020 = 1577836800
        // Jan 1, 2100 = 4102444800
        constexpr time_t MIN_VALID_TIMESTAMP = 1577836800;
        constexpr time_t MAX_VALID_TIMESTAMP = 4102444800;

        if (timestamp < MIN_VALID_TIMESTAMP || timestamp > MAX_VALID_TIMESTAMP) {
            snprintf(logBuf, sizeof(logBuf),
                    "timestamp=%lld (%.3f) out_of_range min=%lld max=%lld",
                    (long long)timestamp, timestamp_double,
                    (long long)MIN_VALID_TIMESTAMP, (long long)MAX_VALID_TIMESTAMP);
            logMessage(LOG_ERROR, "TIME", "Invalid timestamp from API", logBuf);
            return false;
        }

        currentTime = timestamp;
        timezoneOffsetSeconds = doc["timezone_offset_with_dst"].as<int>() * 3600;  // Convert hours to seconds
        lastTimeSyncTimestamp = currentTime;

        // Set ESP32 system clock to UTC time
        // This allows the hardware RTC to track time automatically during deep sleep
        // avoiding manual time estimation errors (especially during quiet hours with extended sleep)
        struct timeval tv;
        tv.tv_sec = timestamp;
        tv.tv_usec = 0;
        settimeofday(&tv, NULL);

        const char* tzName = doc["timezone"] | "Unknown";
        bool isDST = doc["is_dst"] | false;

        // On ESP32, time_t is int64_t (64-bit), must use %lld not %ld to avoid truncation
        snprintf(logBuf, sizeof(logBuf),
                "tz=%s offset=%ds unix=%lld dst=%s source=server",
                tzName,
                timezoneOffsetSeconds,
                (long long)currentTime,
                isDST ? "true" : "false");
        logMessage(LOG_INFO, "TIME", "Timezone synced successfully", logBuf);

        // Mark that we've successfully synced at least once
        hasEverSynced = true;

        return true;
    } else {
        snprintf(logBuf, sizeof(logBuf), "http_code=%d", httpCode);
        logMessage(LOG_WARN, "TIME", "Server GeoIP request failed", logBuf);
        http.end();
        return false;
    }
}

/**
 * Fetch timezone from IPGeolocation.io API
 * Uses user's API key from config (1000 requests/day free tier)
 * SSL certificate validation enabled (Mozilla CA bundle)
 * Returns true if successful, false otherwise
 */
bool fetchTimezoneFromIPGeolocation() {
    const AppConfig& cfg = ConfigManager::getConfig();

    // Validate API key is configured
    if (cfg.timezone.ipgeolocation_api_key.isEmpty()) {
        logMessage(LOG_ERROR, "TIME", "IPGeolocation API key not configured in config.json");
        return false;
    }

    // Configure HTTPS client with Mozilla CA certificate bundle
    WiFiClientSecure secureClient;

    // Use Mozilla CA certificate bundle for proper SSL validation (IPGeolocation.io uses standard certs)
    extern const uint8_t rootca_crt_bundle_start[] asm("_binary_certs_x509_crt_bundle_bin_start");
    extern const uint8_t rootca_crt_bundle_end[] asm("_binary_certs_x509_crt_bundle_bin_end");
    size_t bundle_size = rootca_crt_bundle_end - rootca_crt_bundle_start;
    secureClient.setCACertBundle(rootca_crt_bundle_start, bundle_size);

    HTTPClient http;
    http.setTimeout(10000);  // 10 second timeout

    // Build IPGeolocation.io API URL
    // NOTE: API key is sent as query parameter (standard practice for this API)
    // Security: Connection is encrypted with TLS, API key is not logged
    String url = "https://api.ipgeolocation.io/timezone?apiKey=" + cfg.timezone.ipgeolocation_api_key;

    http.begin(secureClient, url);

    char logBuf[128];
    // Don't log URL to avoid exposing API key in logs
    logMessage(LOG_INFO, "TIME", "Fetching timezone from IPGeolocation.io", nullptr);

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

        // API returns a double, extract as double first to preserve precision
        double timestamp_double = doc["date_time_unix"].as<double>();
        time_t timestamp = static_cast<time_t>(timestamp_double);

        // Validate timestamp (must be after Jan 1, 2020 and before year 2100)
        constexpr time_t MIN_VALID_TIMESTAMP = 1577836800;
        constexpr time_t MAX_VALID_TIMESTAMP = 4102444800;

        if (timestamp < MIN_VALID_TIMESTAMP || timestamp > MAX_VALID_TIMESTAMP) {
            snprintf(logBuf, sizeof(logBuf),
                    "timestamp=%lld (%.3f) out_of_range min=%lld max=%lld",
                    (long long)timestamp, timestamp_double,
                    (long long)MIN_VALID_TIMESTAMP, (long long)MAX_VALID_TIMESTAMP);
            logMessage(LOG_ERROR, "TIME", "Invalid timestamp from API", logBuf);
            return false;
        }

        currentTime = timestamp;
        timezoneOffsetSeconds = doc["timezone_offset_with_dst"].as<int>() * 3600;  // Convert hours to seconds
        lastTimeSyncTimestamp = currentTime;

        // Set ESP32 system clock to UTC time
        struct timeval tv;
        tv.tv_sec = timestamp;
        tv.tv_usec = 0;
        settimeofday(&tv, NULL);

        const char* tzName = doc["timezone"] | "Unknown";
        bool isDST = doc["is_dst"] | false;

        snprintf(logBuf, sizeof(logBuf),
                "tz=%s offset=%ds unix=%lld dst=%s source=ipgeolocation",
                tzName,
                timezoneOffsetSeconds,
                (long long)currentTime,
                isDST ? "true" : "false");
        logMessage(LOG_INFO, "TIME", "Timezone synced successfully", logBuf);

        hasEverSynced = true;
        return true;
    } else {
        snprintf(logBuf, sizeof(logBuf), "http_code=%d", httpCode);
        logMessage(LOG_WARN, "TIME", "IPGeolocation.io request failed", logBuf);
        http.end();
        return false;
    }
}

/**
 * Fetch timezone information (dispatcher for all timezone sources)
 * Routes to appropriate function based on config.timezone.source:
 * - "server": Server-side GeoIP (free, unlimited, requires auth, default)
 * - "ipgeolocation": IPGeolocation.io API (1000/day free, requires API key)
 * Returns true if successful, false otherwise
 */
bool fetchTimezoneFromAPI() {
    const AppConfig& cfg = ConfigManager::getConfig();
    String source = cfg.timezone.source;
    source.toLowerCase();  // Normalize for comparison

    char logBuf[64];
    snprintf(logBuf, sizeof(logBuf), "source=%s", source.c_str());
    logMessage(LOG_INFO, "TIME", "Fetching timezone", logBuf);

    if (source == "server") {
        return fetchTimezoneFromServer();
    } else if (source == "ipgeolocation") {
        return fetchTimezoneFromIPGeolocation();
    } else {
        // Invalid source - log warning and fall back to server
        snprintf(logBuf, sizeof(logBuf), "invalid_source=%s fallback=server", source.c_str());
        logMessage(LOG_WARN, "TIME", "Invalid timezone source", logBuf);
        return fetchTimezoneFromServer();
    }
}

/**
 * Check if timezone sync is needed
 * Sync on power-on boot or based on elapsed time (1 hour until first success, then 24 hours)
 * Uses actual elapsed time from ESP32 RTC to handle variable sleep durations correctly
 * (e.g., quiet hours with 15-min sleep vs normal 1-min sleep)
 */
bool shouldSyncTimezone(bool isPowerOnBoot) {
    const AppConfig& cfg = ConfigManager::getConfig();

    // Always sync on power-on boot
    if (isPowerOnBoot) {
        return true;
    }

    // If time is unknown or never synced, attempt sync now
    // This handles edge cases where RTC wasn't set properly
    if (currentTime == 0 || lastTimeSyncTimestamp == 0 || !hasEverSynced) {
        return true;
    }

    // Use actual elapsed time to determine if sync is needed
    time_t elapsedSeconds = currentTime - lastTimeSyncTimestamp;
    time_t syncIntervalSeconds = cfg.timezone.sync_interval_hours * 3600;  // Default: 24 hours

    return (elapsedSeconds >= syncIntervalSeconds);
}

/**
 * Update current time from ESP32 RTC
 * The ESP32 hardware RTC continues running during deep sleep, providing accurate time
 * without manual estimation. This eliminates clock drift that occurred with the previous
 * approach (which added base sleep duration, ignoring quiet hours multipliers).
 */
void updateEstimatedTime() {
    // Read current time from ESP32 system clock (RTC)
    struct timeval tv;
    gettimeofday(&tv, NULL);
    currentTime = tv.tv_sec;

    // Log the time update with source information
    char logBuf[80];
    snprintf(logBuf, sizeof(logBuf), "utc_time=%lld source=ESP32_RTC", (long long)currentTime);
    logMessage(LOG_DEBUG, "TIME", "Updated time from RTC", logBuf);
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
static bool otaCheckedThisBoot = false; // Track if OTA check completed this boot (prevents duplicate checks)

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

            // Show disconnect message ONCE after 10 failed attempts (not on every subsequent disconnect)
            // This prevents repeated e-paper refreshes that drain battery and are visually disruptive
            // After the initial warning, preserve the "Connection Lost" display until reconnected
            if (!enteringSleep && metrics.failedConnections == 10) {
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
            registrationErrorCount = 0;  // Reset registration error tracking on successful connection
            registrationFailedPermanently = false;
            ResilienceManager::markConnectionRestored();  // Track restoration
            ResilienceManager::recordHeartbeat();  // Record initial heartbeat
            const AppConfig& cfg = ConfigManager::getConfig();

            // Declare hexKey outside if block so it's accessible throughout the case
            String hexKey = "";

            // Send registration message with AES key if encryption is enabled
            if (cfg.security.use_aes && !cfg.security.aes_key.isEmpty()) {
                // Convert base64 key to hex for server
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
                } else {
                    logMessage(LOG_WARN, "WS", "Failed to decode AES key for registration");
                }
            }

            // Send registration message with auth_token (mandatory two-factor auth)
            // This must be sent immediately after WebSocket connection for authentication
            JsonDocument regDoc;
            regDoc["type"] = "register";
            regDoc["device_type"] = "esp32_eink";
            regDoc["auth_token"] = cfg.security.auth_token;

            // Include AES key if available
            if (!hexKey.isEmpty()) {
                regDoc["aes_key"] = hexKey;
            }

            String regMsg;
            serializeJson(regDoc, regMsg);
            webSocket.sendTXT(regMsg);

            if (!hexKey.isEmpty()) {
                logMessage(LOG_INFO, "WS", "Sent registration with auth_token and AES key");
            } else {
                logMessage(LOG_INFO, "WS", "Sent registration with auth_token");
            }

            // Check for firmware updates on first WebSocket connection (power-on boot only)
            // Timing: Performed here instead of immediately after WiFi connect ensures DNS has fully
            // propagated and stabilized. WebSocket connection success confirms DNS is working.
            // This prevents spurious "DNS Failed" errors during OTA hostname resolution.
            if (!otaCheckedThisBoot && !justWokeFromSleep) {
                otaCheckedThisBoot = true;  // Mark as checked to prevent duplicate checks on reconnects
                logMessage(LOG_INFO, "OTA", "First connection established - checking for firmware updates");

                // DNS stabilization: Allow additional time after WebSocket connection before making
                // new DNS queries. Even though WebSocket connected successfully, the DNS resolver
                // may need time to fully stabilize its cache and state. This prevents transient
                // "DNS Failed" errors on the first HTTP request after WebSocket establishment.
                delay(2000);  // 2-second stabilization period

                checkForFirmwareUpdate();
                lastOTACheckMillis = millis();
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
            const AppConfig& cfg = ConfigManager::getConfig();

            registrationErrorCount++;

            if (registrationErrorCount < MAX_REGISTRATION_RETRIES) {
                // Attempts 1-2: Show retry screen with counter (1 of 2, 2 of 2)
                String line1 = "Connecting...";
                String line2 = String("Attempt ") + registrationErrorCount + " of 2";
                String line3 = String("ID: ") + (deviceId[0] ? deviceId : cfg.device.id.c_str());
                String line4 = "Checking registration";
                DisplayManager::showMessage(line1, line2, line3, line4);

                delay(30000);  // 30 seconds between retries
            } else {
                // Attempt 3: Show "restart device" screen (no attempt counter)
                String line1 = "Setup Required";
                String line2 = String("ID: ") + (deviceId[0] ? deviceId : cfg.device.id.c_str());
                String line3 = "Complete registration";
                String line4 = "then RESTART device";
                DisplayManager::showMessage(line1, line2, line3, line4);

                // Mark registration as permanently failed (stop reconnecting)
                registrationFailedPermanently = true;
                wsConnected = false;
                webSocket.disconnect();

                // Deep sleep for 10 minutes, then wake to check again
                // Counter will reset on wake (not RTC), so gets fresh 3 attempts
                delay(2000);  // Show message for 2 seconds before sleeping
                enterDeepSleep(10);  // 10 minutes (parameter is in minutes, not microseconds)
                // Never returns from deep sleep
            }
        } else if (strcmp(errorCode, "INVALID_AUTH_TOKEN") == 0 || strcmp(errorCode, "AUTH_FAILED") == 0) {
            // Handle authentication failures - config error, not transient
            const AppConfig& cfg = ConfigManager::getConfig();

            registrationErrorCount++;

            if (registrationErrorCount < MAX_REGISTRATION_RETRIES) {
                // Attempts 1-2: Show retry screen (in case of transient issue)
                String line1 = "Auth Failed";
                String line2 = String("Attempt ") + registrationErrorCount + " of 2";
                String line3 = "Check auth_token";
                String line4 = "in config.json";
                DisplayManager::showMessage(line1, line2, line3, line4);

                delay(30000);  // 30 seconds between retries
            } else {
                // Attempt 3: Show permanent error screen - this is a config issue
                String line1 = "Auth Token Invalid";
                String line2 = "Fix config.json:";
                String line3 = "security.auth_token";
                String line4 = "then RESTART device";
                DisplayManager::showMessage(line1, line2, line3, line4);

                // Mark registration as permanently failed (stop reconnecting)
                registrationFailedPermanently = true;
                wsConnected = false;
                webSocket.disconnect();

                // Deep sleep for 10 minutes, then wake to check again
                // Counter will reset on wake (not RTC), so gets fresh 3 attempts
                delay(2000);  // Show message for 2 seconds before sleeping
                enterDeepSleep(10);  // 10 minutes (parameter is in minutes, not microseconds)
                // Never returns from deep sleep
            }
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

            snprintf(buf, sizeof(buf), "timeout=%lu force_high=%s",
                     cfg.wifi.timeout_ms, cfg.wifi.force_high_power ? "true" : "false");
            logMessage(LOG_TEST, "CONFIG", "WiFi", buf);

            snprintf(buf, sizeof(buf), "host=%s port=%d ssl=%s",
                     cfg.server.host.c_str(), cfg.server.port, cfg.server.use_ssl ? "true" : "false");
            logMessage(LOG_TEST, "CONFIG", "Server", buf);

            snprintf(buf, sizeof(buf), "rotation=%d", cfg.display.rotation);
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

            if (key == "device.id") {
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
            logMessage(LOG_TEST, "CONFIG", "Configuration saved to LittleFS");

            File f = LittleFS.open("/config.json", "r");
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
            snprintf(buf, sizeof(buf), "device_id=%s server=%s:%d",
                     cfg.device.id.c_str(), cfg.server.host.c_str(), cfg.server.port);
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
class SlackNetworkManager {
public:
    static bool connectWiFi(bool silent = false) {
        const AppConfig& cfg = ConfigManager::getConfig();
        char logBuf[128];

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
            snprintf(logBuf, sizeof(logBuf), "attempt=%d/%d", attempt, maxRetries);
            logMessage(LOG_INFO, "WIFI", "Connecting", logBuf);

            WiFi.mode(WIFI_STA);

            // Set network hostname using device.name from config (RFC 1123 compliant)
            // Always append 4-char device ID hash to ensure uniqueness across multiple devices
            // Example: "Slack Reactions Display" -> "slack-reactions-display-a1b2"
            // Fallback: "slack-reactions-device-{hash}" if device.name is empty/invalid
            String deviceHash = generateDeviceHash(cfg.device.id);
            String hashedFallback = "slack-reactions-device-" + deviceHash;

            String hostnameWithHash = cfg.device.name.isEmpty()
                ? hashedFallback
                : cfg.device.name + "-" + deviceHash;
            String hostname = sanitizeHostname(hostnameWithHash, hashedFallback);

            WiFi.setHostname(hostname.c_str());
            snprintf(logBuf, sizeof(logBuf), "hostname=%s (from: %s)",
                     hostname.c_str(), cfg.device.name.c_str());
            logMessage(LOG_DEBUG, "WIFI", "Hostname configured", logBuf);

            // Apply TX power: force HIGH or use adaptive (LOW → MEDIUM → HIGH)
            wifi_power_t txPower = wifiPowerState.currentPower;
            const char* powerName = "UNKNOWN";

            if (cfg.wifi.force_high_power) {
                // Force HIGH power mode (disables adaptive escalation)
                txPower = WIFI_POWER_19_5dBm;
                powerName = "HIGH (forced)";
            } else {
                // Use adaptive power from RTC memory (starts at LOW, auto-escalates on failures)
                if (txPower == WIFI_POWER_11dBm) {
                    powerName = "LOW";
                } else if (txPower == WIFI_POWER_15dBm) {
                    powerName = "MEDIUM";
                } else {
                    powerName = "HIGH";
                }
            }

            // WiFi.begin() with no parameters uses credentials stored in ESP32 NVS
            // These are saved by WiFiManager during provisioning mode
            WiFi.begin();

            // Set TX power after WiFi.begin() to avoid "Neither AP or STA has been started" warning
            WiFi.setTxPower(txPower);
            snprintf(logBuf, sizeof(logBuf), "tx_power=%s", powerName);
            logMessage(LOG_DEBUG, "WIFI", "Setting TX power", logBuf);

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

                // Reset adaptive power failures on successful connection (unless forced HIGH)
                if (!cfg.wifi.force_high_power) {
                    wifiPowerState.consecutiveFailures = 0;
                    wifiPowerState.totalFailedWakes = 0;
                    // Keep current power level (sticky behavior)
                }

                // Give DNS servers time to be ready (prevents early OTA check failures)
                delay(2000);

                return true;
            }

            // Failed attempt - handle adaptive power escalation (only if not forcing HIGH)
            if (!cfg.wifi.force_high_power) {
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

    /**
     * Validate device ID format for security
     * - Must be 3-64 characters
     * - Only alphanumeric, underscore, hyphen allowed
     */
    static bool validateDeviceId(const String& id) {
        if (id.length() < 3 || id.length() > 64) return false;

        for (size_t i = 0; i < id.length(); i++) {
            char c = id.charAt(i);
            if (!isalnum(c) && c != '_' && c != '-') return false;
        }
        return true;
    }

    /**
     * Generate a short 4-character hash from device ID for privacy-safe hostname fallback
     *
     * Creates a stable, deterministic hash using simple checksum algorithm (no crypto imports).
     * The hash is consistent across reboots for the same device.id.
     *
     * Algorithm:
     * - Compute 32-bit hash using polynomial rolling hash (multiplier: 31)
     * - Convert to base-36 (0-9, a-z) for compact representation
     * - Pad to 4 characters for consistency
     *
     * Example: "lilygo_t5_v231_213_1" → "a3f2"
     *
     * @param deviceId The device.id string to hash
     * @return 4-character alphanumeric hash string
     */
    static String generateDeviceHash(const String& deviceId) {
        uint32_t hash = 0;
        for (size_t i = 0; i < deviceId.length(); i++) {
            hash = hash * 31 + deviceId.charAt(i);
        }
        // Convert to base-36 (0-9, a-z) for short string
        // 36^4 = 1,679,616 possible combinations (sufficient for collision resistance)
        String result = String(hash % 1679616, 36);

        // Pad to exactly 4 characters
        while (result.length() < 4) {
            result = "0" + result;
        }
        return result;
    }

    /**
     * Sanitize hostname to comply with RFC 1123 DNS naming conventions
     *
     * RFC 1123 Rules:
     * - Valid characters: a-z, A-Z, 0-9, hyphen (-)
     * - Must start and end with alphanumeric (not hyphen)
     * - Maximum length: 63 characters
     * - Case-insensitive (converted to lowercase for consistency)
     *
     * Sanitization Process:
     * 1. Convert to lowercase
     * 2. Replace spaces and underscores with hyphens
     * 3. Remove invalid characters (keep only alphanumeric and hyphens)
     * 4. Truncate to 63 characters
     * 5. Remove leading/trailing hyphens
     * 6. Collapse consecutive hyphens into single hyphen
     * 7. Fallback to provided fallback string if result is empty/invalid
     *
     * @param name Original hostname to sanitize (from config.device.name)
     * @param fallback Fallback hostname if sanitization produces invalid result
     * @return RFC 1123 compliant hostname string
     */
    static String sanitizeHostname(const String& name, const String& fallback) {
        String result = name;

        // Step 1: Convert to lowercase
        result.toLowerCase();

        // Step 2 & 3: Replace spaces/underscores with hyphens, remove invalid chars
        String cleaned = "";
        for (size_t i = 0; i < result.length(); i++) {
            char c = result.charAt(i);
            if (isalnum(c)) {
                cleaned += c;
            } else if (c == ' ' || c == '_' || c == '-') {
                cleaned += '-';
            }
            // All other characters are silently removed
        }
        result = cleaned;

        // Step 4: Truncate to 63 characters max
        if (result.length() > 63) {
            result = result.substring(0, 63);
        }

        // Step 5: Remove leading hyphens
        while (result.length() > 0 && result.charAt(0) == '-') {
            result = result.substring(1);
        }

        // Remove trailing hyphens
        while (result.length() > 0 && result.charAt(result.length() - 1) == '-') {
            result = result.substring(0, result.length() - 1);
        }

        // Step 6: Collapse consecutive hyphens
        String collapsed = "";
        bool lastWasHyphen = false;
        for (size_t i = 0; i < result.length(); i++) {
            char c = result.charAt(i);
            if (c == '-') {
                if (!lastWasHyphen) {
                    collapsed += c;
                    lastWasHyphen = true;
                }
                // Skip consecutive hyphens
            } else {
                collapsed += c;
                lastWasHyphen = false;
            }
        }
        result = collapsed;

        // Step 7: Validate result is non-empty and starts/ends with alphanumeric
        if (result.length() == 0 ||
            !isalnum(result.charAt(0)) ||
            !isalnum(result.charAt(result.length() - 1))) {
            // Use fallback if result is invalid (recursive sanitization with ultimate fallback)
            return sanitizeHostname(fallback, "slack-reactions-device");
        }

        return result;
    }

    static bool startProvisioning(bool forcePortal = false) {
        logMessage(LOG_INFO, "WIFI", "Starting WiFi provisioning mode");

        // Validate that config is loaded before proceeding
        if (!ConfigManager::isLoaded()) {
            logMessage(LOG_ERROR, "CONFIG", "Config not loaded - cannot start provisioning");
            ESP.restart();
            return false;  // Never reached, but explicit
        }

        WiFiManager wm;

        // Load current config to pre-fill Device ID field
        const AppConfig& cfg = ConfigManager::getConfig();

        // Create buffer for device ID (WiFiManagerParameter needs char array, not String)
        static char device_id_buffer[65];  // 64 chars + null terminator
        strncpy(device_id_buffer, cfg.device.id.c_str(), sizeof(device_id_buffer) - 1);
        device_id_buffer[sizeof(device_id_buffer) - 1] = '\0';  // Ensure null termination

        // Create custom parameter with current device ID as default/editable value
        WiFiManagerParameter custom_device_id(
            "device_id",                    // Parameter ID
            "Device ID",                    // Label shown in form
            device_id_buffer,               // Current value (pre-filled, user can edit)
            64                              // Max length
        );

        // Add informational HTML text above the field
        WiFiManagerParameter custom_html_text(
            "<p><small>Change Device ID only if instructed by support for security recovery. "
            "Leave unchanged for normal WiFi setup.</small></p>"
        );

        // Add parameters to WiFiManager (will appear in portal)
        wm.addParameter(&custom_html_text);
        wm.addParameter(&custom_device_id);

        // Flag to track if we should save custom parameters
        bool shouldSaveConfig = false;

        // Set custom AP name
        const char* apName = "SlackReact-Setup";

        // Configure callback to show provisioning UI on e-paper (QR code screen)
        wm.setAPCallback([](WiFiManager* myWM) {
            logMessage(LOG_INFO, "WIFI", "Entered provisioning mode - showing QR code");
            String ssid = myWM->getConfigPortalSSID();
            String ip = WiFi.softAPIP().toString();
            DisplayManager::showProvisioningMode(ssid, ip);  // Shows QR code screen
        });

        // Configure callback when WiFi credentials/params are saved
        wm.setSaveConfigCallback([&shouldSaveConfig]() {
            shouldSaveConfig = true;
            logMessage(LOG_INFO, "WIFI", "Configuration save callback triggered");
            DisplayManager::showMessage("Config Saved!", "Processing...");
        });

        // Set timeout for config portal (3 minutes)
        wm.setConfigPortalTimeout(180);

        // Choose connection method based on context
        bool success;
        if (forcePortal) {
            // Manual config mode (long press) - force portal even if WiFi works
            logMessage(LOG_INFO, "WIFI", "Forcing config portal (disconnecting WiFi first)");
            WiFi.disconnect();  // Disconnect from current WiFi to enable AP mode
            success = wm.startConfigPortal(apName);  // Force portal to open
        } else {
            // Auto-triggered mode (no WiFi) - try to connect or open portal
            logMessage(LOG_INFO, "WIFI", "Auto-connect mode");
            success = wm.autoConnect(apName);
        }

        // Process results
        if (success) {
            // Check if we should process custom parameter changes
            if (shouldSaveConfig) {
                // Get the new device ID value from form (with nullptr safety)
                const char* rawValue = custom_device_id.getValue();
                String newDeviceId = rawValue ? String(rawValue) : String("");
                newDeviceId.trim();  // Remove leading/trailing whitespace

                // Only update if changed and non-empty
                if (newDeviceId.length() > 0 && newDeviceId != cfg.device.id) {
                    if (validateDeviceId(newDeviceId)) {
                        // CRITICAL: Save old ID before modifying config (cfg references same object)
                        String oldDeviceId = cfg.device.id;

                        // Update config in memory
                        AppConfig& mutableCfg = ConfigManager::getMutableConfig();
                        mutableCfg.device.id = newDeviceId;

                        // Save to LittleFS
                        if (ConfigManager::save()) {
                            // Log the change
                            char logBuf[128];
                            snprintf(logBuf, sizeof(logBuf), "old_id=%s new_id=%s",
                                     oldDeviceId.c_str(), newDeviceId.c_str());
                            logMessage(LOG_INFO, "CONFIG", "Device ID updated via portal", logBuf);

                            // Truncate device IDs for display (max ~18 chars to fit with prefix)
                            String oldIdShort = oldDeviceId;
                            String newIdShort = newDeviceId;

                            if (oldIdShort.length() > 18) {
                                oldIdShort = oldIdShort.substring(0, 15) + "...";
                            }
                            if (newIdShort.length() > 18) {
                                newIdShort = newIdShort.substring(0, 15) + "...";
                            }

                            // Show success message (4 lines, properly truncated)
                            DisplayManager::showMessage(
                                "ID Changed!",              // Line 1: ~11 chars
                                "Old: " + oldIdShort,       // Line 2: max 23 chars
                                "New: " + newIdShort,       // Line 3: max 23 chars
                                "Restarting..."             // Line 4: ~13 chars
                            );
                            delay(3000);
                        } else {
                            logMessage(LOG_ERROR, "CONFIG", "Failed to save config file");
                            DisplayManager::showMessage("Save Failed!", "Using old ID", "", "Restarting...");
                            delay(2000);
                        }
                    } else {
                        // Invalid format
                        logMessage(LOG_WARN, "CONFIG", "Invalid device ID format", newDeviceId.c_str());
                        DisplayManager::showMessage(
                            "Invalid ID Format!",
                            "Must be 3-64 chars",
                            "alphanumeric/_/- only",
                            "Using old ID"
                        );
                        delay(3000);
                    }
                } else {
                    // Device ID unchanged
                    logMessage(LOG_INFO, "CONFIG", "Device ID unchanged");
                }
            }

            // WiFi connection successful
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
            DisplayManager::showMessage("Setup Timeout", "Restarting...");
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

        // Build WebSocket path with device_id and display_variant
        String path = cfg.server.path + "?device_id=";
        path += cfg.device.id;

        // Add display_variant if configured (for OTA firmware tracking)
        if (!cfg.device.display_variant.isEmpty()) {
            path += "&display_variant=";
            path += cfg.device.display_variant;
        }

        // Use beginSSL for secure WebSocket connection if configured
        if (cfg.server.use_ssl) {
            webSocket.beginSSL(cfg.server.host.c_str(), cfg.server.port, path);
        } else {
            webSocket.begin(cfg.server.host.c_str(), cfg.server.port, path);
        }

        webSocket.onEvent(webSocketEvent);

        // Disable library auto-reconnect (manual handling) and internal heartbeat (conflicts with server heartbeats)
        webSocket.setReconnectInterval(0);

        // Update reconnect delay
        reconnectDelay = ConnectionTiming::WS_INITIAL_RECONNECT_MS;
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
        const uint32_t now = millis();
        // WiFi reconnect interval: 30 seconds
        constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 30000;
        if (now - lastWiFiReconnect > WIFI_RECONNECT_INTERVAL_MS) {
            lastWiFiReconnect = now;
            logMessage(LOG_WARN, "WIFI", "Reconnecting");
            WiFi.reconnect();
        }
    }

    static void handleWebSocketReconnection() {
        const AppConfig& cfg = ConfigManager::getConfig();

        // Don't reconnect if registration permanently failed
        if (registrationFailedPermanently) {
            return;
        }

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
                reconnectDelay = min(reconnectDelay * 2, ConnectionTiming::WS_MAX_RECONNECT_MS);
            }
        } else {
            // Reset reconnect delay on successful connection
            reconnectDelay = ConnectionTiming::WS_INITIAL_RECONNECT_MS;
        }
    }

    static void checkHeartbeatTimeout() {
        const AppConfig& cfg = ConfigManager::getConfig();
        if (wsConnected && lastHeartbeat > 0) {
            uint32_t timeSinceHeartbeat = (unsigned long)(millis() - lastHeartbeat);
            // Only disconnect if we haven't received ANY messages (not just heartbeats) for double the timeout
            // The WebSocketsClient has its own ping/pong mechanism that should keep the connection alive
            if (timeSinceHeartbeat > (ConnectionTiming::HEARTBEAT_TIMEOUT_MS * 2)) {
                char logBuf[64];
                snprintf(logBuf, sizeof(logBuf), "last_seen_ms=%lu", timeSinceHeartbeat);
                logMessage(LOG_WARN, "WS", "Connection appears stale, forcing reconnect", logBuf);

                // Force reconnection
                wsConnected = false;
                webSocket.disconnect();
                lastHeartbeat = 0;
            } else if (timeSinceHeartbeat > ConnectionTiming::HEARTBEAT_TIMEOUT_MS) {
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

    // Check wake reason early to apply UART stabilization if needed
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    // UART stabilization: After deep sleep wake, the UART peripheral needs extra time
    // to stabilize its clock/timing circuits before transmitting data reliably.
    // Without this delay, the first ~50 characters get corrupted (bit-level framing errors).
    // Power-on boots don't need this delay since the UART initializes cleanly.
    if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER || wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
        delay(100);  // 100ms allows UART clock to lock and stabilize
    }

    while (!Serial && millis() < 3000) {
        // Wait for serial port to connect (timeout after 3 seconds)
    }

    metrics.startTime = millis();

    Serial.println(F("\n\n========================================"));
    Serial.println(F("ESP32 Slack Reactions Client v2.0"));
    Serial.println(F("========================================"));

    // Check wake reason
    ++bootCount;

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
            // Button woke us up - check for long press to enter config mode
            pinMode(GPIO_NUM_39, INPUT_PULLUP);
            delay(100);  // Debounce

            if (digitalRead(GPIO_NUM_39) == LOW) {  // Button still pressed
                // Measure how long button is held
                uint32_t pressStart = millis();
                while (digitalRead(GPIO_NUM_39) == LOW && (millis() - pressStart) < 5000) {
                    delay(50);
                }

                uint32_t pressDuration = millis() - pressStart;
                if (pressDuration >= 3000) {  // Held for 3+ seconds = config mode
                    logMessage(LOG_INFO, "CONFIG", "Long press detected - entering forced config mode");

                    // Load config for startProvisioning to access
                    ConfigManager::begin();
                    if (!ConfigManager::load()) {
                        logMessage(LOG_ERROR, "CONFIG", "Failed to load config - restarting");
                        ESP.restart();
                        return;  // Never reached, but explicit
                    }

                    // Initialize display hardware (required for QR code provisioning screen)
                    initializeDisplayHardware();

                    // Enter provisioning with forced portal mode (will show QR code screen)
                    // startProvisioning() calls ESP.restart() at end, so no return needed
                    SlackNetworkManager::startProvisioning(true);  // true = force portal
                }
            }

            // Short press = normal wake behavior
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

    // Initialize display hardware (SPI + display driver)
    initializeDisplayHardware();

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
    ResilienceManager::init(ConnectionTiming::HEARTBEAT_INTERVAL_MS, ConnectionTiming::HEARTBEAT_TIMEOUT_MS);
    logMessage(LOG_INFO, "SYSTEM", "Resilience manager initialized");

    // Initialize ADC calibration for accurate battery voltage readings
    // Must be called before first getBatteryStatus() call
    setupADCCalibration();

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
    // Show warning and optionally skip WiFi/sleep if battery is critically low
    if (isWakeFromSleep && !usbPowered) {
        using namespace BatteryConstants;
        if (startupBattery.percentage >= 0 && startupBattery.percentage < LOW_BATTERY_SLEEP_THRESHOLD) {
            char logBuf[128];
            snprintf(logBuf, sizeof(logBuf), "battery=%d%% voltage=%.2fV sleep_enabled=%s",
                     startupBattery.percentage, startupBattery.voltage,
                     cfg.power.sleep_enabled ? "true" : "false");
            logMessage(LOG_WARN, "POWER", "Critical low battery on wake", logBuf);

            // Show warning screen (regardless of sleep_enabled setting)
            handleCriticalLowBatteryWarning();

            // Only skip WiFi and sleep if sleep is enabled
            if (cfg.power.sleep_enabled) {
                logMessage(LOG_INFO, "POWER", "Skipping WiFi and entering sleep to preserve battery");
                enterDeepSleep(calculateSleepDuration());
                // Never returns, but for clarity:
                return;
            } else {
                logMessage(LOG_INFO, "POWER", "Sleep disabled - continuing with WiFi connection despite low battery");
            }
        }
    }

    // Small delay before WiFi connection to let radio stabilize
    logMessage(LOG_INFO, "WIFI", "Waiting for WiFi radio to stabilize");
    delay(500);

    // Connect to WiFi (silent mode on wake from sleep to skip connection screens)
    if (SlackNetworkManager::connectWiFi(isWakeFromSleep)) {
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

            // Track wake power state to determine appropriate battery timeout later
            // This flag persists across deep sleep cycles for timeout differentiation
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
                if (!lastReaction.hasReaction && !hasShownBlankScreen) {
                    // First boot (never shown blank screen) - show it once
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
                // Fallback mode: skip_refresh_on_no_message disabled
                // Always refresh display on wake (consumes more battery but ensures display updates)
                shouldRefreshDisplay = true;
                logMessage(LOG_INFO, "DISPLAY", "Refresh optimization disabled - always refreshing on wake");
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
                hasShownBlankScreen = true;  // Mark that we've shown the blank screen

                char stateBuf[64];
                snprintf(stateBuf, sizeof(stateBuf), "fullRefresh=true displayShowingBattery=%s",
                         displayShowingBattery ? "true" : "false");
                logMessage(LOG_DEBUG, "DISPLAY", "Display state updated after refresh", stateBuf);
            }
        }

        // DNS stabilization: After WiFi connection, DNS resolver needs time to initialize
        // HTTPS requests (especially SSL handshake) will fail if DNS isn't ready
        logMessage(LOG_DEBUG, "NETWORK", "Waiting for DNS resolver to stabilize");
        delay(2000);  // 2 seconds for DNS to fully initialize

        // Timezone sync logic (after WiFi connected)
        bool isPowerOnBoot = (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED);

        if (shouldSyncTimezone(isPowerOnBoot)) {
            if (!hasEverSynced) {
                logMessage(LOG_INFO, "TIME", "Timezone sync needed (never synced)");
            } else {
                char syncBuf[128];
                time_t elapsedSinceSync = currentTime - lastTimeSyncTimestamp;
                snprintf(syncBuf, sizeof(syncBuf), "elapsed=%ldh", elapsedSinceSync / 3600);
                logMessage(LOG_INFO, "TIME", "Timezone sync needed (scheduled)", syncBuf);
            }

            if (fetchTimezoneFromAPI()) {
                // Sync successful
                char timeBuf[64];
                int hour = getCurrentLocalHour();
                snprintf(timeBuf, sizeof(timeBuf), "current_hour=%d quiet_hours=%s",
                        hour, isQuietHours() ? "yes" : "no");
                logMessage(LOG_INFO, "TIME", "Timezone sync complete", timeBuf);

                // SSL/TLS cleanup delay: After HTTPS timezone fetch, allow SSL/TLS stack time to
                // release resources before WebSocket SSL handshake. Without this, the first WebSocket
                // connection may fail with ABNORMAL_CLOSURE (1006) due to resource contention between
                // HTTPClient SSL and WebSocket SSL operations.
                delay(1000);
            } else {
                // Sync failed - will retry on next wake if shouldSyncTimezone() returns true
                if (!hasEverSynced) {
                    logMessage(LOG_WARN, "TIME", "CRITICAL: First timezone sync failed - will retry on next wake");
                } else {
                    logMessage(LOG_WARN, "TIME", "Timezone sync failed, will retry on next wake");
                }
            }
        } else {
            // Update time from RTC
            updateEstimatedTime();

            // Log current time status
            char timeBuf[128];
            int hour = getCurrentLocalHour();
            time_t elapsedSinceSync = currentTime - lastTimeSyncTimestamp;
            time_t nextSyncIn = (cfg.timezone.sync_interval_hours * 3600) - elapsedSinceSync;

            snprintf(timeBuf, sizeof(timeBuf),
                    "time_since_sync=%ldh next_sync_in=%ldh current_hour=%d",
                    elapsedSinceSync / 3600,
                    nextSyncIn / 3600,
                    hour);
            logMessage(LOG_DEBUG, "TIME", "Using RTC time", timeBuf);
        }

        // Small delay before WebSocket connection
        delay(2000);
        SlackNetworkManager::connectWebSocket();

        // Initialize OTA manager (requires network connectivity)
        logMessage(LOG_INFO, "OTA", "Initializing OTA manager");

        // Build server URL from config components
        // Only include port if non-default (443 for HTTPS, 80 for HTTP)
        // This prevents potential HTTP client parsing issues with explicit default ports
        String serverUrl = String(cfg.server.use_ssl ? "https://" : "http://") + cfg.server.host;
        if ((cfg.server.use_ssl && cfg.server.port != 443) ||
            (!cfg.server.use_ssl && cfg.server.port != 80)) {
            serverUrl += ":" + String(cfg.server.port);
        }

        otaManager = new OTAManager(serverUrl, cfg.device.id);

        // Check boot validation (mark new firmware as valid if just updated)
        if (otaManager->checkBootValidation()) {
            logMessage(LOG_INFO, "OTA", "Boot validation successful - new firmware marked as valid");
        }

        // Firmware update check occurs automatically on first WebSocket connection (power-on boot only)
        // See WStype_CONNECTED handler for implementation. Deferred timing ensures DNS stability.
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
    SlackNetworkManager::handleReconnection();

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

    // Power management - check battery status every 10 seconds
    using namespace BatteryConstants;
    const AppConfig& cfg = ConfigManager::getConfig();

    static unsigned long lastPowerCheck = 0;
    unsigned long now = millis();

    // Check power status every 10 seconds (rollover-safe comparison)
    if ((unsigned long)(now - lastPowerCheck) > 10000) {
        lastPowerCheck = now;

        // Call getBatteryStatus() ONCE and reuse the result (avoid multiple 50ms ADC samplings)
        BatteryStatus batteryStatus = getBatteryStatus();
        bool isOnBattery = !batteryStatus.isUSBPowered;
        int batteryPct = batteryStatus.percentage;

        // Track USB connection state to detect when USB is plugged in (regardless of sleep_enabled)
        // This ensures the low battery warning flag resets properly even when sleep is disabled
        static bool lastPowerWasOnBattery = false;

        // Detect USB reconnection and reset warning flag
        if (!isOnBattery && lastPowerWasOnBattery) {
            hasShownLowBatteryWarning = false;  // Reset warning flag when USB is plugged in
            lastPowerWasOnBattery = false;
            logMessage(LOG_INFO, "POWER", "USB connected - low battery warning flag reset");
        } else if (isOnBattery && !lastPowerWasOnBattery) {
            lastPowerWasOnBattery = true;
            logMessage(LOG_DEBUG, "POWER", "Switched to battery power");
        }

        // Check for critically low battery (regardless of sleep_enabled)
        // This ensures users are warned even if they've disabled sleep
        if (isOnBattery && batteryPct >= 0 && batteryPct < LOW_BATTERY_SLEEP_THRESHOLD) {
            char logBuf[128];
            snprintf(logBuf, sizeof(logBuf), "battery=%d%% voltage=%.2fV sleep_enabled=%s",
                     batteryPct, batteryStatus.voltage,
                     cfg.power.sleep_enabled ? "true" : "false");
            logMessage(LOG_WARN, "POWER", "Critical low battery in loop", logBuf);

            // Show warning screen (regardless of sleep_enabled)
            handleCriticalLowBatteryWarning();

            // Only enter deep sleep if sleep is enabled
            if (cfg.power.sleep_enabled) {
                logMessage(LOG_INFO, "POWER", "Entering deep sleep to preserve battery");
                enterDeepSleep(calculateSleepDuration());
                return;  // Never reached, but for clarity
            } else {
                logMessage(LOG_INFO, "POWER", "Sleep disabled - staying awake despite low battery");
            }
        }

        // Additional power management logic (only when sleep is enabled)
        if (cfg.power.sleep_enabled) {
            // Use RTC memory for battery state (survives deep sleep)
            unsigned long& batteryModeStartTime = rtcBatteryModeStartTime;
            bool& wasPreviouslyOnBattery = rtcWasPreviouslyOnBattery;

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
                // Note: hasShownLowBatteryWarning reset is handled above (outside sleep_enabled block)
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

                // Battery timeout varies by power transition scenario for optimal behavior:
                // Scenario 1: Woke on battery (batteryModeStartTime = 0, wokeOnBattery = true)
                //   → Use 20s timeout: Device was already unplugged, minimize battery drain
                // Scenario 2: USB unplugged while awake (batteryModeStartTime > 0, set during runtime)
                //   → Use 60s grace period: Allow user to finish interactions after unplugging
                unsigned long sleepTimeout;
                const char* timeoutReason;

                // Detect scenario: batteryModeStartTime=0 means we woke already on battery
                // batteryModeStartTime>0 means USB was unplugged during this wake cycle
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
        } else {
            static bool loggedOnce = false;
            if (!loggedOnce) {
                logMessage(LOG_INFO, "POWER", "Sleep disabled in config");
                loggedOnce = true;
            }
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
        // checkForUpdate returns false for two normal cases:
        // 1. No firmware configured on server
        // 2. Already up to date
        // OTAManager logs detailed status internally, so only log errors here
        if (otaManager->getStatus() == OTAManager::OTAStatus::FAILED) {
            logMessage(LOG_ERROR, "OTA", "Update check failed", otaManager->getLastError().c_str());
        }
        // Success case (up to date or no firmware) already logged by OTAManager
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