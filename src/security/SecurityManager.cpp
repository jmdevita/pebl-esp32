#include "SecurityManager.h"

// Static member definitions
mbedtls_aes_context SecurityManager::aesContext;
bool SecurityManager::initialized = false;
uint8_t SecurityManager::aesKey[32];
uint8_t SecurityManager::aesIV[16];

// External log function (defined in main.cpp)
extern void logMessage(int level, const char* module, const char* message, const char* kvPairs);

void SecurityManager::logMessage(const char* level, const char* module, const char* message, const char* details) {
    int logLevel = 2; // INFO by default
    if (strcmp(level, "ERROR") == 0) logLevel = 0;
    else if (strcmp(level, "WARN") == 0) logLevel = 1;
    else if (strcmp(level, "DEBUG") == 0) logLevel = 3;

    ::logMessage(logLevel, module, message, details);
}

bool SecurityManager::init(const String& base64Key) {
    logMessage("INFO", "SECURITY", "Initializing AES-256");

    if (base64Key.isEmpty()) {
        logMessage("WARN", "SECURITY", "No AES key provided, encryption disabled");
        initialized = false;
        return false;
    }

    // Decode base64 key
    size_t keyLen = 32; // AES-256 requires 32 bytes
    unsigned char decodedKey[32];
    size_t outLen;

    int ret = mbedtls_base64_decode(decodedKey, keyLen, &outLen,
                                    (const unsigned char*)base64Key.c_str(),
                                    base64Key.length());

    if (ret != 0 || outLen != 32) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ret=%d outLen=%zu", ret, outLen);
        logMessage("ERROR", "SECURITY", "Invalid AES key", buf);
        return false;
    }

    // Copy key to static storage
    memcpy(aesKey, decodedKey, 32);

    // Initialize AES context
    mbedtls_aes_init(&aesContext);
    ret = mbedtls_aes_setkey_dec(&aesContext, aesKey, 256);

    if (ret != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "error=%d", ret);
        logMessage("ERROR", "SECURITY", "AES key setup failed", buf);
        mbedtls_aes_free(&aesContext);
        return false;
    }

    initialized = true;
    logMessage("INFO", "SECURITY", "AES-256 initialized successfully");

    return true;
}

void SecurityManager::cleanup() {
    if (initialized) {
        mbedtls_aes_free(&aesContext);
        memset(aesKey, 0, sizeof(aesKey));
        initialized = false;
        logMessage("INFO", "SECURITY", "AES context cleaned up");
    }
}

String SecurityManager::decrypt(const String& encryptedBase64, const String& ivBase64) {
    if (!initialized) {
        logMessage("WARN", "SECURITY", "Decrypt called but not initialized");
        return "";
    }

    // Decode IV from base64
    unsigned char iv[16];
    size_t ivLen;
    int ret = mbedtls_base64_decode(iv, 16, &ivLen,
                                    (const unsigned char*)ivBase64.c_str(),
                                    ivBase64.length());

    if (ret != 0 || ivLen != 16) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ret=%d ivLen=%zu", ret, ivLen);
        logMessage("ERROR", "SECURITY", "Invalid IV", buf);
        return "";
    }

    // Decode encrypted data from base64
    size_t encLen = encryptedBase64.length();
    size_t maxDecLen = (encLen * 3) / 4 + 16; // Overestimate for safety
    unsigned char* encData = new unsigned char[maxDecLen];
    size_t actualEncLen;

    ret = mbedtls_base64_decode(encData, maxDecLen, &actualEncLen,
                                (const unsigned char*)encryptedBase64.c_str(),
                                encLen);

    if (ret != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "error=%d", ret);
        logMessage("ERROR", "SECURITY", "Base64 decode failed", buf);
        delete[] encData;
        return "";
    }

    // Decrypt the data
    unsigned char* decData = new unsigned char[actualEncLen + 16];
    memcpy(aesIV, iv, 16); // Copy IV for CBC mode

    ret = mbedtls_aes_crypt_cbc(&aesContext, MBEDTLS_AES_DECRYPT,
                                actualEncLen, aesIV,
                                encData, decData);

    delete[] encData;

    if (ret != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "error=%d", ret);
        logMessage("ERROR", "SECURITY", "AES decrypt failed", buf);
        delete[] decData;
        return "";
    }

    // Remove PKCS7 padding
    size_t finalLen = removePadding(decData, actualEncLen);

    if (finalLen == 0) {
        logMessage("ERROR", "SECURITY", "Invalid padding");
        delete[] decData;
        return "";
    }

    // Convert to string
    String result((char*)decData, finalLen);
    delete[] decData;

    char buf[64];
    snprintf(buf, sizeof(buf), "decrypted_len=%zu", finalLen);
    logMessage("DEBUG", "SECURITY", "Decryption successful", buf);

    return result;
}

