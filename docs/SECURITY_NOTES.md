# Security Notes

## ECDH P-256 Encryption

The ESP32 client uses Elliptic Curve Diffie-Hellman (ECDH) P-256 for end-to-end message encryption (default, recommended).

### How It Works
1. Device generates an ECDH P-256 keypair on first boot (hardware RNG)
2. Private key stored in NVS — never leaves the device
3. Public key uploaded to server via authenticated HTTPS
4. Server encrypts each message with a fresh ephemeral key (ECDH + HKDF-SHA256 + AES-256-CBC)
5. Device decrypts using its private key and the per-message ephemeral public key

This provides **forward secrecy** — each message uses a unique ephemeral key, so compromising the device key cannot decrypt past messages.

### Configuration
```json
{ "security": { "encryption": "ecdh" } }
```
Set to `"none"` to disable (not recommended).

## Device Authentication

Two-factor: `device_id` (auto-generated from ESP32 eFuse MAC) + `auth_token` (obtained via self-service pairing or admin pre-provisioning). Both required for all server communication.

## Pairing Security

- 8-character code, 10-minute TTL, one-time use
- Server-side rate limiting with exponential backoff on 429
- Battery-aware timeouts (2 min battery, 10 min USB)

## OTA Firmware Security

- SHA-256 hash verification
- ECDSA P-256 signature verification (tamper protection)
- Dual OTA partition with automatic rollback on failed boot
- HTTPS transport
