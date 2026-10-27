#include "secrets.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "psa/crypto.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "secrets";

#define NVS_NAMESPACE  "dock"
#define KEY_DEVKEY     "devkey"
#define KEY_NONCE      "nonce"
#define KEY_SSID       "sta_ssid"
#define KEY_PASS       "sta_pass"

static uint8_t  s_key[AUTH_KEY_LEN];

// ESP-IDF 6.0 ships mbedTLS 4, where the legacy mbedtls_md_* one-shot HMAC is
// gone and PSA Crypto is the API. The key is imported once and reused: an
// import and destroy per request would be pure overhead on every command.
static mbedtls_svc_key_id_t s_psa_key;
static bool                 s_psa_ready;
static bool     s_key_is_new;
static uint64_t s_high_water;
static bool     s_pairing_open;
static esp_timer_handle_t s_pairing_timer;
static uint64_t s_persisted;

static esp_err_t open_nvs(nvs_open_mode_t mode, nvs_handle_t *out)
{
    return nvs_open(NVS_NAMESPACE, mode, out);
}

// (Re)import s_key into PSA. Called after the key is loaded or rotated.
static void import_psa_key(void)
{
    if (s_psa_ready) {
        psa_destroy_key(s_psa_key);
        s_psa_ready = false;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, AUTH_KEY_LEN * 8);

    if (psa_import_key(&attr, s_key, sizeof(s_key), &s_psa_key) == PSA_SUCCESS) {
        s_psa_ready = true;
    } else {
        ESP_LOGE(TAG, "PSA key import failed -- every request will be refused");
    }
}

static esp_err_t generate_key(nvs_handle_t h)
{
    esp_fill_random(s_key, sizeof(s_key));  // hardware RNG, not rand()
    s_key_is_new = true;

    const esp_err_t err = nvs_set_blob(h, KEY_DEVKEY, s_key, sizeof(s_key));
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(h);
}

esp_err_t secrets_init(void)
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t h;
    err = open_nvs(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(s_key);
    err = nvs_get_blob(h, KEY_DEVKEY, s_key, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || len != sizeof(s_key)) {
        err = generate_key(h);
        if (err != ESP_OK) {
            nvs_close(h);
            return err;
        }
    } else if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    if (nvs_get_u64(h, KEY_NONCE, &s_persisted) != ESP_OK) {
        s_persisted = 0;
    }
    // Resume beyond anything that may have been accepted but not yet written.
    s_high_water = s_persisted + SECRETS_NONCE_STRIDE;

    nvs_close(h);
    import_psa_key();

    ESP_LOGI(TAG, "device key %s, nonce resumes at %llu",
             s_key_is_new ? "GENERATED (dock has never been paired)" : "loaded",
             (unsigned long long)s_high_water);
    return ESP_OK;
}

const uint8_t *secrets_device_key(void) { return s_key; }
bool           secrets_key_is_new(void) { return s_key_is_new; }

esp_err_t secrets_rotate_key(void)
{
    nvs_handle_t h;
    esp_err_t err = open_nvs(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = generate_key(h);
    nvs_close(h);
    import_psa_key();
    ESP_LOGW(TAG, "device key rotated -- every paired app must pair again");
    return err;
}

// --- pairing window ---------------------------------------------------------
static void pairing_expired(void *arg)
{
    (void)arg;
    secrets_close_pairing("window expired");
}

void secrets_open_pairing(uint32_t seconds)
{
    if (s_pairing_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = pairing_expired, .name = "pairing",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_pairing_timer));
    }
    s_pairing_open = true;
    esp_timer_stop(s_pairing_timer);
    esp_timer_start_once(s_pairing_timer, (uint64_t)seconds * 1000000ULL);

    printf("\n*** PAIRING OPEN for %lu s ***\n", (unsigned long)seconds);
    printf("A device can take the key now. It closes on first handover.\n\n");
    fflush(stdout);
}

void secrets_close_pairing(const char *why)
{
    if (!s_pairing_open) {
        return;
    }
    s_pairing_open = false;
    if (s_pairing_timer) {
        esp_timer_stop(s_pairing_timer);
    }
    printf("PAIR:  closed (%s)\n", why);
    fflush(stdout);
}

bool secrets_pairing_open(void) { return s_pairing_open; }

void secrets_note_key_handover(const char *how)
{
    printf("PAIR:  key handed over via %s\n", how);
    fflush(stdout);
    secrets_close_pairing("key handed over");
}

uint64_t secrets_nonce_high_water(void) { return s_high_water; }

void secrets_note_nonce(uint64_t nonce)
{
    if (nonce <= s_high_water) {
        return;                             // auth_verify should have refused this
    }
    s_high_water = nonce;

    if (s_high_water < s_persisted + SECRETS_NONCE_STRIDE) {
        return;                             // still inside the window already on flash
    }

    nvs_handle_t h;
    if (open_nvs(NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_u64(h, KEY_NONCE, s_high_water) == ESP_OK && nvs_commit(h) == ESP_OK) {
        s_persisted = s_high_water;
    }
    nvs_close(h);
}

esp_err_t secrets_set_wifi(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = open_nvs(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, KEY_SSID, ssid ? ssid : "");
    if (err == ESP_OK) {
        err = nvs_set_str(h, KEY_PASS, password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    // Deliberately not logged: the password would end up in the console buffer.
    ESP_LOGI(TAG, "wifi credentials %s", err == ESP_OK ? "stored" : "NOT stored");
    return err;
}

bool secrets_get_wifi(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    nvs_handle_t h;
    if (open_nvs(NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    size_t n1 = ssid_len, n2 = password_len;
    const bool ok = nvs_get_str(h, KEY_SSID, ssid, &n1) == ESP_OK &&
                    nvs_get_str(h, KEY_PASS, password, &n2) == ESP_OK &&
                    ssid[0] != '\0';
    nvs_close(h);
    return ok;
}

esp_err_t secrets_clear_wifi(void)
{
    nvs_handle_t h;
    esp_err_t err = open_nvs(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_key(h, KEY_SSID);
    nvs_erase_key(h, KEY_PASS);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void secrets_hmac_sha256(const uint8_t key[AUTH_KEY_LEN],
                         const uint8_t *msg, size_t msg_len,
                         uint8_t out[AUTH_SIG_LEN])
{
    (void)key;                              // the imported PSA key is the key

    size_t written = 0;
    if (!s_psa_ready ||
        psa_mac_compute(s_psa_key, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                        msg, msg_len, out, AUTH_SIG_LEN, &written) != PSA_SUCCESS ||
        written != AUTH_SIG_LEN) {
        // Fail closed: a zeroed digest will not match any real signature.
        memset(out, 0, AUTH_SIG_LEN);
    }
}
