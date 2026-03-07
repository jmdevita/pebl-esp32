#include "ConfigManager.h"

// Static member definitions
AppConfig ConfigManager::config;
bool ConfigManager::loaded = false;
const char* ConfigManager::CONFIG_FILE = "/config.json";

// External log function (defined in main.cpp)
extern void logMessage(int level, const char* module, const char* message, const char* kvPairs);

void ConfigManager::logMessage(const char* level, const char* module, const char* message, const char* details) {
    // Map string level to int for external function
    int logLevel = 2; // INFO by default
    if (strcmp(level, "ERROR") == 0) logLevel = 0;
    else if (strcmp(level, "WARN") == 0) logLevel = 1;
    else if (strcmp(level, "DEBUG") == 0) logLevel = 3;
    
    ::logMessage(logLevel, module, message, details);
}

void ConfigManager::setDefaults() {
    // Device defaults
    config.device.id = "";
    config.device.name = "ESP32 Device";
    config.device.display_variant = "";  // Must be set in config.json for OTA updates

    // WiFi defaults
    config.wifi.timeout_ms = 15000;
    // TX power defaults
    config.wifi.force_high_power = false;  // Use adaptive power by default
    config.wifi.escalation_threshold = 3;
    config.wifi.max_failed_wakes = 20;
    config.wifi.seed_networks.clear();  // No seed networks by default (use NVS only)

    // Server defaults
    config.server.host = "";  // Must be set in config.json
    config.server.port = 443;
    config.server.path = "/ws-stream";
    config.server.use_ssl = true;

    // Display defaults for T5 V2.3.1
    config.display.rotation = 1;

    // Security defaults
    config.security.auth_token = "";  // Must be set in config.json
    config.security.encryption = "ecdh";

    // Power defaults
    config.power.sleep_enabled = true;
    config.power.sleep_duration_min = 1;

    // Logging defaults
    config.logging.default_level = "WARN";
    config.logging.enable_test_commands = false;

    // Timezone defaults
    config.timezone.sync_interval_hours = 24;
    config.timezone.source = "server";                 // Default to server GeoIP (free, unlimited)
    config.timezone.ipgeolocation_api_key = "";        // Empty by default
    config.timezone.update_server = true;              // Default: sync timezone to server for local time display

    // Quiet hours defaults
    config.quiet_hours.start_hour = 23;  // 11 PM
    config.quiet_hours.end_hour = 7;     // 7 AM
    config.quiet_hours.sleep_multiplier = 6;  // 6x sleep duration during quiet hours (30 min)

    // Display policy defaults
    config.display_policy.skip_refresh_on_no_message = true;   // Save battery by only refreshing on new reactions

    // OTA defaults
    config.ota.enabled = true;  // OTA enabled by default for official devices
}

bool ConfigManager::begin() {
    logMessage("INFO", "CONFIG", "Initializing LittleFS");

    if (!LittleFS.begin(true)) {
        logMessage("ERROR", "CONFIG", "Failed to mount LittleFS");
        return false;
    }

    // Check LittleFS size
    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();

    char buf[128];
    snprintf(buf, sizeof(buf), "total=%u used=%u free=%u",
             totalBytes, usedBytes, totalBytes - usedBytes);
    logMessage("INFO", "CONFIG", "LittleFS mounted", buf);

    return load();
}

bool ConfigManager::load() {
    logMessage("INFO", "CONFIG", "Loading configuration", CONFIG_FILE);

    // Set defaults first
    setDefaults();

    // Check if config file exists
    if (!LittleFS.exists(CONFIG_FILE)) {
        logMessage("WARN", "CONFIG", "Config file not found, using defaults");
        loaded = true;  // Defaults are valid
        return true;
    }

    // Open the file
    File file = LittleFS.open(CONFIG_FILE, "r");
    if (!file) {
        logMessage("ERROR", "CONFIG", "Failed to open config file");
        return false;
    }

    // Read file content
    size_t size = file.size();
    if (size > 4096) {
        logMessage("ERROR", "CONFIG", "Config file too large", "size>4096");
        file.close();
        return false;
    }

    String jsonStr = file.readString();
    file.close();

    // Parse JSON
    return loadFromJson(jsonStr);
}

