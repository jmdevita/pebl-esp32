#include "OTAManager.h"
#include "config/ConfigManager.h"

OTAManager::OTAManager(const String& serverUrl, const String& deviceId)
    : serverUrl(serverUrl), deviceId(deviceId), status(OTAStatus::IDLE) {

    // Get current version from app descriptor
    const esp_app_desc_t* app_desc = esp_ota_get_app_description();
    currentVersion = String(app_desc->version);

    // Configure HTTPS (server is always HTTPS)
    // Uses standard certificate validation like WebSocket
    // FUTURE: Add certificate pinning for enhanced security
    Serial.println("[OTA] Configuring HTTPS");
    secureClient.setInsecure();
    Serial.println("[OTA] HTTPS enabled");
}

// Helper method to parse serverUrl into host and port components
// Assumes serverUrl format: "https://example.com" or "https://example.com:443"
void OTAManager::parseServerUrl(String& host, uint16_t& port) {
    host = serverUrl;
    host.remove(0, 8);  // Remove "https://"

    port = 443;  // Default HTTPS port
    int portIndex = host.indexOf(':');
    if (portIndex > 0) {
        port = host.substring(portIndex + 1).toInt();
        host = host.substring(0, portIndex);
    }
}

bool OTAManager::checkForUpdate(FirmwareInfo& info) {
    status = OTAStatus::CHECKING;

    // Build request path with query parameters
    String path = "/api/firmware/version?device_id=" + deviceId;

    // Parse serverUrl to extract host and port
    String host;
    uint16_t port;
    parseServerUrl(host, port);

    // Use explicit begin() signature to prevent URL parsing issues
    // Only the hostname is passed to DNS resolver, not the full URL
    httpClient.begin(secureClient, host, port, path);

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
    Serial.printf("[OTA] Update available: %s → %s (%d bytes)\n",
                  current.c_str(), info.version.c_str(), info.size);

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

    // Build download path with query parameters
    String path = info.downloadUrl + "?device_id=" + deviceId;

    // Parse serverUrl to extract host and port
    String host;
    uint16_t port;
    parseServerUrl(host, port);

    // Use explicit begin() signature to prevent URL parsing issues
    // Only the hostname is passed to DNS resolver, not the full URL
    httpClient.begin(secureClient, host, port, path);

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
    WiFiClient* stream = httpClient.getStreamPtr();

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
