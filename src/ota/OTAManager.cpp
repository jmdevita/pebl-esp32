#include "OTAManager.h"
#include "config/ConfigManager.h"
#include <Preferences.h>

OTAManager::OTAManager(const String& serverUrl, const String& deviceId)
    : serverUrl(serverUrl), deviceId(deviceId), status(OTAStatus::IDLE) {

    // Get current version from APP_VERSION define (set by scripts/set_version.py)
    // This is more reliable than esp_app_get_description()->version which may
    // return a git hash if PROJECT_VER isn't properly propagated through PlatformIO
#ifdef APP_VERSION
    currentVersion = String(APP_VERSION);
#else
    // Fallback to app descriptor if APP_VERSION not defined
    const esp_app_desc_t* app_desc = esp_app_get_description();
    currentVersion = String(app_desc->version);
#endif
}

bool OTAManager::checkForUpdate(FirmwareInfo& info) {
    status = OTAStatus::CHECKING;

    // Check if OTA updates are enabled in config (allows users to opt out)
    if (!ConfigManager::getConfig().ota.enabled) {
        lastError = "OTA disabled in config";
        status = OTAStatus::IDLE;
        Serial.println("[OTA] Updates disabled via config.json (ota.enabled = false)");
        return false;
    }

    // Check if display_variant is configured (required for OTA)
    String variant = ConfigManager::getDisplayVariant();
    if (variant.isEmpty()) {
        lastError = "display_variant not configured - OTA disabled";
        status = OTAStatus::FAILED;
        Serial.println("[OTA] display_variant not set in config.json - cannot check for updates");
        return false;
    }

    // Build full URL with query parameters including display_variant and current version
    String url = serverUrl + "/api/firmware/version?device_id=" + deviceId
                 + "&display_variant=" + variant
                 + "&current_version=" + currentVersion;

    // Create fresh HTTPS client instances
    WiFiClientSecure secureClient;
    HTTPClient httpClient;

    // WORKAROUND: Skip certificate validation for Cloudflare tunnel
    //
    // Root cause: Server uses ECDSA certificates via Cloudflare tunnel, which causes
    // mbedTLS certificate chain validation to fail with error -12288 (X509 verification failed).
    // The timezone API (ipgeolocation.io) uses RSA certificates and validates successfully.
    //
    // Investigation showed:
    // - OTA server: EC P-256 keys, Google Trust Services WE1 intermediate, 3-cert chain
    // - Timezone API: RSA 2048 keys, Let's Encrypt R11 intermediate, 2-cert chain
    // - Desktop browsers and curl validate both chains successfully
    // - ESP32 mbedTLS validates RSA chain but fails on ECDSA chain
    //
    // Connection is still encrypted with TLS 1.3, just without certificate validation.
    // This matches WebSocket behavior which uses beginSSL() without validation.
    //
    // FUTURE: Investigate proper fix - see docs/SSL_CERT_VALIDATION_ISSUE.md
    // Options: 1) Update mbedTLS config for ECDSA, 2) Force Cloudflare to use RSA certs
    secureClient.setInsecure();
    Serial.println("[OTA] Skipping cert check (encrypted + signed, trusted network)");

    httpClient.begin(secureClient, url);

    // FUTURE: OTA API authentication not yet implemented
    // When added, enable this to require Bearer token for firmware downloads:
    // if (ConfigManager::getConfig().security.api_token.length() > 0) {
    //     httpClient.addHeader("Authorization", "Bearer " + ConfigManager::getConfig().security.api_token);
    // }
    // See server/app/api/firmware.py for corresponding server-side implementation

    httpClient.setTimeout(10000);  // 10 second timeout

    int httpCode = httpClient.GET();

    // Server now returns 200 for all normal cases (including no firmware available)
    // Only non-200 codes are actual errors (400 bad request, 403 forbidden, 500 server error)
    if (httpCode != HTTP_CODE_OK) {
        lastError = "Server returned: " + String(httpCode);
        status = OTAStatus::FAILED;
        httpClient.end();
        return false;
    }

    String payload = httpClient.getString();
    httpClient.end();

    // Parse JSON response
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
        lastError = "JSON parse error: " + String(error.c_str());
        status = OTAStatus::FAILED;
        return false;
    }

    // New JSON format: Always check "update_available" and "status" fields
    // Three possible states:
    // 1. "no_firmware_configured" - Admin hasn't uploaded firmware yet
    // 2. "up_to_date" - Device is on latest version
    // 3. "update_available" - New firmware ready to download

    bool updateAvailable = doc["update_available"] | false;
    String statusStr = doc["status"] | "unknown";
    String message = doc["message"] | "";

    if (!updateAvailable) {
        status = OTAStatus::IDLE;
        lastError = "";  // Clear error on success

        // Log the specific reason (no_firmware_configured vs up_to_date)
        if (statusStr == "no_firmware_configured") {
            Serial.println("[OTA] No firmware configured on server");
        } else if (statusStr == "up_to_date") {
            String current = doc["current_version"] | "unknown";
            String latest = doc["latest_version"] | "unknown";
            Serial.printf("[OTA] Firmware is up to date: %s\n", latest.c_str());
        }

        return false;  // No update needed
    }

    // Update is available - extract firmware metadata
    info.version = doc["latest_version"].as<String>();
    info.downloadUrl = doc["download_url"].as<String>();
    info.sha256Hash = doc["sha256"].as<String>();
    info.signature = doc["signature"].as<String>();
    info.size = doc["size"];
    info.required = doc["required"] | false;
    info.changelog = doc["changelog"].as<String>();

    // Defense-in-depth: Verify version is actually different
    // Protects against server bugs, API mismatches, or data corruption
    if (info.version == currentVersion) {
        Serial.printf("[OTA] Server says update available, but versions match: %s\n", currentVersion.c_str());
        status = OTAStatus::IDLE;
        lastError = "";  // Clear error - this is a normal case
        return false;  // No update actually needed
    }

    // Log update details
    String current = doc["current_version"] | "unknown";
    Serial.printf("[OTA] Update available: %s → %s (%d bytes) [variant: %s]\n",
                  current.c_str(), info.version.c_str(), info.size, variant.c_str());

    if (info.required) {
        Serial.println("[OTA] This is a required update");
    }

    status = OTAStatus::IDLE;
    lastError = "";  // Clear error on success
    return true;  // Update available
}

