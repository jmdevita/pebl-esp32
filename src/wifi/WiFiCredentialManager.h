#ifndef WIFI_CREDENTIAL_MANAGER_H
#define WIFI_CREDENTIAL_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>
#include <WiFiMulti.h>
#include <vector>

/**
 * WiFi credential storage for multi-network support (iPhone-like behavior)
 *
 * Features:
 * - Store up to 5 WiFi networks with LRU eviction
 * - NVS persistence across reboots
 * - Seed networks from config.json (pinned, never evicted)
 * - Automatic password update for existing networks
 */
struct WiFiCredential {
    String ssid;           // Max 32 chars (WiFi spec limit)
    String password;       // Max 64 chars (WPA2 spec limit)
    uint32_t lastUsed;     // Unix timestamp for LRU ordering
    bool isPinned;         // true = from config.json seed_networks (never evicted)
};

class WiFiCredentialManager {
public:
    static const uint8_t MAX_NETWORKS = 5;
    static const char* NVS_NAMESPACE;  // "wifi_creds"

    /**
     * Initialize the credential manager
     * - Loads credentials from NVS
     * - Merges seed networks from config.json (marked as pinned)
     *
     * @return true if initialization succeeded
     */
    static bool begin();

    /**
     * Add or update a WiFi network credential
     * - If SSID exists: updates password and lastUsed timestamp
     * - If at capacity (5 networks): evicts oldest non-pinned network
     * - If all 5 slots are pinned: network works for session but won't persist
     *
     * @param ssid Network SSID (max 32 chars)
     * @param password Network password (max 64 chars)
     * @return true if saved to NVS, false if all slots pinned (session-only)
     */
    static bool addNetwork(const String& ssid, const String& password);

    /**
     * Update lastUsed timestamp for a network (call on successful connection)
     * This maintains LRU ordering for eviction decisions.
     *
     * @param ssid Network SSID that successfully connected
     */
    static void updateLastUsed(const String& ssid);

    /**
     * Get all stored credentials (NVS + pinned seed networks)
     * Use this to feed WiFiMulti.addAP() for each credential.
     *
     * @return Reference to credentials vector
     */
    static const std::vector<WiFiCredential>& getCredentials();

    /**
     * Get number of stored networks (NVS + pinned)
     *
     * @return Network count (0-5)
     */
    static uint8_t getNetworkCount();

    /**
     * Clear all credentials from NVS (does not affect pinned seeds)
     * Used for factory reset scenarios.
     */
    static void clearNVS();

    /**
     * Print debug info to Serial for diagnostics
     * Shows all stored networks with pinned status and lastUsed times.
     */
    static void debugPrint();

private:
    static std::vector<WiFiCredential> credentials;
    static bool initialized;

    /**
     * Load credentials from NVS
     * NVS schema: count, ssid_0..4, pass_0..4, used_0..4
     */
    static bool loadFromNVS();

    /**
     * Save non-pinned credentials to NVS
     * Pinned networks are stored in config.json, not NVS.
     */
    static bool saveToNVS();

    /**
     * Find credential index by SSID
     *
     * @param ssid Network SSID to find
     * @return Index in credentials vector, or -1 if not found
     */
    static int findBySSID(const String& ssid);

    /**
     * Evict the oldest non-pinned network (lowest lastUsed timestamp)
     * Networks with lastUsed=0 (never connected) are evicted first.
     *
     * @return true if a network was evicted, false if all are pinned
     */
    static bool evictLRU();

    /**
     * Merge seed networks from config.json into credentials
     * Seed networks are marked as pinned and won't be evicted.
     * If SSID already exists, it becomes pinned (password from seed takes precedence).
     */
    static void mergeSeedNetworks();
};

#endif // WIFI_CREDENTIAL_MANAGER_H