bool ConfigManager::loadFromJson(const String& jsonStr) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonStr);

    if (error) {
        char buf[64];
        snprintf(buf, sizeof(buf), "error=%s", error.c_str());
        logMessage("ERROR", "CONFIG", "JSON parse error", buf);
        return false;
    }

    // Parse device section
    if (doc["device"].is<JsonObject>()) {
        JsonObject device = doc["device"];
        config.device.id = device["id"] | config.device.id;

        // Auto-generate device ID from hardware eFuse MAC if not manually set.
        // The eFuse MAC is factory-burned and unique per chip, producing a deterministic
        // 12-char lowercase hex ID (e.g., "a1b2c3d4e5f6") on every boot.
        // No save back to config.json — keeps the file clean so a manually set ID always wins.
        if (config.device.id.isEmpty() || config.device.id == "esp32-default") {
            uint64_t mac = ESP.getEfuseMac();
            char hwId[13];
            snprintf(hwId, sizeof(hwId), "%04x%08x",
                     (uint16_t)(mac >> 32), (uint32_t)mac);
            config.device.id = String(hwId);
            logMessage("INFO", "CONFIG", "Device ID auto-generated from hardware MAC", config.device.id.c_str());
        }

        config.device.name = device["name"] | config.device.name;
        config.device.display_variant = device["display_variant"] | config.device.display_variant;
    }

    // Parse WiFi section
    if (doc["wifi"].is<JsonObject>()) {
        JsonObject wifi = doc["wifi"];
        config.wifi.timeout_ms = wifi["timeout_ms"] | config.wifi.timeout_ms;
        // TX power settings
        config.wifi.force_high_power = wifi["force_high_power"] | config.wifi.force_high_power;
        config.wifi.escalation_threshold = wifi["escalation_threshold"] | config.wifi.escalation_threshold;
        config.wifi.max_failed_wakes = wifi["max_failed_wakes"] | config.wifi.max_failed_wakes;

        // Parse seed_networks array (pinned WiFi networks)
        if (wifi["seed_networks"].is<JsonArray>()) {
            config.wifi.seed_networks.clear();
            JsonArray seedArray = wifi["seed_networks"].as<JsonArray>();

            // Limit to 5 seed networks to prevent memory issues
            int seedCount = 0;
            for (JsonObject seed : seedArray) {
                if (seedCount >= 5) {
                    logMessage("WARN", "CONFIG", "Max 5 seed networks allowed, ignoring extras");
                    break;
                }

                String ssid = seed["ssid"] | "";
                String password = seed["password"] | "";

                // Validate SSID (required, max 32 chars per WiFi spec)
                if (ssid.length() == 0) {
                    logMessage("WARN", "CONFIG", "Skipping seed network with empty SSID");
                    continue;
                }
                if (ssid.length() > 32) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "ssid_length=%u (max 32)", (unsigned int)ssid.length());
                    logMessage("WARN", "CONFIG", "Skipping seed network - SSID too long", buf);
                    continue;
                }

                // Validate password (max 64 chars per WPA2 spec)
                if (password.length() > 64) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "password_length=%u (max 64)", (unsigned int)password.length());
                    logMessage("WARN", "CONFIG", "Skipping seed network - password too long", buf);
                    continue;
                }

                WiFiSeedNetwork seedNet;
                seedNet.ssid = ssid;
                seedNet.password = password;
                config.wifi.seed_networks.push_back(seedNet);
                seedCount++;

                char buf[64];
                snprintf(buf, sizeof(buf), "ssid=%s", ssid.c_str());
                logMessage("DEBUG", "CONFIG", "Loaded seed network", buf);
            }

            if (seedCount > 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "count=%d", seedCount);
                logMessage("INFO", "CONFIG", "Seed networks loaded", buf);
            }
        }
    }

    // Parse server section
    if (doc["server"].is<JsonObject>()) {
        JsonObject server = doc["server"];
        config.server.host = server["host"] | config.server.host;
        config.server.port = server["port"] | config.server.port;
        config.server.path = server["path"] | config.server.path;
        config.server.use_ssl = server["use_ssl"] | config.server.use_ssl;
        config.server.reconnect_jitter_max_sec = server["reconnect_jitter_max_sec"] | config.server.reconnect_jitter_max_sec;
    }

    // Parse display section
    if (doc["display"].is<JsonObject>()) {
        JsonObject display = doc["display"];
        config.display.rotation = display["rotation"] | config.display.rotation;
    }

    // Parse security section
    if (doc["security"].is<JsonObject>()) {
        JsonObject security = doc["security"];
        config.security.auth_token = security["auth_token"] | config.security.auth_token;

        // New format: "encryption" field ("ecdh" or "none")
        if (security["encryption"].is<const char*>()) {
            config.security.encryption = security["encryption"].as<String>();
        } else if (security["use_aes"].is<bool>()) {
            // Migration from old config: use_aes:true → "ecdh", use_aes:false → "none"
            // Old aes_key field is silently ignored (ECDH uses NVS-generated keys)
            config.security.encryption = security["use_aes"].as<bool>() ? "ecdh" : "none";
            logMessage("INFO", "CONFIG", "Migrated use_aes to encryption format",
                      config.security.encryption.c_str());
        }
    }

    // Parse power section
    if (doc["power"].is<JsonObject>()) {
        JsonObject power = doc["power"];
        config.power.sleep_enabled = power["sleep_enabled"] | config.power.sleep_enabled;
        config.power.sleep_duration_min = power["sleep_duration_min"] | config.power.sleep_duration_min;
    }

    // Parse logging section
    if (doc["logging"].is<JsonObject>()) {
        JsonObject logging = doc["logging"];
        config.logging.default_level = logging["default_level"] | config.logging.default_level;
        config.logging.enable_test_commands = logging["enable_test_commands"] | config.logging.enable_test_commands;
    }

    // Parse timezone section
    if (doc["timezone"].is<JsonObject>()) {
        JsonObject timezone = doc["timezone"];
        config.timezone.sync_interval_hours = timezone["sync_interval_hours"] | config.timezone.sync_interval_hours;
        config.timezone.source = timezone["source"] | config.timezone.source;
        config.timezone.ipgeolocation_api_key = timezone["ipgeolocation_api_key"] | config.timezone.ipgeolocation_api_key;
        config.timezone.update_server = timezone["update_server"] | config.timezone.update_server;
    }

    // Parse quiet_hours section
    if (doc["quiet_hours"].is<JsonObject>()) {
        JsonObject quiet_hours = doc["quiet_hours"];
        config.quiet_hours.start_hour = quiet_hours["start_hour"] | config.quiet_hours.start_hour;
        config.quiet_hours.end_hour = quiet_hours["end_hour"] | config.quiet_hours.end_hour;
        config.quiet_hours.sleep_multiplier = quiet_hours["sleep_multiplier"] | config.quiet_hours.sleep_multiplier;
    }

    // Parse display_policy section
    if (doc["display_policy"].is<JsonObject>()) {
        JsonObject display_policy = doc["display_policy"];
        config.display_policy.skip_refresh_on_no_message = display_policy["skip_refresh_on_no_message"] | config.display_policy.skip_refresh_on_no_message;
    }

    // Parse OTA section
    if (doc["ota"].is<JsonObject>()) {
        JsonObject ota = doc["ota"];
        config.ota.enabled = ota["enabled"] | config.ota.enabled;
    }

    loaded = true;

    // Validate display_variant is set (required for OTA updates)
    if (config.device.display_variant.isEmpty()) {
        logMessage("WARN", "CONFIG", "display_variant not set - OTA updates will be disabled");
        logMessage("WARN", "CONFIG", "Add display_variant to config.json to enable OTA (see config.json.example)");
    }

    // Validate sync_interval_hours (1-168 hours = 1 week max)
    // Minimum prevents excessive syncing/battery drain, maximum prevents RTC drift accumulation
    if (config.timezone.sync_interval_hours < 1) {
        char buf[64];
        snprintf(buf, sizeof(buf), "invalid_interval=%d clamping_to=1",
                config.timezone.sync_interval_hours);
        logMessage("WARN", "CONFIG", "Sync interval too low", buf);
        config.timezone.sync_interval_hours = 1;
    } else if (config.timezone.sync_interval_hours > 168) {
        char buf[64];
        snprintf(buf, sizeof(buf), "invalid_interval=%d clamping_to=168",
                config.timezone.sync_interval_hours);
        logMessage("WARN", "CONFIG", "Sync interval too high (max 1 week)", buf);
        config.timezone.sync_interval_hours = 168;
    }

    // Validate timezone source
    config.timezone.source.toLowerCase();  // Normalize to lowercase
    if (config.timezone.source != "server" &&
        config.timezone.source != "ipgeolocation") {
        char buf[128];
        snprintf(buf, sizeof(buf), "invalid_source=%s defaulting_to=server",
                config.timezone.source.c_str());
        logMessage("WARN", "CONFIG", "Invalid timezone source", buf);
        config.timezone.source = "server";
    }

    // Validate API key length (prevent memory issues)
    // IPGeolocation.io API keys are typically 32 characters
    if (config.timezone.ipgeolocation_api_key.length() > 256) {
        logMessage("ERROR", "CONFIG", "IPGeolocation API key exceeds maximum length (256 chars), clearing");
        config.timezone.ipgeolocation_api_key = "";  // Clear invalid key
    }

    // Warn if ipgeolocation selected but no API key
    if (config.timezone.source == "ipgeolocation" && config.timezone.ipgeolocation_api_key.isEmpty()) {
        logMessage("WARN", "CONFIG", "timezone.source='ipgeolocation' but ipgeolocation_api_key is empty!");
        logMessage("WARN", "CONFIG", "Device will fail to sync timezone. Set ipgeolocation_api_key or change source to 'server'.");
    }

    logMessage("INFO", "CONFIG", "Configuration loaded successfully");

    // Log key config values for debugging
    char buf[256];
    snprintf(buf, sizeof(buf), "device_id=%s server=%s:%d variant=%s",
             config.device.id.c_str(), config.server.host.c_str(), config.server.port,
             config.device.display_variant.c_str());
    logMessage("DEBUG", "CONFIG", "Loaded values", buf);

    return true;
}

