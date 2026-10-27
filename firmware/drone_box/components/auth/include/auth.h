// Request authorisation: canonical form, replay window, signature check.
//
// Implements spec FR1 (reject unauthorised commands) and FR2 (reject replays).
//
// This component contains NO hardware access and NO ESP-IDF headers, so it is
// unit tested on a host alongside `command`. The HMAC primitive is *injected*
// rather than called directly: on target that is mbedTLS HMAC-SHA256, and in
// tests it is a deterministic stand-in. What is tested here is the part that
// can be got wrong by reasoning -- how a request is turned into bytes, which
// nonces are acceptable, and how signatures are compared.
//
// Why signing rather than TLS: mbedTLS costs flash and RAM, the S3's handshake
// is slow, and a self-signed certificate is rejected by browsers anyway. Since
// only the paired app may control the dock, authenticity and replay protection
// are what is actually needed, and they cost a few kilobytes.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUTH_KEY_LEN        32      // device key, from esp_random() on first boot
#define AUTH_SIG_LEN        32      // HMAC-SHA256 output
#define AUTH_CANONICAL_MAX  384     // longest canonical form we will build

// A client may run ahead of the dock's high-water mark by at most this much.
//
// Without a cap, anyone who ever learns the key could send nonce = UINT64_MAX
// once and permanently lock out the real app -- a denial of service that
// survives reboots because the high-water mark is persisted. The app keeps its
// counter in step by reading the dock's current value, which is not a secret.
#define AUTH_NONCE_MAX_SKIP  1000000ULL

// HMAC-SHA256: key -> msg -> out. Injected so this component stays
// hardware-free and host-testable.
typedef void (*auth_hmac_fn)(const uint8_t key[AUTH_KEY_LEN],
                             const uint8_t *msg, size_t msg_len,
                             uint8_t out[AUTH_SIG_LEN]);

void auth_init(auth_hmac_fn hmac);

// Build the bytes that get signed. Fields are length-prefixed ("4:POST8:/api")
// rather than separator-joined, so no combination of method, path and query can
// be rearranged into a different request with the same canonical form. A
// newline-joined format would allow exactly that, because a query string can
// contain the separator.
//
// Returns the number of bytes written, or 0 if the buffer was too small.
size_t auth_canonical(char *out, size_t out_len,
                      const char *method, const char *path,
                      const char *query, uint64_t nonce);

// As auth_canonical(), but the last field is arbitrary bytes rather than a C
// string -- a Wi-Fi payload is "ssid\0password", so strlen() would silently
// sign only the SSID. The result may contain NUL bytes, so callers must use
// the returned length and never strlen() it.
size_t auth_canonical_blob(char *out, size_t out_len,
                           const char *method, const char *path,
                           const uint8_t *payload, size_t payload_len,
                           uint64_t nonce);

// Blob equivalent of auth_verify(), for BLE writes.
//
// BLE link encryption is not sufficient on its own: the dock pairs "just
// works", so any device in range can bond and obtain an encrypted link. Only a
// client that received the device key through the pairing window can sign.
bool auth_verify_blob(const uint8_t key[AUTH_KEY_LEN],
                      const char *method, const char *path,
                      const uint8_t *payload, size_t payload_len,
                      uint64_t nonce, const uint8_t sig[AUTH_SIG_LEN],
                      uint64_t high_water);

// Strictly increasing, and no further ahead than AUTH_NONCE_MAX_SKIP.
bool auth_nonce_acceptable(uint64_t nonce, uint64_t high_water);

// Compare in constant time: a byte-at-a-time early return would let an attacker
// discover a valid signature one byte at a time by timing the response.
bool auth_equal_ct(const uint8_t *a, const uint8_t *b, size_t len);

// Full check: nonce acceptable, canonical form builds, signature matches.
// The caller records the nonce only after this returns true.
bool auth_verify(const uint8_t key[AUTH_KEY_LEN],
                 const char *method, const char *path, const char *query,
                 uint64_t nonce, const uint8_t sig[AUTH_SIG_LEN],
                 uint64_t high_water);
