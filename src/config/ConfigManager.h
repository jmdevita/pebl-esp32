#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>

// Configuration structure to hold all settings
struct AppConfig {
    // Device settings
    struct {
        String id;
        String name;
    } device;

    // WiFi settings
    struct {
        String ssid;
        String password;
        uint32_t timeout_ms;
        uint32_t reconnect_interval_ms;
    } wifi;

    // Server settings
    struct {
        String host;
        uint16_t port;
        String path;
        bool use_ssl;
    } server;

    // Display settings
    struct {
        uint16_t width;
        uint16_t height;
        uint8_t rotation;
        struct {
            uint8_t cs;
            uint8_t dc;
            uint8_t rst;
            uint8_t busy;
            uint8_t sclk;
            uint8_t mosi;
        } pins;
    } display;

    // Timing settings
    struct {
        uint32_t heartbeat_interval_ms;
        uint32_t heartbeat_timeout_ms;
        uint32_t ws_initial_reconnect_ms;
        uint32_t ws_max_reconnect_ms;
    } timing;

    // Security settings
    struct {
        bool use_aes;
        String aes_key;
    } security;

    // Power management settings
    struct {
        bool sleep_enabled;
        uint8_t sleep_duration_min;
        uint8_t battery_pin;
        float usb_threshold_v;
    } power;

    // Logging settings
    struct {
        String default_level;
        bool enable_test_commands;
    } logging;
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
    static String getWiFiSSID() { return config.wifi.ssid; }
    static String getWiFiPassword() { return config.wifi.password; }
    static String getServerHost() { return config.server.host; }
    static uint16_t getServerPort() { return config.server.port; }
    
    // For testing - allows injecting JSON config
    static bool loadFromJson(const String& json);
    static String toJson();
};

#endif // CONFIG_MANAGER_H
