#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>

// Seed network structure for pre-configured WiFi networks
// Defined outside AppConfig to allow proper type referencing
struct WiFiSeedNetwork {
    String ssid;       // Max 32 chars (WiFi spec)
    String password;   // Max 64 chars (WPA2 spec)
};

// Configuration structure to hold all settings
struct AppConfig {
    // Device settings
    struct {
        String id;
        String name;
        String display_variant;  // Platformio environment name (e.g., "lilygo_t5_gdew_4g")
    } device;

    // WiFi settings
    // Note: WiFi credentials are stored in ESP32 NVS by WiFiCredentialManager with LRU eviction.
    // seed_networks in config.json are pinned (never evicted) and merged with NVS on boot.
    struct {
        uint32_t timeout_ms;
        // TX power settings
        bool force_high_power;           // true = always use HIGH (19.5dBm), false = adaptive LOW→MEDIUM→HIGH
        uint8_t escalation_threshold;    // Failures before escalating power (default: 3)
        uint8_t max_failed_wakes;        // Failed wakes before fallback mode (default: 20)

        // Seed networks: Pre-configured WiFi networks from config.json
        // These are pinned (never evicted by LRU) and merged with NVS credentials on boot.
        // Max 5 networks total (seeds + NVS combined).
        std::vector<WiFiSeedNetwork> seed_networks;
    } wifi;

    // Server settings
    struct {
        String host;
        uint16_t port;
        String path;
        bool use_ssl;
        uint16_t reconnect_jitter_max_sec = 0;  // 0 = disabled (B2C default), set 30-60 for fleet deployments
    } server;

    // Display settings
    // Note: Display width/height and pins are set at compile-time via platformio.ini build flags
    // Pins are hardware-specific and cannot be changed without rewiring
    struct {
        uint8_t rotation;
    } display;

    // Note: Connection timing settings are hardcoded to match server protocol
    // and should not be user-configurable to prevent connection issues

    // Security settings
    struct {
        String auth_token;     // Device authentication token (two-factor auth with device_id)
        String encryption;     // "ecdh" (default, auto key generation) or "none"
    } security;

    // Power management settings
    // Note: Battery pin is hardcoded to GPIO 35, USB voltage thresholds are constants
    struct {
        bool sleep_enabled;
        uint8_t sleep_duration_min;
    } power;

    // Logging settings
    struct {
        String default_level;
        bool enable_test_commands;
    } logging;

    // Timezone settings
    // Supports two sources: "server" (default, free), "ipgeolocation" (requires API key)
    struct {
        uint8_t sync_interval_hours;      // How often to re-sync (default: 24, range: 1-168 hours)
        String source;                    // "server" or "ipgeolocation"
        String ipgeolocation_api_key;     // IPGeolocation.io API key (only if source = "ipgeolocation")
        bool update_server;               // Whether to sync timezone to server (enables local time display)
    } timezone;

    // Quiet hours settings
    struct {
        uint8_t start_hour;           // Start of quiet hours (0-23, default: 23)
        uint8_t end_hour;             // End of quiet hours (0-23, default: 7)
        uint8_t sleep_multiplier;     // Sleep multiplier during quiet hours (default: 3)
    } quiet_hours;

    // Display update policy settings
    struct {
        bool skip_refresh_on_no_message;  // Skip full refresh on wake if no new reaction (default: true)
    } display_policy;

    // OTA update settings
    struct {
        bool enabled;  // Enable/disable OTA firmware updates (default: true)
    } ota;
};

class ConfigManager {
private:
    static AppConfig config;
    static bool loaded;
    static const char* CONFIG_FILE;

    static void logMessage(const char* level, const char* module, const char* message, const char* details = nullptr);
    static void setDefaults();

public:
    static bool begin();
    static bool load();
    static bool save();
    static void reset();
    
    static const AppConfig& getConfig() { return config; }
    static AppConfig& getMutableConfig() { return config; }  // For TEST commands only
    static bool isLoaded() { return loaded; }
    
    // Helper methods for common access patterns
    static String getDeviceId() { return config.device.id; }
    static String getServerHost() { return config.server.host; }
    static uint16_t getServerPort() { return config.server.port; }
    static String getDisplayVariant() { return config.device.display_variant; }
    
    // For testing - allows injecting JSON config
    static bool loadFromJson(const String& json);
    static String toJson();
};

#endif // CONFIG_MANAGER_H
