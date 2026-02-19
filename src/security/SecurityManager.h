#ifndef SECURITY_MANAGER_H
#define SECURITY_MANAGER_H

#include <Arduino.h>

// mbedtls 3.x hides struct members behind MBEDTLS_PRIVATE() macro.
// This define allows direct access to ecp_keypair members (grp, d, Q)
// which is needed for ECDH operations. Standard pattern for ESP32 projects.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include <mbedtls/ecp.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/pk.h>
#include <mbedtls/aes.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

// ECDH P-256 encryption manager
// Device generates a keypair on first boot, stores the private key in NVS,
// and uploads the public key to the server. Per-message decryption uses
// ECDH + HKDF-SHA256 + AES-256-GCM to match the server's wire format.
class SecurityManager {
private:
    static uint8_t privateKey[32];   // P-256 private scalar, loaded from NVS
    static bool initialized;
    static bool keyUploaded;         // Persisted in NVS

    // Logging helper
    static void logMessage(const char* level, const char* module, const char* message, const char* details = nullptr);

    // NVS operations
    static bool loadKeyFromNVS();
    static bool generateAndSaveKey();

    // Derive PEM-encoded public key from private scalar
    // Returns number of bytes written (including null terminator), or 0 on failure
    static size_t derivePublicKeyPEM(char* buf, size_t bufSize);

    // HKDF-SHA256: extract-then-expand with caller-provided salt, info="aes-key", length=32
    static bool hkdfSHA256(const uint8_t* ikm, size_t ikmLen, const uint8_t* salt, size_t saltLen, uint8_t* okm, size_t okmLen);

public:
    // Load key from NVS or generate on first boot
    static bool init();

    // Zero private key from RAM
    static void cleanup();

    // Decrypt ECDH envelope JSON: {"encrypted": b64, "ephemeral_public_key": PEM, "iv": b64, "tag": b64, "salt": b64}
    static String decryptECDH(const String& envelopeJson);

    // Get PEM-encoded public key for display/debugging
    static String getPublicKeyPEM();

    // Upload public key to server via multipart POST to /upload
    static bool uploadPublicKey(const String& serverBaseUrl, const String& authToken, const String& deviceId);

    // Check if key has been uploaded to server (persisted in NVS)
    static bool isKeyUploaded() { return keyUploaded; }

    // Reset the uploaded flag so the key is re-uploaded on next boot.
    // Called after re-pairing (new auth token = new server-side user association,
    // so the server needs the public key re-uploaded for the new user record).
    static void resetKeyUploaded();

    // Check if ECDH keypair is loaded and ready
    static bool isEnabled() { return initialized; }
};

#endif // SECURITY_MANAGER_H
