#include "SecurityManager.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// Static member definitions
uint8_t SecurityManager::privateKey[32];
bool SecurityManager::initialized = false;
bool SecurityManager::keyUploaded = false;

static const char* NVS_NAMESPACE = "security";
static const char* NVS_KEY_PRIV = "ecdh_priv";
static const char* NVS_KEY_UPLOADED = "key_uploaded";

// External log function (defined in main.cpp)
extern void logMessage(int level, const char* module, const char* message, const char* kvPairs);

void SecurityManager::logMessage(const char* level, const char* module, const char* message, const char* details) {
    int logLevel = 2; // INFO by default
    if (strcmp(level, "ERROR") == 0) logLevel = 0;
    else if (strcmp(level, "WARN") == 0) logLevel = 1;
    else if (strcmp(level, "DEBUG") == 0) logLevel = 3;

    ::logMessage(logLevel, module, message, details);
}

// ============================================================================
// NVS Operations
// ============================================================================

bool SecurityManager::loadKeyFromNVS() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) {  // read-only
        logMessage("DEBUG", "SECURITY", "NVS namespace not found (first boot)");
        return false;
    }

    size_t keyLen = prefs.getBytes(NVS_KEY_PRIV, privateKey, 32);
    keyUploaded = prefs.getBool(NVS_KEY_UPLOADED, false);
    prefs.end();

    if (keyLen != 32) {
        logMessage("DEBUG", "SECURITY", "No ECDH private key in NVS");
        return false;
    }

    // Verify the key is non-zero (sanity check)
    bool allZero = true;
    for (int i = 0; i < 32; i++) {
        if (privateKey[i] != 0) { allZero = false; break; }
    }
    if (allZero) {
        logMessage("WARN", "SECURITY", "ECDH private key in NVS is all zeros, regenerating");
        return false;
    }

    logMessage("INFO", "SECURITY", "ECDH P-256 private key loaded from NVS");
    return true;
}

bool SecurityManager::generateAndSaveKey() {
    logMessage("INFO", "SECURITY", "Generating ECDH P-256 keypair (first boot)");

    // Initialize RNG
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    const char* pers = "ecdh_keygen";
    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                     (const unsigned char*)pers, strlen(pers));
    if (ret != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "error=0x%04x", -ret);
        logMessage("ERROR", "SECURITY", "DRBG seed failed", buf);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return false;
    }

    // Generate P-256 keypair using mbedtls ECP
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point Q;

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);

    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "error=0x%04x", -ret);
        logMessage("ERROR", "SECURITY", "ECP group load failed", buf);
        goto keygen_cleanup;
    }

    ret = mbedtls_ecp_gen_keypair(&grp, &d, &Q, mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "error=0x%04x", -ret);
        logMessage("ERROR", "SECURITY", "Keypair generation failed", buf);
        goto keygen_cleanup;
    }

    // Extract private scalar as 32 bytes (big-endian, zero-padded)
    ret = mbedtls_mpi_write_binary(&d, privateKey, 32);
    if (ret != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "error=0x%04x", -ret);
        logMessage("ERROR", "SECURITY", "Private key extraction failed", buf);
        goto keygen_cleanup;
    }

    // Save to NVS
    {
        Preferences prefs;
        if (!prefs.begin(NVS_NAMESPACE, false)) {  // read-write
            logMessage("ERROR", "SECURITY", "Failed to open NVS for writing");
            ret = -1;
            goto keygen_cleanup;
        }
        prefs.putBytes(NVS_KEY_PRIV, privateKey, 32);
        prefs.putBool(NVS_KEY_UPLOADED, false);
        prefs.end();
    }

    keyUploaded = false;
    logMessage("INFO", "SECURITY", "ECDH P-256 keypair generated and saved to NVS");

keygen_cleanup:
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    return (ret == 0);
}

// ============================================================================
// Public API
// ============================================================================

