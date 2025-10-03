# 📡 WiFi Power Management & Provisioning Improvements

## Overview

This document outlines planned WiFi enhancements for the ESP32 Arduino client, including adaptive TX power management and WiFi provisioning via Access Point mode. These features will improve battery life and user experience when setting up or relocating devices.

---

## 🔋 Feature 1: Adaptive TX Power Management

### Goal
Automatically adjust WiFi transmission power based on signal strength to optimize battery life while maintaining reliable connectivity.

### Battery Impact
- **LOW power (11dBm)**: ~40% battery savings vs HIGH
- **MEDIUM power (15dBm)**: ~25% battery savings vs HIGH
- **HIGH power (19.5dBm)**: Default, maximum range

### Expected Battery Life (2000mAh battery)
- **Strong signal (LOW)**: ~7 days (vs ~5 days at HIGH)
- **Medium signal (MEDIUM)**: ~6 days
- **Weak signal (HIGH)**: ~5 days (baseline)

---

## Implementation Design

### A. RTC Memory State (Persists across deep sleep)

```cpp
// Add after line 177 in main.cpp
RTC_DATA_ATTR struct {
    wifi_power_t currentPower;       // Current TX power level
    uint8_t consecutiveFailures;     // Connection failures at current level
    bool hasEscalated;               // True if power has been increased this session
    uint8_t totalFailedWakes;        // Total failed wake cycles
    bool wifiDisabledMode;           // True if in low-power fallback mode
} wifiState = {
    WIFI_POWER_11dBm,  // Always start at LOW on boot
    0,
    false,
    0,
    false
};
```

### B. Configuration Settings

Add to `ConfigManager.h`:

```cpp
// WiFi settings
struct {
    String ssid;
    String password;
    uint32_t timeout_ms;
    uint32_t reconnect_interval_ms;

    // Adaptive TX power settings
    bool adaptive_tx_power;          // Enable/disable adaptive power
    String force_tx_power;           // Override: "LOW", "MEDIUM", "HIGH", or empty
    uint8_t escalation_threshold;    // Failures before escalating (default: 3)
    uint8_t max_failed_wakes;        // Failed wakes before entering low-power mode (default: 20)
} wifi;
```

Add to `ConfigManager.cpp` defaults:

```cpp
config.wifi.adaptive_tx_power = true;
config.wifi.force_tx_power = "";
config.wifi.escalation_threshold = 3;
config.wifi.max_failed_wakes = 20;
```

---

## State Machine Logic

### On Boot (Power Cycle)
```
wifiState.currentPower = WIFI_POWER_11dBm  // Reset to LOW
wifiState.consecutiveFailures = 0
wifiState.hasEscalated = false
wifiState.totalFailedWakes = 0
wifiState.wifiDisabledMode = false
```

### On WiFi Connection Attempt

```
1. Check if user forced specific power level
   → If yes: Use forced level, skip adaptive logic

2. Check if in wifiDisabledMode
   → If yes: Skip connection, sleep for extended period

3. Set WiFi.setTxPower(wifiState.currentPower)

4. Attempt connection with timeout

5. If SUCCESS:
   - Reset consecutiveFailures = 0
   - Reset totalFailedWakes = 0
   - Stay at current power level (sticky)
   - Log: "Connected at [POWER] (RSSI: [value])"

6. If FAILURE:
   - consecutiveFailures++
   - totalFailedWakes++

   6a. Check escalation threshold
       If consecutiveFailures >= escalation_threshold:
         - If at LOW → Escalate to MEDIUM
         - If at MEDIUM → Escalate to HIGH
         - If at HIGH → Log error, give up escalation
         - Reset consecutiveFailures = 0

   6b. Check total failed wakes
       If totalFailedWakes >= max_failed_wakes:
         - Enter wifiDisabledMode
         - Log: "Entering low-power mode - WiFi disabled"
         - Sleep for 60 minutes instead of 5 minutes
         - Every 6 days: Retry (reset to LOW, exit disabled mode)
```

### Power Escalation Path

```
Boot → LOW (11dBm)
  ↓ (3 failures)
MEDIUM (15dBm)
  ↓ (3 failures)
HIGH (19.5dBm)
  ↓ (3 failures)
Give up - Not a TX power issue
```

