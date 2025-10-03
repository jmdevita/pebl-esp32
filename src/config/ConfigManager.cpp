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

    // WiFi defaults
    config.wifi.ssid = "";
    config.wifi.password = "";
    config.wifi.timeout_ms = 15000;
    config.wifi.reconnect_interval_ms = 30000;

    // Server defaults
    config.server.host = "slack-reactions.devita.dev";
    config.server.port = 443;
    config.server.path = "/ws-stream";
    config.server.use_ssl = true;

    // Display defaults for T5 V2.3.1
    config.display.width = 250;
    config.display.height = 122;
    config.display.rotation = 1;
    config.display.pins.cs = 5;
    config.display.pins.dc = 17;
    config.display.pins.rst = 16;
    config.display.pins.busy = 4;
    config.display.pins.sclk = 18;
    config.display.pins.mosi = 23;

    // Timing defaults
    config.timing.heartbeat_interval_ms = 15000;
    config.timing.heartbeat_timeout_ms = 30000;
    config.timing.ws_initial_reconnect_ms = 5000;
    config.timing.ws_max_reconnect_ms = 60000;

    // Security defaults
    config.security.use_aes = false;
    config.security.aes_key = "";

    // Power defaults
    config.power.sleep_enabled = true;
    config.power.sleep_duration_min = 5;
    config.power.battery_pin = 36;
    config.power.usb_threshold_v = 4.0;  // Lowered from 4.2 to handle typical charging voltages (4.0-4.15V)

    // Logging defaults
    config.logging.default_level = "WARN";
    config.logging.enable_test_commands = true;
}

bool ConfigManager::begin() {
    logMessage("INFO", "CONFIG", "Initializing SPIFFS");

    if (!SPIFFS.begin(true)) {
        logMessage("ERROR", "CONFIG", "Failed to mount SPIFFS");
        return false;
    }

    // Check SPIFFS size
    size_t totalBytes = SPIFFS.totalBytes();
    size_t usedBytes = SPIFFS.usedBytes();
    
    char buf[128];
    snprintf(buf, sizeof(buf), "total=%u used=%u free=%u", 
             totalBytes, usedBytes, totalBytes - usedBytes);
    logMessage("INFO", "CONFIG", "SPIFFS mounted", buf);

    return load();
}

bool ConfigManager::load() {
    logMessage("INFO", "CONFIG", "Loading configuration", CONFIG_FILE);

    // Set defaults first
    setDefaults();

    // Check if config file exists
    if (!SPIFFS.exists(CONFIG_FILE)) {
        logMessage("WARN", "CONFIG", "Config file not found, using defaults");
        loaded = true;  // Defaults are valid
        return true;
    }

    // Open the file
    File file = SPIFFS.open(CONFIG_FILE, "r");
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
    if (doc.containsKey("device")) {
        JsonObject device = doc["device"];
        config.device.id = device["id"] | config.device.id;
        config.device.name = device["name"] | config.device.name;
    }

    // Parse WiFi section
    if (doc.containsKey("wifi")) {
        JsonObject wifi = doc["wifi"];
        config.wifi.ssid = wifi["ssid"] | config.wifi.ssid;
        config.wifi.password = wifi["password"] | config.wifi.password;
        config.wifi.timeout_ms = wifi["timeout_ms"] | config.wifi.timeout_ms;
        config.wifi.reconnect_interval_ms = wifi["reconnect_interval_ms"] | config.wifi.reconnect_interval_ms;
    }

    // Parse server section
    if (doc.containsKey("server")) {
        JsonObject server = doc["server"];
        config.server.host = server["host"] | config.server.host;
        config.server.port = server["port"] | config.server.port;
        config.server.path = server["path"] | config.server.path;
        config.server.use_ssl = server["use_ssl"] | config.server.use_ssl;
    }

    // Parse display section
    if (doc.containsKey("display")) {
        JsonObject display = doc["display"];
        config.display.width = display["width"] | config.display.width;
        config.display.height = display["height"] | config.display.height;
        config.display.rotation = display["rotation"] | config.display.rotation;

        if (display.containsKey("pins")) {
            JsonObject pins = display["pins"];
            config.display.pins.cs = pins["cs"] | config.display.pins.cs;
            config.display.pins.dc = pins["dc"] | config.display.pins.dc;
            config.display.pins.rst = pins["rst"] | config.display.pins.rst;
            config.display.pins.busy = pins["busy"] | config.display.pins.busy;
            config.display.pins.sclk = pins["sclk"] | config.display.pins.sclk;
            config.display.pins.mosi = pins["mosi"] | config.display.pins.mosi;
        }
    }

    // Parse timing section
    if (doc.containsKey("timing")) {
        JsonObject timing = doc["timing"];
        config.timing.heartbeat_interval_ms = timing["heartbeat_interval_ms"] | config.timing.heartbeat_interval_ms;
        config.timing.heartbeat_timeout_ms = timing["heartbeat_timeout_ms"] | config.timing.heartbeat_timeout_ms;
        config.timing.ws_initial_reconnect_ms = timing["ws_initial_reconnect_ms"] | config.timing.ws_initial_reconnect_ms;
        config.timing.ws_max_reconnect_ms = timing["ws_max_reconnect_ms"] | config.timing.ws_max_reconnect_ms;
    }

    // Parse security section
    if (doc.containsKey("security")) {
        JsonObject security = doc["security"];
        config.security.use_aes = security["use_aes"] | config.security.use_aes;
        config.security.aes_key = security["aes_key"] | config.security.aes_key;
    }

    // Parse power section
    if (doc.containsKey("power")) {
        JsonObject power = doc["power"];
        config.power.sleep_enabled = power["sleep_enabled"] | config.power.sleep_enabled;
        config.power.sleep_duration_min = power["sleep_duration_min"] | config.power.sleep_duration_min;
        config.power.battery_pin = power["battery_pin"] | config.power.battery_pin;
        config.power.usb_threshold_v = power["usb_threshold_v"] | config.power.usb_threshold_v;
    }

    // Parse logging section
    if (doc.containsKey("logging")) {
        JsonObject logging = doc["logging"];
        config.logging.default_level = logging["default_level"] | config.logging.default_level;
        config.logging.enable_test_commands = logging["enable_test_commands"] | config.logging.enable_test_commands;
    }

    loaded = true;
    logMessage("INFO", "CONFIG", "Configuration loaded successfully");
    
    // Log key config values for debugging
    char buf[256];
    snprintf(buf, sizeof(buf), "device_id=%s wifi_ssid=%s server=%s:%d",
             config.device.id.c_str(), config.wifi.ssid.c_str(),
             config.server.host.c_str(), config.server.port);
    logMessage("DEBUG", "CONFIG", "Loaded values", buf);

    return true;
}