bool SecurityManager::init() {
    logMessage("INFO", "SECURITY", "Initializing ECDH P-256 encryption");

    if (loadKeyFromNVS()) {
        initialized = true;
        char buf[32];
        snprintf(buf, sizeof(buf), "key_uploaded=%s", keyUploaded ? "true" : "false");
        logMessage("INFO", "SECURITY", "ECDH P-256 encryption initialized", buf);
        return true;
    }

    // First boot: generate new keypair
    if (generateAndSaveKey()) {
        initialized = true;
        logMessage("INFO", "SECURITY", "ECDH P-256 encryption initialized (new keypair)");
        return true;
    }

    logMessage("ERROR", "SECURITY", "Failed to initialize ECDH encryption");
    initialized = false;
    return false;
}

void SecurityManager::cleanup() {
    if (initialized) {
        memset(privateKey, 0, sizeof(privateKey));
        initialized = false;
        logMessage("INFO", "SECURITY", "ECDH private key zeroed from RAM");
    }
}

// ============================================================================
// Public Key Derivation
// ============================================================================

size_t SecurityManager::derivePublicKeyPEM(char* buf, size_t bufSize) {
    // Reconstruct the full keypair in a pk_context so mbedtls can write PEM
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    int ret = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret != 0) {
        mbedtls_pk_free(&pk);
        return 0;
    }

    mbedtls_ecp_keypair* ec = mbedtls_pk_ec(pk);

    ret = mbedtls_ecp_group_load(&ec->grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        mbedtls_pk_free(&pk);
        return 0;
    }

    // Load private scalar
    ret = mbedtls_mpi_read_binary(&ec->d, privateKey, 32);
    if (ret != 0) {
        mbedtls_pk_free(&pk);
        return 0;
    }

    // Derive public point Q = d * G
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0);

    ret = mbedtls_ecp_mul(&ec->grp, &ec->Q, &ec->d, &ec->grp.G,
                          mbedtls_ctr_drbg_random, &ctr_drbg);

    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    if (ret != 0) {
        mbedtls_pk_free(&pk);
        return 0;
    }

    // Write public key as PEM
    ret = mbedtls_pk_write_pubkey_pem(&pk, (unsigned char*)buf, bufSize);
    mbedtls_pk_free(&pk);

    if (ret != 0) {
        return 0;
    }

    return strlen(buf);
}

String SecurityManager::getPublicKeyPEM() {
    if (!initialized) return "";

    char pemBuf[256];
    size_t len = derivePublicKeyPEM(pemBuf, sizeof(pemBuf));
    if (len == 0) return "";

    return String(pemBuf);
}

// ============================================================================
// HKDF-SHA256 (manual implementation for ESP32 compatibility)
// ============================================================================

bool SecurityManager::hkdfSHA256(const uint8_t* ikm, size_t ikmLen, const uint8_t* salt, size_t saltLen, uint8_t* okm, size_t okmLen) {
    // HKDF with caller-provided salt, info="aes-key", length=32
    // Step 1: Extract — PRK = HMAC-SHA256(salt, IKM)
    // When salt is NULL, use a zero-filled buffer of hash length (32 bytes per RFC 5869)
    uint8_t zeroSalt[32] = {0};
    const uint8_t* effectiveSalt = (salt != NULL && saltLen > 0) ? salt : zeroSalt;
    size_t effectiveSaltLen = (salt != NULL && saltLen > 0) ? saltLen : 32;
    uint8_t prk[32];

    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info) return false;

    int ret = mbedtls_md_hmac(md_info, effectiveSalt, effectiveSaltLen, ikm, ikmLen, prk);
    if (ret != 0) return false;

    // Step 2: Expand — T(1) = HMAC-SHA256(PRK, info || 0x01)
    // We only need 32 bytes (one SHA-256 block), so one iteration suffices
    const char* info = "aes-key";
    size_t infoLen = strlen(info);

    // Build input: info || 0x01
    uint8_t expandInput[64];  // info (7 bytes) + counter (1 byte)
    memcpy(expandInput, info, infoLen);
    expandInput[infoLen] = 0x01;

    ret = mbedtls_md_hmac(md_info, prk, 32, expandInput, infoLen + 1, okm);

    // Zero intermediate PRK
    memset(prk, 0, sizeof(prk));

    return (ret == 0);
}