---

## Testing & Debug Commands

### Manual Testing Commands

Add to `processSerialCommand()`:

```cpp
// Test different TX power levels
TEST:WIFI:TXPOWER:LOW       // Force LOW power
TEST:WIFI:TXPOWER:MEDIUM    // Force MEDIUM power
TEST:WIFI:TXPOWER:HIGH      // Force HIGH power
TEST:WIFI:TXPOWER:AUTO      // Re-enable adaptive

// Show current state
TEST:WIFI:TXPOWER:STATUS    // Display current power, RSSI, failure counts

// Reset state
TEST:WIFI:TXPOWER:RESET     // Reset to LOW, clear failures

// Signal strength test
TEST:WIFI:RSSI              // Show current RSSI and recommendation
```

### RSSI Interpretation Guide

```
RSSI > -60 dBm:  Excellent - LOW power safe (40% battery savings)
RSSI -60 to -70: Good - MEDIUM power recommended (25% savings)
RSSI -70 to -80: Fair - HIGH power recommended (stay at default)
RSSI < -80 dBm:  Poor - HIGH power required, consider relocating device
```

### Example Testing Session

```bash
# Enable debug features
pio device monitor --environment [your_env]

# Test baseline at HIGH
> TEST:WIFI:TXPOWER:HIGH
TX Power: HIGH
RSSI: -54 dBm
Signal Quality: Excellent
Recommendation: LOW safe, 40% battery savings possible

# Test LOW power
> TEST:WIFI:TXPOWER:LOW
TX Power: LOW
RSSI: -56 dBm
Signal Quality: Excellent
Connection stable: YES
Recommendation: Use LOW for battery savings

# Enable automatic mode
> TEST:WIFI:TXPOWER:AUTO
Adaptive TX power enabled - will start at LOW on next boot
```

---

## Implementation Checklist

### Phase 1: Core Adaptive Power (Estimated: 2 hours)
- [ ] Add RTC memory struct to `main.cpp`
- [ ] Add config settings to `ConfigManager.h/cpp`
- [ ] Implement power escalation logic in `connectWiFi()`
- [ ] Add helper function `getTxPowerName()`
- [ ] Add logging for power changes
- [ ] Test escalation path manually

### Phase 2: Low-Power Fallback Mode (Estimated: 1 hour)
- [ ] Add `wifiDisabledMode` logic
- [ ] Implement extended sleep duration (60 min)
- [ ] Add periodic retry (every 6 days)
- [ ] Create "WiFi Unavailable" display message
- [ ] Test failure recovery

### Phase 3: Debug Commands (Estimated: 1 hour)
- [ ] Add `TEST:WIFI:TXPOWER:*` commands
- [ ] Add `TEST:WIFI:RSSI` command
- [ ] Add status display command
- [ ] Document testing procedure

### Phase 4: Testing & Validation (Estimated: 2 hours)
- [ ] Test at various distances from router
- [ ] Verify battery consumption at each power level
- [ ] Test escalation/de-escalation scenarios
- [ ] Test low-power fallback mode
- [ ] Validate reboot behavior

**Total Estimated Time**: 6 hours

---

## 📶 Feature 2: WiFi Provisioning via Access Point

### Goal
Allow users to configure WiFi credentials without hardcoding them or needing to reflash the device. Useful for:
- **Initial setup** (fresh device)
- **WiFi unavailable** (device moved to new location)
- **Network change** (router replaced, password changed)

### User Experience

#### Scenario 1: First-Time Setup
```
1. Device boots with no WiFi configured
2. Displays: "Setup Required - Connect to: SlackReactions-XXXX"
3. User connects phone/laptop to device's WiFi AP
4. Browser opens captive portal at 192.168.4.1
5. User enters WiFi credentials + device name
6. Device saves config and reboots
7. Device connects to configured WiFi
```

#### Scenario 2: WiFi Unavailable (Fallback)
```
1. Device fails to connect for 20 consecutive wake cycles (~100 min)
2. Enters low-power mode (1 hour sleep cycles)
3. After 6 days of failures: Enters provisioning mode
4. Displays: "WiFi Setup Mode - Connect to: SlackReactions-XXXX"
5. User reconfigures WiFi
6. Device reboots and connects
```

