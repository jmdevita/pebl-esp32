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
    config.device.id = "esp32-default";
    config.device.name = "ESP32 Device";
    config.device.display_variant = "";  // Must be set in config.json for OTA updates

    // WiFi defaults
    config.wifi.timeout_ms = 15000;
    // TX power defaults
    config.wifi.force_high_power = false;  // Use adaptive power by default
    config.wifi.escalation_threshold = 3;
    config.wifi.max_failed_wakes = 20;

    // Server defaults
    config.server.host = "slack-reactions.devita.dev";
    config.server.port = 443;
    config.server.path = "/ws-stream";
    config.server.use_ssl = true;

    // Display defaults for T5 V2.3.1
    config.display.rotation = 1;

    // Security defaults
    config.security.use_aes = false;
    config.security.aes_key = "";

    // Power defaults
    config.power.sleep_enabled = true;
    config.power.sleep_duration_min = 1;

    // Logging defaults
    config.logging.default_level = "WARN";
    config.logging.enable_test_commands = false;

    // Timezone defaults
    config.timezone.sync_interval_hours = 24;

    // Quiet hours defaults
    config.quiet_hours.start_hour = 23;  // 11 PM
    config.quiet_hours.end_hour = 7;     // 7 AM
    config.quiet_hours.sleep_multiplier = 6;  // 6x sleep duration during quiet hours (30 min)

    // Display policy defaults
    config.display_policy.skip_refresh_on_no_message = true;   // Save battery by only refreshing on new reactions
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
    }

    // Parse server section
    if (doc["server"].is<JsonObject>()) {
        JsonObject server = doc["server"];
        config.server.host = server["host"] | config.server.host;
        config.server.port = server["port"] | config.server.port;
        config.server.path = server["path"] | config.server.path;
        config.server.use_ssl = server["use_ssl"] | config.server.use_ssl;
    }

    // Parse display section
    if (doc["display"].is<JsonObject>()) {
        JsonObject display = doc["display"];
        config.display.rotation = display["rotation"] | config.display.rotation;
    }

    // Parse security section
    if (doc["security"].is<JsonObject>()) {
        JsonObject security = doc["security"];
        config.security.use_aes = security["use_aes"] | config.security.use_aes;
        config.security.aes_key = security["aes_key"] | config.security.aes_key;
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

    loaded = true;

    // Validate display_variant is set (required for OTA updates)
    if (config.device.display_variant.isEmpty()) {
        logMessage("WARN", "CONFIG", "display_variant not set - OTA updates will be disabled");
        logMessage("WARN", "CONFIG", "Add display_variant to config.json to enable OTA (see config.json.example)");
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

    // Server section
    JsonObject server = doc["server"].to<JsonObject>();
    server["host"] = config.server.host;
    server["port"] = config.server.port;
    server["path"] = config.server.path;
    server["use_ssl"] = config.server.use_ssl;

    // Display section
    JsonObject display = doc["display"].to<JsonObject>();
    display["rotation"] = config.display.rotation;

    // Security section
    JsonObject security = doc["security"].to<JsonObject>();
    security["use_aes"] = config.security.use_aes;
    security["aes_key"] = config.security.aes_key;

    // Logging section
    JsonObject logging = doc["logging"].to<JsonObject>();
    logging["default_level"] = config.logging.default_level;
    logging["enable_test_commands"] = config.logging.enable_test_commands;

    // Timezone section
    JsonObject timezone = doc["timezone"].to<JsonObject>();
    timezone["sync_interval_hours"] = config.timezone.sync_interval_hours;

    // Quiet hours section
    JsonObject quiet_hours = doc["quiet_hours"].to<JsonObject>();
    quiet_hours["start_hour"] = config.quiet_hours.start_hour;
    quiet_hours["end_hour"] = config.quiet_hours.end_hour;
    quiet_hours["sleep_multiplier"] = config.quiet_hours.sleep_multiplier;

    // Display policy section
    JsonObject display_policy = doc["display_policy"].to<JsonObject>();
    display_policy["skip_refresh_on_no_message"] = config.display_policy.skip_refresh_on_no_message;

    String output;
    serializeJsonPretty(doc, output);
    return output;
}

void ConfigManager::reset() {
    logMessage("WARN", "CONFIG", "Resetting to defaults");
    setDefaults();
    loaded = true;
}
