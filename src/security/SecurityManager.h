#ifndef SECURITY_MANAGER_H
#define SECURITY_MANAGER_H

#include <Arduino.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>

// AES-256 CBC encryption/decryption manager
class SecurityManager {
private:
    static mbedtls_aes_context aesContext;
    static bool initialized;
    static uint8_t aesKey[32];  // 256-bit key
    static uint8_t aesIV[16];   // 128-bit IV

    // Logging helper
    static void logMessage(const char* level, const char* module, const char* message, const char* details = nullptr);

    // PKCS7 padding helpers
    static void addPadding(uint8_t* data, size_t dataLen, size_t blockSize);
    static size_t removePadding(uint8_t* data, size_t dataLen);

public:
    // Initialize with AES key (base64 encoded)
    static bool init(const String& base64Key);

    // Cleanup
    static void cleanup();

    // Decrypt AES-256-CBC encrypted data (base64 encoded)
    static String decrypt(const String& encryptedBase64, const String& ivBase64);

    // Decrypt AES-256-CBC encrypted data (hex encoded)
    static String decryptHex(const String& encryptedHex, const String& ivHex);

    // Encrypt data (for testing)
    static String encrypt(const String& plaintext, String& ivBase64Out);

    // Generate random IV
    static void generateIV(uint8_t* iv);

    // Test encryption/decryption round trip
    static bool selfTest();

    // Check if security is enabled
    static bool isEnabled() { return initialized; }
};

#endif // SECURITY_MANAGER_H