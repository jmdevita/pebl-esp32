#include "WiFiCredentialManager.h"
#include "../config/ConfigManager.h"
#include <algorithm>  // For std::remove_if

// External log function (defined in main.cpp)
extern void logMessage(int level, const char* module, const char* message, const char* kvPairs);

// Log level constants (must match main.cpp LogLevel enum)
static const int LOG_ERROR = 0;
static const int LOG_WARN = 1;
static const int LOG_INFO = 2;
static const int LOG_DEBUG = 3;

// Minimum valid Unix timestamp (Jan 1, 2020 00:00:00 UTC)
// Used to detect if NTP has synced yet - times before this are considered invalid
static const uint32_t MIN_VALID_TIMESTAMP = 1577836800;

/**
 * Get a reliable timestamp for LRU tracking
 *
 * Before NTP sync, time(nullptr) returns seconds since boot (small values).
 * After NTP sync, it returns proper Unix timestamps (large values like 1705500000).
 *
 * To ensure consistent LRU ordering:
 * - If time is synced (>= Jan 1, 2020): use actual Unix timestamp
 * - If time not synced: use millis()/1000 + offset to create monotonic ordering
 *   The offset ensures pre-NTP timestamps are always older than post-NTP timestamps
 */
static uint32_t getReliableTimestamp() {
    uint32_t now = time(nullptr);
    if (now >= MIN_VALID_TIMESTAMP) {
        // NTP is synced, use real timestamp
        return now;
    }
    // NTP not synced yet - use millis-based timestamp
    // Add 1 to ensure it's never 0 (0 means "never connected" for eviction priority)
    return (millis() / 1000) + 1;
}

// Static member definitions
std::vector<WiFiCredential> WiFiCredentialManager::credentials;
bool WiFiCredentialManager::initialized = false;
const char* WiFiCredentialManager::NVS_NAMESPACE = "wifi_creds";

bool WiFiCredentialManager::begin() {
    if (initialized) {
        return true;
    }

    logMessage(LOG_INFO, "WIFI_CRED", "Initializing WiFi credential manager", nullptr);

    // Clear any existing credentials
    credentials.clear();

    // Load credentials from NVS first
    if (!loadFromNVS()) {
        logMessage(LOG_WARN, "WIFI_CRED", "No credentials found in NVS (first boot or cleared)", nullptr);
    }

    // Merge seed networks from config.json (marked as pinned)
    mergeSeedNetworks();

    initialized = true;

    char buf[64];
    snprintf(buf, sizeof(buf), "total_networks=%d", getNetworkCount());
    logMessage(LOG_INFO, "WIFI_CRED", "Credential manager initialized", buf);

    return true;
}

bool WiFiCredentialManager::loadFromNVS() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) {  // true = read-only
        logMessage(LOG_WARN, "WIFI_CRED", "Failed to open NVS namespace (may not exist yet)", nullptr);
        return false;
    }

    uint8_t count = prefs.getUChar("count", 0);
    if (count == 0) {
        prefs.end();
        return false;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "nvs_network_count=%d", count);
    logMessage(LOG_DEBUG, "WIFI_CRED", "Loading from NVS", buf);

    for (uint8_t i = 0; i < count && i < MAX_NETWORKS; i++) {
        char ssidKey[12], passKey[12], usedKey[12];
        snprintf(ssidKey, sizeof(ssidKey), "ssid_%d", i);
        snprintf(passKey, sizeof(passKey), "pass_%d", i);
        snprintf(usedKey, sizeof(usedKey), "used_%d", i);

        String ssid = prefs.getString(ssidKey, "");
        String password = prefs.getString(passKey, "");
        uint32_t lastUsed = prefs.getUInt(usedKey, 0);

        if (ssid.length() > 0) {
            WiFiCredential cred;
            cred.ssid = ssid;
            cred.password = password;
            cred.lastUsed = lastUsed;
            cred.isPinned = false;  // NVS credentials are never pinned
            credentials.push_back(cred);

            snprintf(buf, sizeof(buf), "loaded ssid=%s lastUsed=%lu", ssid.c_str(), lastUsed);
            logMessage(LOG_DEBUG, "WIFI_CRED", "NVS network loaded", buf);
        }
    }

    prefs.end();
    return credentials.size() > 0;
}