bool OTAManager::downloadAndInstall(const FirmwareInfo& info,
                                   void (*progressCallback)(size_t, size_t)) {
    status = OTAStatus::DOWNLOADING;

    // Get display_variant (same as in checkForUpdate)
    String variant = ConfigManager::getDisplayVariant();
    if (variant.isEmpty()) {
        lastError = "display_variant not configured";
        status = OTAStatus::FAILED;
        return false;
    }

    // Build full download URL with query parameters including display_variant
    // IMPORTANT: Must use full URL (with https://) for certificate validation to work
    String url = serverUrl + info.downloadUrl + "?device_id=" + deviceId + "&display_variant=" + variant;

    // Create fresh HTTPS client instances
    WiFiClientSecure secureClient;
    HTTPClient httpClient;

    // WORKAROUND: Skip certificate validation (same as checkForUpdate)
    // See docs/SSL_CERT_VALIDATION_ISSUE.md for details on ECDSA validation failure
    secureClient.setInsecure();
    Serial.println("[OTA] Skipping cert check (encrypted + signed, trusted network)");

    httpClient.begin(secureClient, url);

    // FUTURE: OTA API authentication not yet implemented
    // When added, enable this to require Bearer token for firmware downloads:
    // if (ConfigManager::getConfig().security.api_token.length() > 0) {
    //     httpClient.addHeader("Authorization", "Bearer " + ConfigManager::getConfig().security.api_token);
    // }
    // See server/app/api/firmware.py for corresponding server-side implementation

    int httpCode = httpClient.GET();

    if (httpCode != HTTP_CODE_OK) {
        lastError = "Download failed: " + String(httpCode);
        status = OTAStatus::FAILED;
        httpClient.end();
        return false;
    }

    int contentLength = httpClient.getSize();
    if (contentLength != info.size) {
        lastError = "Size mismatch: expected " + String(info.size) + " got " + String(contentLength);
        status = OTAStatus::FAILED;
        httpClient.end();
        return false;
    }

    // Get stream
    NetworkClient* stream = httpClient.getStreamPtr();

    // Verify hash during download
    status = OTAStatus::VERIFYING;
    mbedtls_sha256_context sha256_ctx;
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts(&sha256_ctx, 0);  // SHA-256 (not SHA-224)

    // Begin OTA update
    status = OTAStatus::INSTALLING;
    if (!Update.begin(contentLength)) {
        lastError = "OTA begin failed: " + String(Update.errorString());
        status = OTAStatus::FAILED;
        mbedtls_sha256_free(&sha256_ctx);
        httpClient.end();
        return false;
    }

    // Write firmware while computing hash
    size_t written = 0;
    uint8_t buffer[512];

    while (httpClient.connected() && written < contentLength) {
        size_t available = stream->available();
        if (available) {
            size_t toRead = min(available, sizeof(buffer));
            size_t bytesRead = stream->readBytes(buffer, toRead);

            // Update hash
            mbedtls_sha256_update(&sha256_ctx, buffer, bytesRead);

            // Write to flash
            size_t bytesWritten = Update.write(buffer, bytesRead);
            if (bytesWritten != bytesRead) {
                lastError = "Write failed at " + String(written);
                status = OTAStatus::FAILED;
                Update.abort();
                mbedtls_sha256_free(&sha256_ctx);
                httpClient.end();
                return false;
            }

            written += bytesWritten;

            // Progress callback
            if (progressCallback) {
                progressCallback(written, contentLength);
            }
        }
        delay(1);
    }

    httpClient.end();

    // Finalize hash
    uint8_t hash[32];
    mbedtls_sha256_finish(&sha256_ctx, hash);
    mbedtls_sha256_free(&sha256_ctx);

    // Convert to hex string
    char hashStr[65];
    for (int i = 0; i < 32; i++) {
        sprintf(hashStr + (i * 2), "%02x", hash[i]);
    }
    hashStr[64] = 0;

    // Verify hash
    if (String(hashStr) != info.sha256Hash) {
        lastError = "Hash mismatch: expected " + info.sha256Hash + " got " + String(hashStr);
        status = OTAStatus::FAILED;
        Update.abort();
        return false;
    }

    // Verify ECDSA signature
    if (!verifySignature(hash, info.signature)) {
        lastError = "Signature verification failed";
        status = OTAStatus::FAILED;
        Update.abort();
        return false;
    }

    // Finalize update
    if (!Update.end()) {
        lastError = "OTA end failed: " + String(Update.errorString());
        status = OTAStatus::FAILED;
        return false;
    }

    if (!Update.isFinished()) {
        lastError = "Update not finished";
        status = OTAStatus::FAILED;
        return false;
    }

    // Save version info to NVS before reboot
    // After reboot, reportUpdateSuccess() reads these and reports to server
    savePendingUpdate(currentVersion, info.version);

    status = OTAStatus::SUCCESS;

    // Device will reboot after this
    // On next boot, checkBootValidation() will mark the app as valid
    return true;
}

