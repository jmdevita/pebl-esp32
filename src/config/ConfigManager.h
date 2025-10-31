#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// Configuration structure to hold all settings
struct AppConfig {
    // Device settings
    struct {
        String id;
        String name;
        String display_variant;  // Platformio environment name (e.g., "lilygo_t5_gdew_4g")
    } device;

    // WiFi settings
    // Note: WiFi credentials (SSID/password) are stored in ESP32 NVS by WiFiManager
    // and do not need to be in config.json. Provisioning mode handles credential management.
    struct {
        uint32_t timeout_ms;
        // TX power settings
        bool force_high_power;           // true = always use HIGH (19.5dBm), false = adaptive LOW→MEDIUM→HIGH
        uint8_t escalation_threshold;    // Failures before escalating power (default: 3)
        uint8_t max_failed_wakes;        // Failed wakes before fallback mode (default: 20)
    } wifi;

    // Server settings
    struct {
        String host;
        uint16_t port;
        String path;
        bool use_ssl;
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
        bool use_aes;
        String aes_key;
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
    // Note: API URL and key are hardcoded in firmware (no user configuration needed)
    struct {
        uint8_t sync_interval_hours;  // How often to re-sync (default: 24)
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