#### Scenario 3: Manual Trigger
```
1. User holds GPIO39 button for 5 seconds during boot
2. Device enters provisioning mode
3. User reconfigures
```

---

## Implementation Design

### A. Provisioning Trigger Conditions

```cpp
bool shouldEnterProvisioningMode() {
    // 1. No WiFi configured (first boot)
    if (config.wifi.ssid.isEmpty()) {
        return true;
    }

    // 2. Button held during boot (manual trigger)
    if (digitalRead(PROVISION_BUTTON_PIN) == LOW && bootCount == 0) {
        unsigned long pressStart = millis();
        while (digitalRead(PROVISION_BUTTON_PIN) == LOW) {
            if (millis() - pressStart > 5000) {
                return true;  // Held for 5 seconds
            }
        }
    }

    // 3. Too many failed connections (auto-trigger)
    if (wifiState.totalFailedWakes >= MAX_FAILED_WAKES_BEFORE_PROVISION) {
        return true;
    }

    return false;
}
```

### B. Access Point Configuration

```cpp
void startProvisioningMode() {
    logMessage(LOG_INFO, "PROVISION", "Entering WiFi provisioning mode");

    // Generate unique SSID with device ID
    String apSSID = "SlackReactions-" + getDeviceShortID();  // e.g., "SlackReactions-A8B4"
    String apPassword = "setup123";  // Default password

    // Start Access Point
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apSSID.c_str(), apPassword.c_str());

    IPAddress apIP = WiFi.softAPIP();

    // Update display
    DisplayManager::showMessage(
        "WiFi Setup Mode",
        "Connect to:",
        apSSID.c_str(),
        String("Password: " + apPassword).c_str()
    );

    // Start web server for configuration
    startProvisioningWebServer();

    // Wait for configuration (with timeout)
    unsigned long startTime = millis();
    while (millis() - startTime < PROVISION_TIMEOUT_MS) {
        handleProvisioningClient();
        delay(10);
    }
}

String getDeviceShortID() {
    // Last 4 chars of MAC address
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char shortID[5];
    snprintf(shortID, sizeof(shortID), "%02X%02X", mac[4], mac[5]);
    return String(shortID);
}
```

### C. Web Server for Configuration