bool WiFiCredentialManager::saveToNVS() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false)) {  // false = read-write
        logMessage(LOG_ERROR, "WIFI_CRED", "Failed to open NVS for writing", nullptr);
        return false;
    }

    // Clear existing data first
    prefs.clear();

    // Count non-pinned credentials (pinned ones are in config.json)
    uint8_t count = 0;
    for (const auto& cred : credentials) {
        if (!cred.isPinned) {
            char ssidKey[12], passKey[12], usedKey[12];
            snprintf(ssidKey, sizeof(ssidKey), "ssid_%d", count);
            snprintf(passKey, sizeof(passKey), "pass_%d", count);
            snprintf(usedKey, sizeof(usedKey), "used_%d", count);

            prefs.putString(ssidKey, cred.ssid);
            prefs.putString(passKey, cred.password);
            prefs.putUInt(usedKey, cred.lastUsed);
            count++;

            char buf[64];
            snprintf(buf, sizeof(buf), "saved ssid=%s slot=%d", cred.ssid.c_str(), count - 1);
            logMessage(LOG_DEBUG, "WIFI_CRED", "NVS network saved", buf);
        }
    }

    prefs.putUChar("count", count);
    prefs.end();

    char buf[64];
    snprintf(buf, sizeof(buf), "saved_count=%d", count);
    logMessage(LOG_INFO, "WIFI_CRED", "Credentials saved to NVS", buf);

    return true;
}

void WiFiCredentialManager::mergeSeedNetworks() {
    const AppConfig& cfg = ConfigManager::getConfig();

    if (cfg.wifi.seed_networks.empty()) {
        logMessage(LOG_DEBUG, "WIFI_CRED", "No seed networks in config.json", nullptr);
        return;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "seed_count=%u", (unsigned int)cfg.wifi.seed_networks.size());
    logMessage(LOG_INFO, "WIFI_CRED", "Merging seed networks from config.json", buf);

    for (const auto& seed : cfg.wifi.seed_networks) {
        if (seed.ssid.isEmpty()) {
            continue;
        }

        int idx = findBySSID(seed.ssid);
        if (idx >= 0) {
            // Network already exists - update password and mark as pinned
            credentials[idx].password = seed.password;
            credentials[idx].isPinned = true;
            snprintf(buf, sizeof(buf), "ssid=%s (updated, now pinned)", seed.ssid.c_str());
            logMessage(LOG_DEBUG, "WIFI_CRED", "Seed network merged", buf);
        } else {
            // Check if we've reached MAX_NETWORKS before adding
            if (credentials.size() >= MAX_NETWORKS) {
                // Try to evict a non-pinned network to make room for seed
                if (!evictLRU()) {
                    // All slots are pinned or full - can't add more seeds
                    snprintf(buf, sizeof(buf), "ssid=%s (skipped, at capacity)", seed.ssid.c_str());
                    logMessage(LOG_WARN, "WIFI_CRED", "Cannot add seed network", buf);
                    continue;
                }
            }

            // Add new pinned network
            WiFiCredential cred;
            cred.ssid = seed.ssid;
            cred.password = seed.password;
            cred.lastUsed = 0;  // Never connected yet
            cred.isPinned = true;
            credentials.push_back(cred);
            snprintf(buf, sizeof(buf), "ssid=%s (new, pinned)", seed.ssid.c_str());
            logMessage(LOG_DEBUG, "WIFI_CRED", "Seed network added", buf);
        }
    }
}

bool WiFiCredentialManager::addNetwork(const String& ssid, const String& password) {
    if (!initialized) {
        logMessage(LOG_ERROR, "WIFI_CRED", "Cannot add network - not initialized", nullptr);
        return false;
    }

    // Validate SSID length (WiFi spec: max 32 chars)
    if (ssid.length() == 0 || ssid.length() > 32) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ssid_length=%u (must be 1-32)", (unsigned int)ssid.length());
        logMessage(LOG_WARN, "WIFI_CRED", "Invalid SSID length", buf);
        return false;
    }

    // Validate password length (WPA2 spec: 8-64 chars, or 0 for open networks)
    if (password.length() > 64) {
        char buf[64];
        snprintf(buf, sizeof(buf), "password_length=%u (max 64)", (unsigned int)password.length());
        logMessage(LOG_WARN, "WIFI_CRED", "Invalid password length", buf);
        return false;
    }

    char buf[128];

    // Check if network already exists
    int idx = findBySSID(ssid);
    if (idx >= 0) {
        // Update existing network
        credentials[idx].password = password;
        credentials[idx].lastUsed = getReliableTimestamp();
        snprintf(buf, sizeof(buf), "ssid=%s (password updated)", ssid.c_str());
        logMessage(LOG_INFO, "WIFI_CRED", "Network updated", buf);

        // Only save to NVS if it's not a pinned network
        // Pinned networks come from config.json, no need to persist password changes
        if (!credentials[idx].isPinned) {
            return saveToNVS();
        }
        return true;  // Success, but no NVS save needed for pinned
    }

    // If at capacity, try to evict oldest non-pinned network
    if (credentials.size() >= MAX_NETWORKS) {
        if (!evictLRU()) {
            // All slots are pinned - can't persist this network
            snprintf(buf, sizeof(buf), "ssid=%s (all slots pinned, session-only)", ssid.c_str());
            logMessage(LOG_WARN, "WIFI_CRED", "Cannot persist network", buf);
            return false;
        }
    }

    // Add new network
    WiFiCredential cred;
    cred.ssid = ssid;
    cred.password = password;
    cred.lastUsed = getReliableTimestamp();
    cred.isPinned = false;
    credentials.push_back(cred);

    snprintf(buf, sizeof(buf), "ssid=%s total_networks=%u", ssid.c_str(), (unsigned int)credentials.size());
    logMessage(LOG_INFO, "WIFI_CRED", "Network added", buf);

    return saveToNVS();
}