bool OTAManager::verifySignature(const uint8_t* hash, const String& signature) {
    Serial.println("[OTA] Verifying ECDSA P-256 signature...");

    // Step 1: Decode base64 signature
    uint8_t sig_bytes[128];  // ECDSA P-256 signature is 64 bytes, but we allow extra space
    size_t sig_len = 0;

    int ret = mbedtls_base64_decode(
        sig_bytes, sizeof(sig_bytes), &sig_len,
        (const unsigned char*)signature.c_str(), signature.length()
    );

    if (ret != 0) {
        char errBuf[64];
        snprintf(errBuf, sizeof(errBuf), "Base64 decode failed: -0x%04x", -ret);
        Serial.println(errBuf);
        lastError = "Signature base64 decode failed";
        return false;
    }

    Serial.printf("[OTA] Decoded signature: %d bytes\n", sig_len);

    // Step 2: Parse public key from PEM string
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    ret = mbedtls_pk_parse_public_key(
        &pk,
        (const unsigned char*)FIRMWARE_PUBLIC_KEY,
        strlen(FIRMWARE_PUBLIC_KEY) + 1  // Include null terminator
    );

    if (ret != 0) {
        char errBuf[64];
        snprintf(errBuf, sizeof(errBuf), "Public key parse failed: -0x%04x", -ret);
        Serial.println(errBuf);
        lastError = "Invalid public key";
        mbedtls_pk_free(&pk);
        return false;
    }

    // Verify it's an EC key
    if (mbedtls_pk_get_type(&pk) != MBEDTLS_PK_ECKEY &&
        mbedtls_pk_get_type(&pk) != MBEDTLS_PK_ECDSA) {
        Serial.println("[OTA] Public key is not an EC key");
        lastError = "Public key type mismatch";
        mbedtls_pk_free(&pk);
        return false;
    }

    // Step 3: Verify signature using ECDSA
    // The signature format from mbedtls signing is typically ASN.1 DER encoded
    // We need to verify using the hash (32 bytes for SHA-256)
    ret = mbedtls_pk_verify(
        &pk,
        MBEDTLS_MD_SHA256,      // Hash algorithm used
        hash,                    // The hash to verify (32 bytes)
        32,                      // Hash length
        sig_bytes,               // The signature
        sig_len                  // Signature length
    );

    mbedtls_pk_free(&pk);

    if (ret != 0) {
        char errBuf[128];
        snprintf(errBuf, sizeof(errBuf), "Signature verification failed: -0x%04x", -ret);
        Serial.println(errBuf);

        // Common error codes
        if (ret == MBEDTLS_ERR_ECP_VERIFY_FAILED) {
            lastError = "Invalid signature - firmware may be tampered";
        } else if (ret == MBEDTLS_ERR_ECP_BAD_INPUT_DATA) {
            lastError = "Signature format invalid";
        } else {
            lastError = "Signature verification error";
        }
        return false;
    }

    Serial.println("[OTA] Signature verified successfully");
    return true;
}