```cpp
#include <WebServer.h>
#include <DNSServer.h>

WebServer provisionServer(80);
DNSServer dnsServer;

void startProvisioningWebServer() {
    // Captive portal - redirect all DNS requests to AP IP
    dnsServer.start(53, "*", WiFi.softAPIP());

    // Serve configuration page
    provisionServer.on("/", HTTP_GET, handleRoot);
    provisionServer.on("/save", HTTP_POST, handleSave);
    provisionServer.on("/status", HTTP_GET, handleStatus);

    provisionServer.begin();
    logMessage(LOG_INFO, "PROVISION", "Web server started", WiFi.softAPIP().toString().c_str());
}

void handleRoot() {
    String html = R"(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Slack Reactions Setup</title>
    <style>
        body { font-family: Arial; margin: 20px; max-width: 500px; }
        input { width: 100%; padding: 10px; margin: 5px 0; box-sizing: border-box; }
        button { width: 100%; padding: 15px; background: #4CAF50; color: white; border: none; cursor: pointer; }
        button:hover { background: #45a049; }
        .info { background: #e7f3fe; padding: 10px; margin: 10px 0; border-left: 4px solid #2196F3; }
    </style>
</head>
<body>
    <h2>🎉 Slack Reactions Setup</h2>
    <div class="info">
        Configure your device to connect to WiFi and the Slack Reactions server.
    </div>

    <form action="/save" method="POST">
        <h3>WiFi Settings</h3>
        <label>Network Name (SSID):</label>
        <input type="text" name="ssid" required placeholder="Your WiFi Network">

        <label>Password:</label>
        <input type="password" name="password" required placeholder="WiFi Password">

        <h3>Device Settings</h3>
        <label>Device Name:</label>
        <input type="text" name="device_name" required placeholder="My Desk Display">

        <label>Device ID:</label>
        <input type="text" name="device_id" value=")" + ConfigManager::getConfig().device.id + R"(" readonly>

        <h3>Server Settings (Optional)</h3>
        <label>Server URL:</label>
        <input type="text" name="server_host" value="slack-reactions.devita.dev" placeholder="slack-reactions.devita.dev">

        <br><br>
        <button type="submit">💾 Save & Restart</button>
    </form>

    <script>
        // Scan for WiFi networks (future enhancement)
        // Display list of available networks for user to select
    </script>
</body>
</html>
    )";

    provisionServer.send(200, "text/html", html);
}

void handleSave() {
    // Get form data
    String ssid = provisionServer.arg("ssid");
    String password = provisionServer.arg("password");
    String deviceName = provisionServer.arg("device_name");
    String serverHost = provisionServer.arg("server_host");

    // Validate inputs
    if (ssid.isEmpty() || password.isEmpty()) {
        provisionServer.send(400, "text/plain", "Missing required fields");
        return;
    }

    // Update config
    AppConfig& cfg = ConfigManager::getMutableConfig();
    cfg.wifi.ssid = ssid;
    cfg.wifi.password = password;
    cfg.device.name = deviceName;
    if (!serverHost.isEmpty()) {
        cfg.server.host = serverHost;
    }

    // Save to SPIFFS
    if (ConfigManager::save()) {
        String successHtml = R"(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <meta http-equiv="refresh" content="5;url=/" />
    <title>Saved</title>
    <style>
        body { font-family: Arial; margin: 20px; text-align: center; }
        .success { color: #4CAF50; font-size: 24px; }
    </style>
</head>
<body>
    <div class="success">✅ Configuration Saved!</div>
    <p>Device will restart in 5 seconds...</p>
    <p>Connect to your WiFi network to continue.</p>
</body>
</html>
        )";

        provisionServer.send(200, "text/html", successHtml);

        // Wait for response to send, then restart
        delay(3000);
        ESP.restart();
    } else {
        provisionServer.send(500, "text/plain", "Failed to save configuration");
    }
}

void handleStatus() {
    String json = "{";
    json += "\"ssid\":\"" + ConfigManager::getConfig().wifi.ssid + "\",";
    json += "\"device_id\":\"" + ConfigManager::getConfig().device.id + "\",";
    json += "\"version\":\"2.0\"";
    json += "}";

    provisionServer.send(200, "application/json", json);
}

void handleProvisioningClient() {
    dnsServer.processNextRequest();
    provisionServer.handleClient();
}
```

---

## Enhanced Features (Future)

### 1. WiFi Network Scanner
Display list of available networks for user to select:

```cpp
void scanWiFiNetworks() {
    int n = WiFi.scanNetworks();
    String networksJson = "[";

    for (int i = 0; i < n; i++) {
        if (i > 0) networksJson += ",";
        networksJson += "{";
        networksJson += "\"ssid\":\"" + WiFi.SSID(i) + "\",";
        networksJson += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        networksJson += "\"encryption\":\"" + String(WiFi.encryptionType(i)) + "\"";
        networksJson += "}";
    }

    networksJson += "]";
    return networksJson;
}

// Add endpoint: /scan
provisionServer.on("/scan", HTTP_GET, []() {
    provisionServer.send(200, "application/json", scanWiFiNetworks());
});
```

### 2. Multi-Network Support
Allow configuration of multiple WiFi networks with priority:

```cpp
struct {
    String networks[5];  // Array of SSIDs
    String passwords[5];
    uint8_t priorities[5];
} wifiNetworks;

// Try networks in priority order
bool connectToAnyNetwork() {
    for (int i = 0; i < 5; i++) {
        if (!wifiNetworks.networks[i].isEmpty()) {
            WiFi.begin(wifiNetworks.networks[i].c_str(),
                      wifiNetworks.passwords[i].c_str());
            if (waitForConnection()) {
                return true;
            }
        }
    }
    return false;
}
```

### 3. OTA Configuration Updates
Allow remote config updates without provisioning mode:

