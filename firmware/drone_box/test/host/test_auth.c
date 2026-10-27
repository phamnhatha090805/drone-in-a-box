// Host unit tests for the `auth` component. Implements spec FR1 and FR2.
//
// These run on the development machine. The HMAC primitive is a deterministic
// stand-in, not real HMAC-SHA256 -- what is under test is the logic around the
// primitive (canonical form, replay window, comparison), not mbedTLS.
// Test IDs match the table in firmware/drone_box/README.md.

#include <string.h>

#include "auth.h"
#include "unity.h"

// A stand-in HMAC: not cryptographic, but avalanches on every input bit so a
// one-bit change anywhere in the message changes the output, which is the
// property these tests rely on.
static void fake_hmac(const uint8_t key[AUTH_KEY_LEN],
                      const uint8_t *msg, size_t len, uint8_t out[AUTH_SIG_LEN])
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < AUTH_KEY_LEN; i++) {
        h = (h ^ key[i]) * 1099511628211ULL;
    }
    for (size_t i = 0; i < len; i++) {
        h = (h ^ msg[i]) * 1099511628211ULL;
    }
    for (size_t i = 0; i < AUTH_SIG_LEN; i++) {
        h = (h ^ (uint64_t)i) * 1099511628211ULL;
        out[i] = (uint8_t)(h >> 24);
    }
}

static uint8_t KEY[AUTH_KEY_LEN];
static uint8_t OTHER_KEY[AUTH_KEY_LEN];

void setUp(void)
{
    for (size_t i = 0; i < AUTH_KEY_LEN; i++) {
        KEY[i]       = (uint8_t)(i * 7 + 1);
        OTHER_KEY[i] = (uint8_t)(i * 7 + 2);
    }
    auth_init(fake_hmac);
}

void tearDown(void) {}

static void sign(uint8_t out[AUTH_SIG_LEN], const uint8_t key[AUTH_KEY_LEN],
                 const char *method, const char *path, const char *query,
                 uint64_t nonce)
{
    char canonical[AUTH_CANONICAL_MAX];
    const size_t len = auth_canonical(canonical, sizeof(canonical),
                                      method, path, query, nonce);
    TEST_ASSERT_NOT_EQUAL_size_t(0, len);
    fake_hmac(key, (const uint8_t *)canonical, len, out);
}

// ---------------------------------------------------------------------------
// A01-A03: canonical form
// ---------------------------------------------------------------------------

static void test_canonical_is_length_prefixed(void)
{
    char buf[AUTH_CANONICAL_MAX];
    const size_t n = auth_canonical(buf, sizeof(buf), "POST", "/api/cmd", "c=e", 7);
    TEST_ASSERT_EQUAL_STRING("4:POST8:/api/cmd3:c=e1:7", buf);
    TEST_ASSERT_EQUAL_size_t(strlen(buf), n);
}

static void test_canonical_resists_field_splicing(void)
{
    // The reason for length prefixes. With a separator-joined format these two
    // different requests could produce the same bytes to sign, so a signature
    // captured for one would authorise the other.
    char a[AUTH_CANONICAL_MAX], b[AUTH_CANONICAL_MAX];
    auth_canonical(a, sizeof(a), "POST", "/api/cmd", "c=e", 1);
    auth_canonical(b, sizeof(b), "POST", "/api",     "/cmdc=e", 1);
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);

    auth_canonical(a, sizeof(a), "GET", "/a", "b", 1);
    auth_canonical(b, sizeof(b), "GET", "/ab", "", 1);
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
}

static void test_canonical_refuses_to_truncate(void)
{
    // Silently truncating would make two different requests sign identically.
    char small[8];
    TEST_ASSERT_EQUAL_size_t(0, auth_canonical(small, sizeof(small),
                                               "POST", "/api/command/long", "c=e", 1));
    TEST_ASSERT_EQUAL_size_t(0, auth_canonical(small, 0, "POST", "/a", "", 1));

    char buf[AUTH_CANONICAL_MAX];
    TEST_ASSERT_NOT_EQUAL_size_t(0, auth_canonical(buf, sizeof(buf), "", "", "", 0));
}

// ---------------------------------------------------------------------------
// A04-A06: replay window
// ---------------------------------------------------------------------------

static void test_nonce_must_increase(void)
{
    TEST_ASSERT_TRUE(auth_nonce_acceptable(11, 10));
    TEST_ASSERT_FALSE(auth_nonce_acceptable(10, 10));   // the replay case
    TEST_ASSERT_FALSE(auth_nonce_acceptable(9, 10));
    TEST_ASSERT_FALSE(auth_nonce_acceptable(0, 10));
}