bool ConfigManager::save() {
    logMessage("INFO", "CONFIG", "Saving configuration");

    File file = SPIFFS.open(CONFIG_FILE, "w");
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

    // WiFi section
    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["ssid"] = config.wifi.ssid;
    wifi["password"] = config.wifi.password;
    wifi["timeout_ms"] = config.wifi.timeout_ms;
    wifi["reconnect_interval_ms"] = config.wifi.reconnect_interval_ms;

    // Server section
    JsonObject server = doc["server"].to<JsonObject>();
    server["host"] = config.server.host;
    server["port"] = config.server.port;
    server["path"] = config.server.path;
    server["use_ssl"] = config.server.use_ssl;

    // Display section
    JsonObject display = doc["display"].to<JsonObject>();
    display["width"] = config.display.width;
    display["height"] = config.display.height;
    display["rotation"] = config.display.rotation;

    JsonObject pins = display["pins"].to<JsonObject>();
    pins["cs"] = config.display.pins.cs;
    pins["dc"] = config.display.pins.dc;
    pins["rst"] = config.display.pins.rst;
    pins["busy"] = config.display.pins.busy;
    pins["sclk"] = config.display.pins.sclk;
    pins["mosi"] = config.display.pins.mosi;

    // Timing section
    JsonObject timing = doc["timing"].to<JsonObject>();
    timing["heartbeat_interval_ms"] = config.timing.heartbeat_interval_ms;
    timing["heartbeat_timeout_ms"] = config.timing.heartbeat_timeout_ms;
    timing["ws_initial_reconnect_ms"] = config.timing.ws_initial_reconnect_ms;
    timing["ws_max_reconnect_ms"] = config.timing.ws_max_reconnect_ms;

    // Security section
    JsonObject security = doc["security"].to<JsonObject>();
    security["use_aes"] = config.security.use_aes;
    security["aes_key"] = config.security.aes_key;

    // Logging section
    JsonObject logging = doc["logging"].to<JsonObject>();
    logging["default_level"] = config.logging.default_level;
    logging["enable_test_commands"] = config.logging.enable_test_commands;

    String output;
    serializeJsonPretty(doc, output);
    return output;
}

void ConfigManager::reset() {
    logMessage("WARN", "CONFIG", "Resetting to defaults");
    setDefaults();
    loaded = true;
}