```cpp
// Add WebSocket command handler
if (msg["type"] == "config_update") {
    updateConfig(msg["config"]);
    ConfigManager::save();
    ESP.restart();
}
```

---

## Implementation Checklist

### Phase 1: Basic Provisioning (Estimated: 4 hours)
- [ ] Add provision trigger logic
- [ ] Implement AP mode startup
- [ ] Create web server with config form
- [ ] Add DNS server for captive portal
- [ ] Test configuration save/load
- [ ] Test device restart after provisioning

### Phase 2: Enhanced UI (Estimated: 2 hours)
- [ ] Improve HTML/CSS for mobile responsiveness
- [ ] Add WiFi network scanner
- [ ] Add signal strength indicators
- [ ] Add validation and error handling
- [ ] Test on multiple devices (phone, tablet, laptop)

### Phase 3: Multi-Network Support (Estimated: 2 hours)
- [ ] Add support for multiple WiFi credentials
- [ ] Implement priority-based connection
- [ ] Update provisioning UI for multiple networks
- [ ] Test network failover

### Phase 4: Integration (Estimated: 2 hours)
- [ ] Integrate with adaptive TX power system
- [ ] Add provision mode trigger from failed connections
- [ ] Update display messages
- [ ] Add button-hold detection for manual trigger
- [ ] End-to-end testing

**Total Estimated Time**: 10 hours

---

## Dependencies

### Required Libraries
```ini
# Add to platformio.ini
lib_deps =
    ...existing deps...
    ESP Async WebServer  # For non-blocking web server (optional, faster)
    DNSServer           # For captive portal (built-in)
```

### Memory Requirements
- **Web server**: ~15KB RAM
- **DNS server**: ~2KB RAM
- **HTML pages**: ~5KB Flash
- **Total overhead**: ~22KB RAM, ~5KB Flash

---

## Security Considerations

### 1. Access Point Security
- Use WPA2 encryption (default password "setup123")
- Timeout provisioning mode after 10 minutes
- Disable AP mode after successful config

### 2. Config Storage
- Store WiFi passwords encrypted in SPIFFS
- Use preferences library with encryption
- Clear passwords from memory after use

### 3. Web Interface
- Add basic authentication option
- CSRF token for form submissions
- Rate limiting on save endpoint

---

## Testing Scenarios

### Test 1: First-Time Setup
```
1. Flash fresh device with no config
2. Boot device
3. Verify AP mode starts automatically
4. Connect to AP from phone
5. Verify captive portal opens
6. Enter WiFi credentials
7. Verify device connects to WiFi
8. Verify config persists after reboot
```

### Test 2: Failed Connection Recovery
```
1. Configure device with valid WiFi
2. Move device out of WiFi range
3. Let device fail 20+ wake cycles
4. Verify enters low-power mode
5. Wait for provisioning trigger (6 days in fast-forward)
6. Verify AP mode starts
7. Reconfigure WiFi
8. Verify device connects
```

### Test 3: Manual Provisioning
```
1. Hold GPIO39 button during boot
2. Verify AP mode starts after 5 seconds
3. Reconfigure
4. Verify normal operation resumes
```

### Test 4: Multiple Networks
```
1. Configure 3 different WiFi networks
2. Test device at each location
3. Verify connects to available network
4. Verify priority ordering works
```

---

## Future Enhancements

1. **Bluetooth Provisioning**: Use BLE instead of WiFi AP (lower power)
2. **QR Code Setup**: Generate QR code for easy mobile configuration
3. **Cloud Provisioning**: Register device via server, download config
4. **Backup/Restore**: Export/import configuration as JSON file
5. **Remote Diagnostics**: View logs and status via web interface

---

## References

- [ESP32 WiFi API Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_wifi.html)
- [ESP32 WebServer Library](https://github.com/espressif/arduino-esp32/tree/master/libraries/WebServer)
- [WiFiManager Library (inspiration)](https://github.com/tzapu/WiFiManager)
- [ESP32 Deep Sleep & RTC Memory](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/sleep_modes.html)

---

*Last Updated: October 2025*
*Status: Design Document - Not Yet Implemented*
*Priority: Medium (Nice to have for v3.0)*