void WiFiCredentialManager::updateLastUsed(const String& ssid) {
    int idx = findBySSID(ssid);
    if (idx >= 0) {
        uint32_t now = getReliableTimestamp();
        credentials[idx].lastUsed = now;

        char buf[64];
        snprintf(buf, sizeof(buf), "ssid=%s timestamp=%lu", ssid.c_str(), (unsigned long)now);
        logMessage(LOG_DEBUG, "WIFI_CRED", "Updated lastUsed", buf);

        // Only save to NVS if it's not a pinned network
        // (pinned networks don't need lastUsed persisted since they're never evicted)
        if (!credentials[idx].isPinned) {
            saveToNVS();
        }
    }
}

const std::vector<WiFiCredential>& WiFiCredentialManager::getCredentials() {
    return credentials;
}

uint8_t WiFiCredentialManager::getNetworkCount() {
    return credentials.size();
}

void WiFiCredentialManager::clearNVS() {
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, false)) {
        prefs.clear();
        prefs.end();
        logMessage(LOG_INFO, "WIFI_CRED", "NVS credentials cleared", nullptr);
    }

    // Remove non-pinned credentials from memory
    // Keep pinned ones since they come from config.json
    credentials.erase(
        std::remove_if(credentials.begin(), credentials.end(),
                       [](const WiFiCredential& c) { return !c.isPinned; }),
        credentials.end()
    );
}

int WiFiCredentialManager::findBySSID(const String& ssid) {
    for (size_t i = 0; i < credentials.size(); i++) {
        if (credentials[i].ssid == ssid) {
            return i;
        }
    }
    return -1;
}

bool WiFiCredentialManager::evictLRU() {
    // Find oldest non-pinned network
    // Networks with lastUsed=0 (never connected) are prioritized for eviction
    int evictIdx = -1;
    uint32_t oldestTime = UINT32_MAX;

    for (size_t i = 0; i < credentials.size(); i++) {
        if (!credentials[i].isPinned) {
            // Prioritize lastUsed=0 (never successfully connected)
            if (credentials[i].lastUsed == 0) {
                evictIdx = i;
                break;  // Evict immediately if never used
            }
            if (credentials[i].lastUsed < oldestTime) {
                oldestTime = credentials[i].lastUsed;
                evictIdx = i;
            }
        }
    }

    if (evictIdx < 0) {
        logMessage(LOG_WARN, "WIFI_CRED", "Cannot evict - all networks are pinned", nullptr);
        return false;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "ssid=%s lastUsed=%lu",
             credentials[evictIdx].ssid.c_str(), credentials[evictIdx].lastUsed);
    logMessage(LOG_INFO, "WIFI_CRED", "Evicting LRU network", buf);

    credentials.erase(credentials.begin() + evictIdx);
    return true;
}

void WiFiCredentialManager::debugPrint() {
    Serial.println("\n=== WiFi Credentials ===");
    Serial.printf("Total networks: %d\n", credentials.size());

    for (size_t i = 0; i < credentials.size(); i++) {
        const auto& cred = credentials[i];
        Serial.printf("[%d] SSID: %-20s | Pinned: %s | LastUsed: %lu\n",
                      i,
                      cred.ssid.c_str(),
                      cred.isPinned ? "Yes" : "No",
                      cred.lastUsed);
    }
    Serial.println("========================\n");
}