bool ConfigManager::save() {
    logMessage("INFO", "CONFIG", "Saving configuration");

    File file = LittleFS.open(CONFIG_FILE, "w");
    if (!file) {
        logMessage("ERROR", "CONFIG", "Failed to create config file");
        return false;
    }

    String json = toJson();
    size_t written = file.print(json);
    file.close();

    char buf[64];
    snprintf(buf, sizeof(buf), "bytes=%u", written);
    logMessage("INFO", "CONFIG", "Configuration saved", buf);

    return written > 0;
}

String ConfigManager::toJson() {
    JsonDocument doc;

    // Device section
    JsonObject device = doc["device"].to<JsonObject>();
    device["id"] = config.device.id;
    device["name"] = config.device.name;
    device["display_variant"] = config.device.display_variant;

    // WiFi section
    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["timeout_ms"] = config.wifi.timeout_ms;
    wifi["force_high_power"] = config.wifi.force_high_power;
    wifi["escalation_threshold"] = config.wifi.escalation_threshold;
    wifi["max_failed_wakes"] = config.wifi.max_failed_wakes;

    // Serialize seed_networks array (only if non-empty)
    if (!config.wifi.seed_networks.empty()) {
        JsonArray seedArray = wifi["seed_networks"].to<JsonArray>();
        for (const auto& seed : config.wifi.seed_networks) {
            JsonObject seedObj = seedArray.add<JsonObject>();
            seedObj["ssid"] = seed.ssid;
            seedObj["password"] = seed.password;
        }
    }

    // Server section
    JsonObject server = doc["server"].to<JsonObject>();
    server["host"] = config.server.host;
    server["port"] = config.server.port;
    server["path"] = config.server.path;
    server["use_ssl"] = config.server.use_ssl;
    server["reconnect_jitter_max_sec"] = config.server.reconnect_jitter_max_sec;

    // Display section
    JsonObject display = doc["display"].to<JsonObject>();
    display["rotation"] = config.display.rotation;

    // Security section
    JsonObject security = doc["security"].to<JsonObject>();
    security["auth_token"] = config.security.auth_token;
    security["encryption"] = config.security.encryption;

    // Logging section
    JsonObject logging = doc["logging"].to<JsonObject>();
    logging["default_level"] = config.logging.default_level;
    logging["enable_test_commands"] = config.logging.enable_test_commands;

    // Timezone section
    JsonObject timezone = doc["timezone"].to<JsonObject>();
    timezone["sync_interval_hours"] = config.timezone.sync_interval_hours;
    timezone["source"] = config.timezone.source;
    timezone["ipgeolocation_api_key"] = config.timezone.ipgeolocation_api_key;
    timezone["update_server"] = config.timezone.update_server;

    // Quiet hours section
    JsonObject quiet_hours = doc["quiet_hours"].to<JsonObject>();
    quiet_hours["start_hour"] = config.quiet_hours.start_hour;
    quiet_hours["end_hour"] = config.quiet_hours.end_hour;
    quiet_hours["sleep_multiplier"] = config.quiet_hours.sleep_multiplier;

    // Display policy section
    JsonObject display_policy = doc["display_policy"].to<JsonObject>();
    display_policy["skip_refresh_on_no_message"] = config.display_policy.skip_refresh_on_no_message;

    // OTA section
    JsonObject ota = doc["ota"].to<JsonObject>();
    ota["enabled"] = config.ota.enabled;

    String output;
    serializeJsonPretty(doc, output);
    return output;
}

void ConfigManager::reset() {
    logMessage("WARN", "CONFIG", "Resetting to defaults");
    setDefaults();
    loaded = true;
}
