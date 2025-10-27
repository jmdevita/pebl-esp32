# SSL Certificate Validation Issue with Cloudflare Tunnel

**Status**: Workaround implemented (using `setInsecure()`)
**Date**: January 2025
**Affected**: OTA firmware updates via Cloudflare tunnel
**Security Impact**: Medium - HTTPS encryption active but certificate validation bypassed

## Problem Summary

OTA firmware update certificate validation fails with error `-12288` (X509 verification failed) when connecting to `slack-reactions.devita.dev` through Cloudflare tunnel, despite the server presenting valid certificates that desktop browsers and curl validate successfully.

## Investigation Findings

### Working Comparison: Timezone API (ipgeolocation.io)

**Certificate Details:**
```
Chain: *.ipgeolocation.io → Let's Encrypt R11 → ISRG Root X1
Key Type: RSA 2048-bit
Signature Algorithm: sha256WithRSAEncryption
Chain Length: 2 certificates (leaf + intermediate)
TLS Version: 1.3
Cipher: TLS_AES_256_GCM_SHA384
```

**ESP32 Result:** ✅ Validates successfully with Mozilla CA bundle

### Failing: OTA Server (slack-reactions.devita.dev via Cloudflare)

**Certificate Details:**
```
Chain: devita.dev (wildcard *.devita.dev) → Google Trust Services WE1 → GTS Root
Key Type: EC P-256 (Elliptic Curve)
Signature Algorithm: ecdsa-with-SHA256
Chain Length: 3 certificates (leaf + intermediate + root)
TLS Version: 1.3
Cipher: TLS_AES_256_GCM_SHA384
```

**ESP32 Result:** ❌ Fails with `-12288` X509 verification error

### Key Differences

| Aspect | Timezone API (Works) | OTA Server (Fails) |
|--------|---------------------|-------------------|
| Key Type | RSA 2048 | EC P-256 |
| Signature | sha256WithRSA | ecdsa-with-SHA256 |
| CA | Let's Encrypt | Google Trust Services |
| Intermediate | R11 | WE1 |
| Chain Length | 2 certs | 3 certs |
| Validation | ✅ Success | ❌ Fails |

## Root Cause Analysis

The ESP32's mbedTLS library (version in Arduino Core 3.3.2 / ESP-IDF 5.5.1) appears to have issues validating **ECDSA certificate chains** specifically when:
1. Using `setCACertBundle()` with Mozilla CA bundle (146 root CAs)
2. Server presents EC P-256 keys with ecdsa-with-SHA256 signatures
3. Certificate chain includes Google Trust Services WE1 intermediate

**Why it's not an ESP32 general ECDSA issue:**
- ESP32 firmware signing uses ECDSA P-256 and validates successfully
- mbedTLS is compiled with ECDSA support (`MBEDTLS_ECDSA_C` enabled)
- The issue is specific to **certificate chain validation**, not ECDSA crypto operations

**Why it's not a Cloudflare issue:**
- Desktop browsers (Chrome, Firefox, Safari) validate the chain successfully
- `curl` and `openssl s_client` validate successfully
- The certificates are valid and properly signed

## Current Workaround

```cpp
// In src/ota/OTAManager.cpp
secureClient.setInsecure();  // Skip certificate validation
```

**Security implications:**
- ✅ Connection is still encrypted with TLS 1.3
- ✅ Firmware integrity protected by SHA-256 hash + ECDSA signature verification
- ❌ Vulnerable to MITM if attacker compromises local network
- ❌ No protection against DNS hijacking

**Why this is acceptable for now:**
1. WebSocket connection already uses `beginSSL()` without certificate validation
2. Firmware downloads are integrity-checked with SHA-256 hash verification (aborts on mismatch)
3. Firmware is cryptographically signed with ECDSA P-256 (verified after download, aborts on failure)
4. Intended for deployment on trusted home networks

## Potential Solutions

### Option 1: Force Cloudflare Tunnel to Use RSA Certificates

**Approach:**
- Check cloudflared configuration for certificate type override
- Cloudflare may auto-select ECDSA for performance (smaller certs, faster handshake)
- Force RSA certificate selection if available

**Pros:**
- Would match the working timezone API pattern
- No ESP32 code changes needed
- Proper certificate validation restored

**Cons:**
- May not be configurable in Cloudflare tunnel
- Slightly larger certificates and slower handshakes (minimal impact)

**Action Items:**
- [ ] Review cloudflared configuration options
- [ ] Check Cloudflare dashboard for certificate type settings
- [ ] Test if forcing RSA resolves the issue

### Option 2: Debug mbedTLS ECDSA Chain Validation

**Approach:**
- Enable mbedTLS debug logging (`MBEDTLS_DEBUG_C`)
- Add `mbedtls_ssl_conf_dbg()` callback to capture detailed error messages
- Identify exact point of failure in certificate chain validation

**Pros:**
- Would identify the root cause
- Could lead to proper fix (config change or upstream bug report)

