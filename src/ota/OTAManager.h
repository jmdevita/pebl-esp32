#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/pk.h>
#include <mbedtls/base64.h>
#include "esp_ota_ops.h"
#include "esp_app_format.h"

/**
 * OTA (Over-The-Air) firmware update manager for ESP32
 *
 * Features:
 * - HTTPS firmware download with SHA-256 verification
 * - Dual OTA partition support (app0/app1)
 * - Automatic rollback on failed boot
 * - Progress callback for user feedback
 * - Boot validation and partition management
 */
class OTAManager {
public:
    enum class OTAStatus {
        IDLE,
        CHECKING,
        DOWNLOADING,
        VERIFYING,
        INSTALLING,
        SUCCESS,
        FAILED,
        ROLLBACK
    };

    struct FirmwareInfo {
        String version;
        String downloadUrl;
        String sha256Hash;
        String signature;  // ECDSA signature over hash
        uint32_t size;
        bool required;     // Force update flag
        String changelog;
    };

    /**
     * Constructor
     * @param serverUrl Base URL of update server (e.g., "https://server.com")
     * @param deviceId Unique device identifier for authentication
     */
    OTAManager(const String& serverUrl, const String& deviceId);

    /**
     * Check if firmware update is available
     * @param info Output parameter filled with firmware information if update available
     * @return true if update is available, false if already up-to-date
     */
    bool checkForUpdate(FirmwareInfo& info);

    /**
     * Download and install firmware update
     * @param info Firmware information from checkForUpdate()
     * @param progressCallback Optional callback for progress updates (current, total)
     * @return true if update successful, false on failure
     */
    bool downloadAndInstall(const FirmwareInfo& info,
                           void (*progressCallback)(size_t current, size_t total) = nullptr);

    /**
     * Verify ECDSA signature of firmware
     * @param hash SHA-256 hash of firmware
     * @param signature Base64-encoded ECDSA signature
     * @return true if signature valid, false otherwise
     */
    bool verifySignature(const uint8_t* hash, const String& signature);

    /**
     * Rollback to factory partition (if available)
     */
    void rollbackToFactory();

    /**
     * Check if device just booted from OTA update and mark as valid
     * Should be called early in setup()
     * @return true if boot validation performed, false otherwise
     */
    bool checkBootValidation();

    /**
     * Save pre-update version info to NVS before reboot.
     * Called by downloadAndInstall() before returning success.
     */
    void savePendingUpdate(const String& oldVersion, const String& newVersion);

    /**
     * Report successful OTA update to server after boot validation.
     * Reads saved versions from NVS, POSTs to /api/firmware/stats,
     * and clears NVS entries on success.
     * @return true if report sent successfully (or no pending report)
     */
    bool reportUpdateSuccess();

    /**
     * Get current OTA status
     */
    OTAStatus getStatus() const { return status; }

    /**
     * Get last error message
     */
    String getLastError() const { return lastError; }

    /**
     * Get current firmware version
     */
    String getCurrentVersion() const { return currentVersion; }

private:
    String serverUrl;
    String deviceId;
    String currentVersion;
    OTAStatus status;
    String lastError;

    // ECDSA P-256 public key for firmware signature verification
    // Generated using scripts/generate_ota_keys.sh
    // Fingerprint: 882517373e7bac2b9f552445d3f8269f15a35998a4c7f52866cd56810620d752
    static constexpr const char* FIRMWARE_PUBLIC_KEY = R"(
-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEpaUoIStuizMGtDR9hJ7SeY8gX9m4
2frDuv7haRz+O67nPYZ/VxMc5R/q4spN3bk315CnIOChSx18gUFoYQ7HFQ==
-----END PUBLIC KEY-----
)";
};