void OTAManager::rollbackToFactory() {
    const esp_partition_t* factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_APP_FACTORY,
        NULL
    );

    if (factory != NULL) {
        esp_ota_set_boot_partition(factory);
        ESP.restart();
    } else {
        Serial.println("No factory partition found - cannot rollback");
    }
}

bool OTAManager::checkBootValidation() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            // New firmware booted successfully, mark as valid
            Serial.println("[OTA] Boot validation: Marking app as valid");
            esp_ota_mark_app_valid_cancel_rollback();
            return true;
        }
    }

    return false;
}

void OTAManager::savePendingUpdate(const String& oldVersion, const String& newVersion) {
    Preferences prefs;
    prefs.begin("ota", false);  // read-write
    prefs.putString("old_ver", oldVersion);
    prefs.putString("new_ver", newVersion);
    prefs.end();
    Serial.printf("[OTA] Saved pending update: %s → %s\n", oldVersion.c_str(), newVersion.c_str());
}

bool OTAManager::reportUpdateSuccess() {
    Preferences prefs;
    prefs.begin("ota", true);  // read-only
    String oldVersion = prefs.getString("old_ver", "");
    String newVersion = prefs.getString("new_ver", "");
    prefs.end();

    // No pending update to report
    if (oldVersion.isEmpty() || newVersion.isEmpty()) {
        return true;
    }

    // Guard against rollback: if the current firmware doesn't match the expected
    // new version, the OTA likely failed and the device rolled back. Clear stale
    // NVS data instead of falsely reporting success.
    if (currentVersion != newVersion) {
        Serial.printf("[OTA] Version mismatch: running %s but NVS says %s — likely rolled back, clearing\n",
                      currentVersion.c_str(), newVersion.c_str());
        Preferences clearPrefs;
        clearPrefs.begin("ota", false);
        clearPrefs.remove("old_ver");
        clearPrefs.remove("new_ver");
        clearPrefs.end();
        return true;
    }

    Serial.printf("[OTA] Reporting update success: %s → %s\n",
                  oldVersion.c_str(), newVersion.c_str());

    // Build JSON payload matching server's POST /api/firmware/stats schema
    JsonDocument doc;
    doc["device_id"] = deviceId;
    doc["old_version"] = oldVersion;
    doc["new_version"] = newVersion;
    doc["status"] = "success";

    String payload;
    serializeJson(doc, payload);

    // POST to /api/firmware/stats
    WiFiClientSecure secureClient;
    HTTPClient httpClient;
    secureClient.setInsecure();  // Same TLS approach as other OTA requests

    String url = serverUrl + "/api/firmware/stats";
    httpClient.begin(secureClient, url);
    httpClient.addHeader("Content-Type", "application/json");
    httpClient.setTimeout(10000);

    int httpCode = httpClient.POST(payload);
    httpClient.end();

    if (httpCode == 200) {
        Serial.println("[OTA] Update success reported to server");
        // Clear NVS entries now that server has recorded the update
        Preferences clearPrefs;
        clearPrefs.begin("ota", false);  // read-write
        clearPrefs.remove("old_ver");
        clearPrefs.remove("new_ver");
        clearPrefs.end();
        return true;
    } else {
        // Non-critical failure — NVS entries persist and we'll retry on next reboot
        Serial.printf("[OTA] Failed to report update success: HTTP %d\n", httpCode);
        return false;
    }
}