static void test_nonce_skip_is_capped(void)
{
    TEST_ASSERT_TRUE(auth_nonce_acceptable(10 + AUTH_NONCE_MAX_SKIP, 10));
    TEST_ASSERT_FALSE(auth_nonce_acceptable(10 + AUTH_NONCE_MAX_SKIP + 1, 10));

    // The lockout a cap exists to prevent: one request at the top of the range
    // would otherwise make every future nonce unacceptable, permanently.
    TEST_ASSERT_FALSE(auth_nonce_acceptable(UINT64_MAX, 10));
}

static void test_nonce_from_zero(void)
{
    TEST_ASSERT_TRUE(auth_nonce_acceptable(1, 0));      // fresh device
    TEST_ASSERT_FALSE(auth_nonce_acceptable(0, 0));
}

// ---------------------------------------------------------------------------
// A07-A11: verification
// ---------------------------------------------------------------------------

static void test_valid_request_is_accepted(void)
{
    uint8_t sig[AUTH_SIG_LEN];
    sign(sig, KEY, "POST", "/api/cmd", "c=e", 100);
    TEST_ASSERT_TRUE(auth_verify(KEY, "POST", "/api/cmd", "c=e", 100, sig, 99));
}

static void test_replayed_request_is_rejected(void)
{
    // The captured-and-resent case: the signature is perfectly valid, and the
    // request must still be refused because the nonce has been seen.
    uint8_t sig[AUTH_SIG_LEN];
    sign(sig, KEY, "POST", "/api/cmd", "c=e", 100);
    TEST_ASSERT_TRUE(auth_verify(KEY, "POST", "/api/cmd", "c=e", 100, sig, 99));
    TEST_ASSERT_FALSE(auth_verify(KEY, "POST", "/api/cmd", "c=e", 100, sig, 100));
}

static void test_any_tampering_invalidates(void)
{
    uint8_t sig[AUTH_SIG_LEN];
    sign(sig, KEY, "POST", "/api/cmd", "c=e", 100);

    TEST_ASSERT_FALSE(auth_verify(KEY, "GET",  "/api/cmd", "c=e", 100, sig, 99));
    TEST_ASSERT_FALSE(auth_verify(KEY, "POST", "/api/stop", "c=e", 100, sig, 99));
    TEST_ASSERT_FALSE(auth_verify(KEY, "POST", "/api/cmd", "c=r", 100, sig, 99));
    TEST_ASSERT_FALSE(auth_verify(KEY, "POST", "/api/cmd", "c=e", 101, sig, 99));
    TEST_ASSERT_FALSE(auth_verify(OTHER_KEY, "POST", "/api/cmd", "c=e", 100, sig, 99));

    // A single flipped bit in the signature.
    uint8_t tampered[AUTH_SIG_LEN];
    memcpy(tampered, sig, sizeof(tampered));
    tampered[17] ^= 0x01;
    TEST_ASSERT_FALSE(auth_verify(KEY, "POST", "/api/cmd", "c=e", 100, tampered, 99));
}

static void test_stop_is_not_privileged(void)
{
    // Spec decision: an unauthenticated stop would let a stranger block drone
    // recovery. Physical safety is a wired E-stop, not an open endpoint.
    uint8_t zeros[AUTH_SIG_LEN] = {0};
    TEST_ASSERT_FALSE(auth_verify(KEY, "POST", "/api/cmd", "c=s", 100, zeros, 99));
}

static void test_verify_without_init_fails_closed(void)
{
    uint8_t sig[AUTH_SIG_LEN];
    sign(sig, KEY, "POST", "/api/cmd", "c=e", 100);
    auth_init(NULL);                        // primitive missing
    TEST_ASSERT_FALSE(auth_verify(KEY, "POST", "/api/cmd", "c=e", 100, sig, 99));
}

static void test_constant_time_compare_is_correct(void)
{
    const uint8_t a[4] = {1, 2, 3, 4};
    const uint8_t b[4] = {1, 2, 3, 4};
    const uint8_t c[4] = {1, 2, 3, 5};      // differs in the last byte only
    const uint8_t d[4] = {9, 2, 3, 4};      // differs in the first byte only

    TEST_ASSERT_TRUE(auth_equal_ct(a, b, 4));
    TEST_ASSERT_FALSE(auth_equal_ct(a, c, 4));
    TEST_ASSERT_FALSE(auth_equal_ct(a, d, 4));
    TEST_ASSERT_TRUE(auth_equal_ct(a, b, 0));
}

// ---------------------------------------------------------------------------
// A13-A15: binary payloads (BLE writes)
// ---------------------------------------------------------------------------