**Cons:**
- Time-consuming investigation
- May be mbedTLS version-specific bug requiring ESP-IDF update
- Could require custom mbedTLS configuration

**Action Items:**
- [ ] Enable mbedTLS debug logging
- [ ] Capture detailed validation failure logs
- [ ] Compare working (RSA) vs failing (ECDSA) validation paths
- [ ] Check ESP-IDF issue tracker for similar reports

### Option 3: Pin Specific Certificate

**Approach:**
- Extract Cloudflare's certificate and pin it directly
- Use `setCACert()` instead of `setCACertBundle()`

**Pros:**
- Would validate Cloudflare's specific certificate
- Simpler than debugging mbedTLS

**Cons:**
- Certificate expires (need to update firmware when renewed)
- Brittle - breaks if Cloudflare changes certificate provider
- Doesn't solve the underlying ECDSA validation issue

**Not recommended** - high maintenance overhead

## Debug Logs

### Successful Timezone API Connection
```
[8052][DEBUG][TIME] Starting timezone fetch
[8061][DEBUG][TIME] Certificate bundle configured (146 CAs)
[8076][DEBUG][TIME] HTTPS connection established
[8808][DEBUG][TIME] GET request completed
```

### Failed OTA Connection (Before Workaround)
```
[14168][INFO][OTA] Checking for firmware updates
[OTA] Full URL: https://slack-reactions.devita.dev/api/firmware/version?...
[OTA] Certificate bundle size: 67551 bytes
[OTA] Certificate bundle configured
[OTA] HTTPClient initialized
E (15660) esp-x509-crt-bundle: Failed to verify certificate
[ 15670][E][ssl_client.cpp:36] _handle_error(): [ssl_starttls_handshake():317]: (-12288) X509 - A fatal error occurred, eg the chain is too long or the vrfy callback failed
[ 15687][E][NetworkClientSecure.cpp:159] connect(): start_ssl_client: connect failed: -12288
```

## Certificate Chain Details

### OTA Server (Cloudflare)
```bash
$ openssl s_client -connect slack-reactions.devita.dev:443 -servername slack-reactions.devita.dev -showcerts

Certificate chain
 0 s:CN=devita.dev
   i:C=US, O=Google Trust Services, CN=WE1
   a:PKEY: EC, (prime256v1); sigalg: ecdsa-with-SHA256
   v:NotBefore: Sep 16 05:02:00 2025 GMT; NotAfter: Dec 15 05:59:40 2025 GMT

 1 s:C=US, O=Google Trust Services, CN=WE1
   i:C=US, O=Google Trust Services LLC, CN=GTS Root R4

 2 s:C=US, O=Google Trust Services LLC, CN=GTS Root R4
   i:C=US, O=Google Trust Services LLC, CN=GTS Root R4

Subject Alternative Names: DNS:devita.dev, DNS:*.devita.dev
```

### Timezone API (Let's Encrypt)
```bash
$ openssl s_client -connect api.ipgeolocation.io:443 -servername api.ipgeolocation.io -showcerts

Certificate chain
 0 s:CN=*.ipgeolocation.io
   i:C=US, O=Let's Encrypt, CN=R11
   a:PKEY: RSA, 2048 (bit); sigalg: sha256WithRSAEncryption
   v:NotBefore: Aug 20 01:58:53 2025 GMT; NotAfter: Nov 18 01:58:52 2025 GMT

 1 s:C=US, O=Let's Encrypt, CN=R11
   i:C=US, O=Internet Security Research Group, CN=ISRG Root X1
```

## Related Code Locations

- **OTA Manager**: `src/ota/OTAManager.cpp:33-51` (checkForUpdate)
- **OTA Manager**: `src/ota/OTAManager.cpp:168-171` (downloadAndInstall)
- **WebSocket**: `src/main.cpp:2324` (also uses insecure mode)
- **Timezone API**: `src/main.cpp:1187-1192` (works with certificate bundle)
- **Emoji Downloads**: `src/main.cpp:467-470` (works with certificate bundle)

## References

- ESP32 Arduino Core: 3.3.2 (pioarduino platform)
- ESP-IDF Version: 5.5.1
- mbedTLS Version: 3.6.0 (included in ESP-IDF 5.5.1)
- Mozilla CA Bundle: 146 root certificates (67,551 bytes)
- Cloudflare Tunnel: Used for exposing local server to internet

## Recommendation

**Short term:** Keep current `setInsecure()` workaround
- Low risk: Firmware integrity protected by hash + signature verification
- Matches WebSocket behavior (also uses insecure mode)
- Minimal attack surface (home network, signed firmware)

**Long term:** Investigate Option 1 (force RSA certs in Cloudflare)
- Would restore proper certificate validation
- No ESP32 code changes needed
- Aligns with working timezone API pattern

**If time permits:** Option 2 (debug mbedTLS)
- Could identify upstream bug or config issue
- Would benefit the ESP32 community
- May lead to proper fix in future ESP-IDF release

---

**Last Updated:** January 27, 2025
**Maintainer:** See git history for contributors