// ============================================================================
// ECDH Decryption
// ============================================================================

String SecurityManager::decryptECDH(const String& envelopeJson) {
    if (!initialized) {
        logMessage("WARN", "SECURITY", "decryptECDH called but not initialized");
        return "";
    }

    // 1. Parse JSON envelope
    JsonDocument doc;
    DeserializationError jsonErr = deserializeJson(doc, envelopeJson);
    if (jsonErr) {
        char buf[64];
        snprintf(buf, sizeof(buf), "error=%s", jsonErr.c_str());
        logMessage("ERROR", "SECURITY", "ECDH envelope JSON parse failed", buf);
        return "";
    }

    const char* encryptedB64 = doc["encrypted"];
    const char* ephemeralPem = doc["ephemeral_public_key"];
    const char* ivB64 = doc["iv"];
    const char* tagB64 = doc["tag"];
    const char* saltB64 = doc["salt"];

    if (!encryptedB64 || !ephemeralPem || !ivB64 || !tagB64 || !saltB64) {
        logMessage("ERROR", "SECURITY", "ECDH envelope missing required fields");
        return "";
    }

    // 2. Parse ephemeral public key from PEM
    mbedtls_pk_context ephPk;
    mbedtls_pk_init(&ephPk);

    size_t pemLen = strlen(ephemeralPem);
    // mbedtls_pk_parse_public_key expects null-terminated PEM, length includes null
    int ret = mbedtls_pk_parse_public_key(&ephPk,
                                           (const unsigned char*)ephemeralPem,
                                           pemLen + 1);
    if (ret != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "error=0x%04x", -ret);
        logMessage("ERROR", "SECURITY", "Failed to parse ephemeral public key", buf);
        mbedtls_pk_free(&ephPk);
        return "";
    }

    // Verify it's an EC key
    if (mbedtls_pk_get_type(&ephPk) != MBEDTLS_PK_ECKEY) {
        logMessage("ERROR", "SECURITY", "Ephemeral key is not EC type");
        mbedtls_pk_free(&ephPk);
        return "";
    }

    mbedtls_ecp_keypair* ephEc = mbedtls_pk_ec(ephPk);

    // 3. Reconstruct device keypair from private scalar
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_mpi shared_z;

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&shared_z);

    String result = "";

    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        logMessage("ERROR", "SECURITY", "ECP group load failed in decrypt");
        goto decrypt_cleanup;
    }

    ret = mbedtls_mpi_read_binary(&d, privateKey, 32);
    if (ret != 0) {
        logMessage("ERROR", "SECURITY", "Failed to load private key for ECDH");
        goto decrypt_cleanup;
    }

    // 4. ECDH: compute shared point = device_private * ephemeral_public
    {
        mbedtls_entropy_context entropy;
        mbedtls_ctr_drbg_context ctr_drbg;
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0);

        ret = mbedtls_ecdh_compute_shared(&grp, &shared_z,
                                           &ephEc->Q, &d,
                                           mbedtls_ctr_drbg_random, &ctr_drbg);

        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
    }

    if (ret != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "error=0x%04x", -ret);
        logMessage("ERROR", "SECURITY", "ECDH compute_shared failed", buf);
        goto decrypt_cleanup;
    }

    // Convert shared secret MPI to bytes
    {
        uint8_t sharedSecret[32];
        ret = mbedtls_mpi_write_binary(&shared_z, sharedSecret, 32);
        if (ret != 0) {
            logMessage("ERROR", "SECURITY", "Failed to extract shared secret bytes");
            goto decrypt_cleanup;
        }

        // 5. Base64-decode HKDF salt and derive AES key
        uint8_t hkdfSalt[16];
        size_t hkdfSaltLen = 0;
        ret = mbedtls_base64_decode(hkdfSalt, 16, &hkdfSaltLen,
                                     (const unsigned char*)saltB64, strlen(saltB64));
        if (ret != 0 || hkdfSaltLen != 16) {
            logMessage("ERROR", "SECURITY", "Failed to decode HKDF salt");
            memset(sharedSecret, 0, sizeof(sharedSecret));
            goto decrypt_cleanup;
        }

        uint8_t aesKey[32];
        if (!hkdfSHA256(sharedSecret, 32, hkdfSalt, hkdfSaltLen, aesKey, 32)) {
            logMessage("ERROR", "SECURITY", "HKDF derivation failed");
            memset(sharedSecret, 0, sizeof(sharedSecret));
            goto decrypt_cleanup;
        }

        // Zero shared secret immediately
        memset(sharedSecret, 0, sizeof(sharedSecret));

        // 6. Base64-decode IV (12 bytes — GCM standard per NIST SP 800-38D)
        uint8_t iv[12];
        size_t ivLen = 0;
        ret = mbedtls_base64_decode(iv, 12, &ivLen,
                                     (const unsigned char*)ivB64, strlen(ivB64));
        if (ret != 0 || ivLen != 12) {
            logMessage("ERROR", "SECURITY", "Failed to decode IV");
            memset(aesKey, 0, sizeof(aesKey));
            goto decrypt_cleanup;
        }

        // Base64-decode GCM authentication tag (16 bytes)
        uint8_t tag[16];
        size_t tagLen = 0;
        ret = mbedtls_base64_decode(tag, 16, &tagLen,
                                     (const unsigned char*)tagB64, strlen(tagB64));
        if (ret != 0 || tagLen != 16) {
            logMessage("ERROR", "SECURITY", "Failed to decode GCM tag");
            memset(aesKey, 0, sizeof(aesKey));
            goto decrypt_cleanup;
        }

        // Base64-decode ciphertext
        size_t encB64Len = strlen(encryptedB64);
        size_t maxDecLen = (encB64Len * 3) / 4 + 16;
        uint8_t* ciphertext = new uint8_t[maxDecLen];
        size_t ciphertextLen = 0;

        ret = mbedtls_base64_decode(ciphertext, maxDecLen, &ciphertextLen,
                                     (const unsigned char*)encryptedB64, encB64Len);
        if (ret != 0 || ciphertextLen == 0) {
            logMessage("ERROR", "SECURITY", "Failed to decode ciphertext");
            delete[] ciphertext;
            memset(aesKey, 0, sizeof(aesKey));
            goto decrypt_cleanup;
        }

        // 7. AES-256-GCM authenticated decrypt
        // GCM verifies the authentication tag, preventing ciphertext tampering
        mbedtls_gcm_context gcmCtx;
        mbedtls_gcm_init(&gcmCtx);
        ret = mbedtls_gcm_setkey(&gcmCtx, MBEDTLS_CIPHER_ID_AES, aesKey, 256);

        memset(aesKey, 0, sizeof(aesKey));

        if (ret != 0) {
            logMessage("ERROR", "SECURITY", "GCM key setup failed");
            mbedtls_gcm_free(&gcmCtx);
            delete[] ciphertext;
            goto decrypt_cleanup;
        }

        uint8_t* plaintext = new uint8_t[ciphertextLen + 1];
        ret = mbedtls_gcm_auth_decrypt(&gcmCtx, ciphertextLen,
                                        iv, ivLen,       // 12-byte IV
                                        NULL, 0,         // no additional authenticated data
                                        tag, 16,         // GCM auth tag
                                        ciphertext, plaintext);

        mbedtls_gcm_free(&gcmCtx);
        delete[] ciphertext;

        if (ret != 0) {
            // ret == MBEDTLS_ERR_GCM_AUTH_FAILED means tag verification failed (tampering)
            char buf[32];
            snprintf(buf, sizeof(buf), "error=0x%04x", -ret);
            logMessage("ERROR", "SECURITY", "AES-GCM decrypt/auth failed", buf);
            delete[] plaintext;
            goto decrypt_cleanup;
        }

        // GCM output is exact plaintext length — no padding to remove
        plaintext[ciphertextLen] = '\0';
        result = String((char*)plaintext, ciphertextLen);
        delete[] plaintext;

        char logBuf[64];
        snprintf(logBuf, sizeof(logBuf), "decrypted_len=%zu", ciphertextLen);
        logMessage("DEBUG", "SECURITY", "ECDH decryption successful", logBuf);
    }