static void test_blob_covers_bytes_after_a_nul(void)
{
    // The reason this variant exists. A Wi-Fi payload is "ssid\0password", so
    // a strlen-based canonical form would sign the SSID and leave the password
    // unprotected -- an attacker could swap it for one they control.
    const uint8_t a[] = { 'n','e','t', 0, 's','e','c','r','e','t' };
    const uint8_t b[] = { 'n','e','t', 0, 'a','t','t','a','c','k' };

    char ca[AUTH_CANONICAL_MAX], cb[AUTH_CANONICAL_MAX];
    const size_t na = auth_canonical_blob(ca, sizeof(ca), "BLE", "/ble/provision",
                                          a, sizeof(a), 1);
    const size_t nb = auth_canonical_blob(cb, sizeof(cb), "BLE", "/ble/provision",
                                          b, sizeof(b), 1);
    TEST_ASSERT_EQUAL_size_t(na, nb);
    TEST_ASSERT_TRUE(memcmp(ca, cb, na) != 0);
}

// Pinned to the exact bytes, not just "different from the other one". The
// same literal is asserted in DockCryptoTest.kt and dock_cli.py; the three are
// cross-checked by tools/check-wire-format.sh. Without this, the C side could
// change the blob format and every C test would still pass while the app
// silently started failing to authenticate.
static void test_blob_canonical_exact_vector(void)
{
    const uint8_t payload[] = { 'n','e','t', 0, 's','e','c','r','e','t' };
    const char expected[] = "3:BLE14:/ble/provision10:net\0secret1:1";
    const size_t expected_len = sizeof(expected) - 1;   // keeps the embedded NUL

    char out[AUTH_CANONICAL_MAX];
    const size_t n = auth_canonical_blob(out, sizeof(out), "BLE", "/ble/provision",
                                         payload, sizeof(payload), 1);
    TEST_ASSERT_EQUAL_size_t(expected_len, n);
    TEST_ASSERT_EQUAL_MEMORY(expected, out, expected_len);
}

static void test_blob_length_is_part_of_the_signature(void)
{
    // A truncated payload must not verify against the full one's signature.
    const uint8_t full[] = { 'n','e','t', 0, 'p','a','s','s' };
    char c1[AUTH_CANONICAL_MAX], c2[AUTH_CANONICAL_MAX];
    const size_t n1 = auth_canonical_blob(c1, sizeof(c1), "BLE", "/p", full, sizeof(full), 1);
    const size_t n2 = auth_canonical_blob(c2, sizeof(c2), "BLE", "/p", full, 3, 1);
    TEST_ASSERT_TRUE(n1 != n2 || memcmp(c1, c2, n1) != 0);
}

static void test_blob_verify_round_trip(void)
{
    const uint8_t payload[] = { 'e' };
    char canonical[AUTH_CANONICAL_MAX];
    const size_t len = auth_canonical_blob(canonical, sizeof(canonical),
                                           "BLE", "/ble/cmd", payload, sizeof(payload), 100);
    uint8_t sig[AUTH_SIG_LEN];
    fake_hmac(KEY, (const uint8_t *)canonical, len, sig);

    TEST_ASSERT_TRUE(auth_verify_blob(KEY, "BLE", "/ble/cmd",
                                      payload, sizeof(payload), 100, sig, 99));
    // Replay
    TEST_ASSERT_FALSE(auth_verify_blob(KEY, "BLE", "/ble/cmd",
                                       payload, sizeof(payload), 100, sig, 100));
    // Different command
    const uint8_t other[] = { 'r' };
    TEST_ASSERT_FALSE(auth_verify_blob(KEY, "BLE", "/ble/cmd",
                                       other, sizeof(other), 100, sig, 99));
    // An encrypted-but-unpaired device has no key, so it cannot sign at all.
    TEST_ASSERT_FALSE(auth_verify_blob(OTHER_KEY, "BLE", "/ble/cmd",
                                       payload, sizeof(payload), 100, sig, 99));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_canonical_is_length_prefixed);
    RUN_TEST(test_canonical_resists_field_splicing);
    RUN_TEST(test_canonical_refuses_to_truncate);

    RUN_TEST(test_nonce_must_increase);
    RUN_TEST(test_nonce_skip_is_capped);
    RUN_TEST(test_nonce_from_zero);

    RUN_TEST(test_valid_request_is_accepted);
    RUN_TEST(test_replayed_request_is_rejected);
    RUN_TEST(test_any_tampering_invalidates);
    RUN_TEST(test_stop_is_not_privileged);
    RUN_TEST(test_verify_without_init_fails_closed);
    RUN_TEST(test_constant_time_compare_is_correct);

    RUN_TEST(test_blob_covers_bytes_after_a_nul);
    RUN_TEST(test_blob_canonical_exact_vector);
    RUN_TEST(test_blob_length_is_part_of_the_signature);
    RUN_TEST(test_blob_verify_round_trip);

    return UNITY_END();
}