String SecurityManager::decryptHex(const String& encryptedHex, const String& ivHex) {
    if (!initialized) {
        logMessage("WARN", "SECURITY", "Decrypt called but not initialized");
        return "";
    }

    // Helper function to convert hex string to bytes
    auto hexToByte = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };

    // Decode IV from hex (should be 32 hex chars = 16 bytes)
    if (ivHex.length() != 32) {
        char buf[64];
        snprintf(buf, sizeof(buf), "expected=32 got=%d", ivHex.length());
        logMessage("ERROR", "SECURITY", "Invalid IV hex length", buf);
        return "";
    }

    unsigned char iv[16];
    for (int i = 0; i < 16; i++) {
        iv[i] = (hexToByte(ivHex[i*2]) << 4) | hexToByte(ivHex[i*2 + 1]);
    }

    // Decode encrypted data from hex
    size_t encLen = encryptedHex.length() / 2;
    if (encryptedHex.length() % 2 != 0) {
        logMessage("ERROR", "SECURITY", "Invalid hex string (odd length)");
        return "";
    }

    unsigned char* encData = new unsigned char[encLen];
    for (size_t i = 0; i < encLen; i++) {
        encData[i] = (hexToByte(encryptedHex[i*2]) << 4) | hexToByte(encryptedHex[i*2 + 1]);
    }

    // Decrypt the data
    unsigned char* decData = new unsigned char[encLen + 16];
    memcpy(aesIV, iv, 16); // Copy IV for CBC mode

    int ret = mbedtls_aes_crypt_cbc(&aesContext, MBEDTLS_AES_DECRYPT,
                                encLen, aesIV,
                                encData, decData);

    delete[] encData;

    if (ret != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "error=%d", ret);
        logMessage("ERROR", "SECURITY", "AES decrypt failed", buf);
        delete[] decData;
        return "";
    }

    // Remove PKCS7 padding
    size_t finalLen = removePadding(decData, encLen);

    if (finalLen == 0) {
        logMessage("ERROR", "SECURITY", "Invalid padding");
        delete[] decData;
        return "";
    }

    // Convert to string
    String result((char*)decData, finalLen);
    delete[] decData;

    char buf[64];
    snprintf(buf, sizeof(buf), "decrypted_len=%zu", finalLen);
    logMessage("DEBUG", "SECURITY", "Hex decryption successful", buf);

    return result;
}

String SecurityManager::encrypt(const String& plaintext, String& ivBase64Out) {
    if (!initialized) {
        logMessage("WARN", "SECURITY", "Encrypt called but not initialized");
        return "";
    }

    // Generate random IV
    uint8_t iv[16];
    generateIV(iv);

    // Encode IV to base64
    char ivB64[32];
    size_t ivB64Len;
    mbedtls_base64_encode((unsigned char*)ivB64, sizeof(ivB64), &ivB64Len,
                         iv, 16);
    ivBase64Out = String(ivB64, ivB64Len);

    // Prepare data with PKCS7 padding
    size_t dataLen = plaintext.length();
    size_t paddedLen = ((dataLen / 16) + 1) * 16;
    unsigned char* data = new unsigned char[paddedLen];
    memcpy(data, plaintext.c_str(), dataLen);
    addPadding(data, dataLen, 16);

    // Encrypt the data
    unsigned char* encData = new unsigned char[paddedLen];
    memcpy(aesIV, iv, 16); // Copy IV for CBC mode

    // Need to use encryption context for encrypt
    mbedtls_aes_context encContext;
    mbedtls_aes_init(&encContext);
    mbedtls_aes_setkey_enc(&encContext, aesKey, 256);

    int ret = mbedtls_aes_crypt_cbc(&encContext, MBEDTLS_AES_ENCRYPT,
                                    paddedLen, aesIV,
                                    data, encData);

    mbedtls_aes_free(&encContext);
    delete[] data;

    if (ret != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "error=%d", ret);
        logMessage("ERROR", "SECURITY", "AES encrypt failed", buf);
        delete[] encData;
        return "";
    }

    // Encode encrypted data to base64
    size_t b64Len = ((paddedLen * 4) / 3) + 4;
    char* b64Data = new char[b64Len];
    size_t actualB64Len;

    mbedtls_base64_encode((unsigned char*)b64Data, b64Len, &actualB64Len,
                         encData, paddedLen);

    delete[] encData;

    String result(b64Data, actualB64Len);
    delete[] b64Data;

    return result;
}

void SecurityManager::generateIV(uint8_t* iv) {
    // Use ESP32 hardware RNG
    for (int i = 0; i < 16; i++) {
        iv[i] = (uint8_t)esp_random();
    }
}

void SecurityManager::addPadding(uint8_t* data, size_t dataLen, size_t blockSize) {
    size_t paddingLen = blockSize - (dataLen % blockSize);
    for (size_t i = 0; i < paddingLen; i++) {
        data[dataLen + i] = paddingLen;
    }
}

size_t SecurityManager::removePadding(uint8_t* data, size_t dataLen) {
    if (dataLen == 0) return 0;

    uint8_t paddingLen = data[dataLen - 1];

    // Validate padding
    if (paddingLen == 0 || paddingLen > 16) {
        return 0;
    }

    for (size_t i = 1; i <= paddingLen; i++) {
        if (data[dataLen - i] != paddingLen) {
            return 0;
        }
    }

    return dataLen - paddingLen;
}

bool SecurityManager::selfTest() {
    logMessage("INFO", "SECURITY", "Running self-test");

    // Test message
    String testMsg = "Hello, ESP32! This is a test message for AES-256-CBC encryption.";
    String ivB64;

    // Encrypt
    String encrypted = encrypt(testMsg, ivB64);
    if (encrypted.isEmpty()) {
        logMessage("ERROR", "SECURITY", "Self-test: Encryption failed");
        return false;
    }

    // Decrypt
    String decrypted = decrypt(encrypted, ivB64);
    if (decrypted != testMsg) {
        logMessage("ERROR", "SECURITY", "Self-test: Decryption mismatch");
        return false;
    }

    logMessage("INFO", "SECURITY", "Self-test passed");
    return true;
}