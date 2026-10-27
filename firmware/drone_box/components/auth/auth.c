#include "auth.h"

#include <stdio.h>
#include <string.h>

static auth_hmac_fn s_hmac;

void auth_init(auth_hmac_fn hmac)
{
    s_hmac = hmac;
}

// Append "<len>:<bytes>". Returns the new write offset, or 0 if it would not
// fit -- 0 is unambiguous as a failure because a successful first field always
// writes at least two characters.
static size_t put_bytes(char *out, size_t out_len, size_t w,
                        const void *value, size_t len)
{
    const char *v = value ? (const char *)value : "";

    const int k = snprintf(out + w, out_len - w, "%zu:", len);
    if (k < 0 || (size_t)k >= out_len - w) {
        return 0;
    }
    w += (size_t)k;

    if (len + 1 > out_len - w) {            // + 1 for the terminator
        return 0;
    }
    memcpy(out + w, v, len);
    w += len;
    out[w] = '\0';
    return w;
}

static size_t put_field(char *out, size_t out_len, size_t w, const char *value)
{
    const char *v = value ? value : "";
    return put_bytes(out, out_len, w, v, strlen(v));
}

size_t auth_canonical(char *out, size_t out_len,
                      const char *method, const char *path,
                      const char *query, uint64_t nonce)
{
    if (out == NULL || out_len == 0) {
        return 0;
    }
    out[0] = '\0';

    char nonce_text[24];
    snprintf(nonce_text, sizeof(nonce_text), "%llu", (unsigned long long)nonce);

    size_t w = 0;
    if ((w = put_field(out, out_len, w, method))     == 0) return 0;
    if ((w = put_field(out, out_len, w, path))       == 0) return 0;
    if ((w = put_field(out, out_len, w, query))      == 0) return 0;
    if ((w = put_field(out, out_len, w, nonce_text)) == 0) return 0;
    return w;
}

size_t auth_canonical_blob(char *out, size_t out_len,
                           const char *method, const char *path,
                           const uint8_t *payload, size_t payload_len,
                           uint64_t nonce)
{
    if (out == NULL || out_len == 0) {
        return 0;
    }
    out[0] = '\0';

    char nonce_text[24];
    snprintf(nonce_text, sizeof(nonce_text), "%llu", (unsigned long long)nonce);

    size_t w = 0;
    if ((w = put_field(out, out_len, w, method)) == 0) return 0;
    if ((w = put_field(out, out_len, w, path))   == 0) return 0;
    if ((w = put_bytes(out, out_len, w, payload, payload_len)) == 0) return 0;
    if ((w = put_field(out, out_len, w, nonce_text)) == 0) return 0;
    return w;
}

bool auth_verify_blob(const uint8_t key[AUTH_KEY_LEN],
                      const char *method, const char *path,
                      const uint8_t *payload, size_t payload_len,
                      uint64_t nonce, const uint8_t sig[AUTH_SIG_LEN],
                      uint64_t high_water)
{
    if (s_hmac == NULL || key == NULL || sig == NULL) {
        return false;
    }
    if (!auth_nonce_acceptable(nonce, high_water)) {
        return false;
    }

    char canonical[AUTH_CANONICAL_MAX];
    const size_t len = auth_canonical_blob(canonical, sizeof(canonical),
                                           method, path, payload, payload_len, nonce);
    if (len == 0) {
        return false;
    }

    uint8_t expected[AUTH_SIG_LEN];
    s_hmac(key, (const uint8_t *)canonical, len, expected);
    return auth_equal_ct(expected, sig, AUTH_SIG_LEN);
}

bool auth_nonce_acceptable(uint64_t nonce, uint64_t high_water)
{
    if (nonce <= high_water) {
        return false;                       // replay, or out of order
    }
    return (nonce - high_water) <= AUTH_NONCE_MAX_SKIP;
}

bool auth_equal_ct(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);     // no early exit, on purpose
    }
    return diff == 0;
}

bool auth_verify(const uint8_t key[AUTH_KEY_LEN],
                 const char *method, const char *path, const char *query,
                 uint64_t nonce, const uint8_t sig[AUTH_SIG_LEN],
                 uint64_t high_water)
{
    if (s_hmac == NULL || key == NULL || sig == NULL) {
        return false;
    }

    // Checked before the HMAC because it is far cheaper, and because the
    // high-water mark is not a secret -- the app reads it to stay in step.
    if (!auth_nonce_acceptable(nonce, high_water)) {
        return false;
    }

    char canonical[AUTH_CANONICAL_MAX];
    const size_t len = auth_canonical(canonical, sizeof(canonical),
                                      method, path, query, nonce);
    if (len == 0) {
        return false;                       // oversized request: refuse, do not truncate
    }

    uint8_t expected[AUTH_SIG_LEN];
    s_hmac(key, (const uint8_t *)canonical, len, expected);
    return auth_equal_ct(expected, sig, AUTH_SIG_LEN);
}