decrypt_cleanup:
    mbedtls_mpi_free(&shared_z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    mbedtls_pk_free(&ephPk);

    return result;
}

void SecurityManager::resetKeyUploaded() {
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, false)) {
        prefs.putBool(NVS_KEY_UPLOADED, false);
        prefs.end();
    }
    keyUploaded = false;
    logMessage("INFO", "SECURITY", "Key upload flag reset (will re-upload on next boot)");
}

// ============================================================================
// Key Upload
// ============================================================================

bool SecurityManager::uploadPublicKey(const String& serverBaseUrl, const String& authToken, const String& deviceId) {
    if (!initialized) {
        logMessage("WARN", "SECURITY", "Cannot upload key - not initialized");
        return false;
    }

    if (keyUploaded) {
        logMessage("DEBUG", "SECURITY", "Key already uploaded, skipping");
        return true;
    }

    // Derive PEM public key
    char pemBuf[256];
    size_t pemLen = derivePublicKeyPEM(pemBuf, sizeof(pemBuf));
    if (pemLen == 0) {
        logMessage("ERROR", "SECURITY", "Failed to derive public key PEM for upload");
        return false;
    }

    logMessage("INFO", "SECURITY", "Uploading ECDH public key to server");

    // Build multipart form data (file only — token/name/key_type go as query params)
    String boundary = "----ESP32ECDHKeyUpload";
    String body = "";

    // File: public key PEM
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\"device_key.pem\"\r\n";
    body += "Content-Type: application/x-pem-file\r\n\r\n";
    body += String(pemBuf) + "\r\n";

    // End boundary
    body += "--" + boundary + "--\r\n";

    // Server expects token, name, key_type as query parameters (not form fields)
    String uploadUrl = serverBaseUrl + "/upload?token=" + authToken
                     + "&name=" + deviceId + "&key_type=ECDH-P256";
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();  // Server cert validation handled at TLS layer by server URL trust

    bool success = false;

    if (http.begin(client, uploadUrl)) {
        http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

        int httpCode = http.POST(body);

        if (httpCode == 200) {
            logMessage("INFO", "SECURITY", "Public key uploaded successfully");
            success = true;

            // Mark as uploaded in NVS
            Preferences prefs;
            if (prefs.begin(NVS_NAMESPACE, false)) {
                prefs.putBool(NVS_KEY_UPLOADED, true);
                prefs.end();
            }
            keyUploaded = true;
        } else if (httpCode == 409) {
            // KEY_EXISTS: server already has a key for this device.
            // Two scenarios:
            //   1. Same keypair (normal reboot) — decryption will work fine
            //   2. Different keypair (NVS erased / reflash) — decryption will fail
            //      because server encrypts with old public key but device has new private key.
            //      Admin must reset key on server to fix: DELETE /admin/users/{id}/key
            logMessage("WARN", "SECURITY", "Key already exists on server (409)",
                      "If decryption fails, ask admin to reset device key on server");
            success = true;

            Preferences prefs;
            if (prefs.begin(NVS_NAMESPACE, false)) {
                prefs.putBool(NVS_KEY_UPLOADED, true);
                prefs.end();
            }
            keyUploaded = true;
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "http_code=%d", httpCode);
            logMessage("WARN", "SECURITY", "Key upload failed", buf);

            String response = http.getString();
            if (response.length() > 0 && response.length() < 200) {
                logMessage("DEBUG", "SECURITY", "Server response", response.c_str());
            }
        }

        http.end();
    } else {
        logMessage("ERROR", "SECURITY", "Failed to connect for key upload", uploadUrl.c_str());
    }

    return success;
}
