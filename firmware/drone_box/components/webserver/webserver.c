#include "webserver.h"

#include <string.h>

#include <stdlib.h>

#include "auth.h"
#include "command.h"
#include "controller.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "actuator.h"
#include "secrets.h"
#include "session.h"
#include "wifi_link.h"

static const char *TAG = "webserver";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// An SSID escapes to at most two bytes per byte (every one a quote or a
// backslash), and each entry costs about 60 bytes of JSON around it.
#define SCAN_SSID_JSON_MAX  (2 * (WIFI_SSID_TEXT_MAX - 1) + 1)
#define SCAN_JSON_MAX       (16 + WIFI_SCAN_MAX_APS * (SCAN_SSID_JSON_MAX + 64))

// ---------------------------------------------------------------------------
// Authorisation (spec FR1, FR2)
//
// Every API request carries X-Dock-Nonce and X-Dock-Sig. The signature covers
// method, path, query and nonce, so none of them can be altered in flight, and
// the nonce must exceed everything already accepted, so a captured request
// cannot be replayed.
//
// Stop is not privileged: an open stop endpoint would let a stranger in range
// block drone recovery. Physical safety is a wired E-stop.
//
// TEMPORARILY DISABLED (DOCK_REQUIRE_AUTH 0). The whole check is switched off
// at compile time while the mobile app is being rebuilt: with it on, every
// client needs the device key before it can move the actuator, and that is a
// decision we have deferred rather than made. Nothing below was deleted --
// auth.c, its host tests and the key handling are untouched, so turning this
// back to 1 restores the signed API exactly as it was.
//
// What an open API means in practice: anyone who can reach the dock -- anyone
// on its access point, whose password is a constant in this repository, or on
// any network it has joined -- can drive the actuator. Keep it on the bench.
// ---------------------------------------------------------------------------
#define DOCK_REQUIRE_AUTH 0

static bool authorize(httpd_req_t *req, const char *method, const char *path,
                      char *query, size_t query_len, const char **why)
{
    if (httpd_req_get_url_query_str(req, query, query_len) != ESP_OK) {
        query[0] = '\0';                    // no query is a valid request shape
    }

#if !DOCK_REQUIRE_AUTH
    (void)method;
    (void)path;
    *why = NULL;
    return true;
#else
    char nonce_text[24], sig_text[72];

    if (httpd_req_get_hdr_value_str(req, "X-Dock-Nonce",
                                    nonce_text, sizeof(nonce_text)) != ESP_OK) {
        *why = "missing X-Dock-Nonce";
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "X-Dock-Sig",
                                    sig_text, sizeof(sig_text)) != ESP_OK) {
        *why = "missing X-Dock-Sig";
        return false;
    }

    char *end = NULL;
    const uint64_t nonce = strtoull(nonce_text, &end, 10);
    if (end == nonce_text || (end && *end != '\0')) {
        *why = "malformed nonce";
        return false;
    }

    uint8_t sig[AUTH_SIG_LEN];
    size_t  sig_len = 0;
    if (mbedtls_base64_decode(sig, sizeof(sig), &sig_len,
                              (const unsigned char *)sig_text,
                              strlen(sig_text)) != 0 || sig_len != AUTH_SIG_LEN) {
        *why = "malformed signature";
        return false;
    }

    if (!auth_nonce_acceptable(nonce, secrets_nonce_high_water())) {
        *why = "replayed or out-of-window nonce";
        return false;
    }
    if (!auth_verify(secrets_device_key(), method, path, query,
                     nonce, sig, secrets_nonce_high_water())) {
        *why = "bad signature";
        return false;
    }

    secrets_note_nonce(nonce);              // only after the signature checks out
    *why = NULL;
    return true;
#endif
}

// Is this request from a client on our own access point, rather than from a
// network we joined? Pairing is restricted to the AP: a device on your home
// Wi-Fi has no business taking the dock's key.
static bool client_is_on_softap(httpd_req_t *req)
{
    struct sockaddr_in6 addr;
    socklen_t len = sizeof(addr);

    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) != 0) {
        return false;                       // cannot tell: refuse
    }

    char ip[INET6_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET6, &addr.sin6_addr, ip, sizeof(ip));
    // lwIP reports IPv4 peers as v4-mapped (::FFFF:192.168.4.2), so a
    // substring test covers both forms.
    return strstr(ip, "192.168.4.") != NULL;
}

static esp_err_t refuse(httpd_req_t *req, const char *path, const char *why)
{
    printf("AUTH:  refused %s -- %s\n", path, why);
    fflush(stdout);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
static esp_err_t send_status(httpd_req_t *req)
{
    char json[320];
    controller_status_json(json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t root_get(httpd_req_t *req)
{
    // EMBED_FILES embeds the bytes verbatim with no NUL appended, so the
    // length is end-start. Subtracting 1 here silently truncated the last
    // byte of the page; EMBED_TXTFILES is the variant that adds a terminator.
    const size_t len = (size_t)(index_html_end - index_html_start);
    printf("HTTP:  GET / (%u bytes)\n", (unsigned)len);
    fflush(stdout);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

static esp_err_t status_get(httpd_req_t *req)
{
    char query[256];
    const char *why = NULL;

    if (!authorize(req, "GET", "/api/status", query, sizeof(query), &why)) {
        return refuse(req, "/api/status", why);
    }
    // Only an authorised poll may pet the watchdog. Otherwise a stranger could
    // hold the dead-man open after the real operator has gone.
    session_touch(LINK_HTTP);
    return send_status(req);
}

// Hands the device key to a client, once, during a pairing window.
//
// Deliberately unauthenticated -- the whole point is that the caller has no
// key yet. Two gates instead: the window must have been opened by a physical
// BOOT button press, and the caller must be on the dock's own access point.
// Neither alone would do: the AP password is a compile-time constant in this
// repository, and an always-open endpoint would hand the key to anyone.
static esp_err_t pair_get(httpd_req_t *req)
{
    if (!secrets_pairing_open()) {
        printf("PAIR:  refused -- press BOOT on the board first\n");
        fflush(stdout);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
            "{\"error\":\"closed\",\"hint\":\"press the BOOT button on the dock\"}");
    }
    if (!client_is_on_softap(req)) {
        printf("PAIR:  refused -- pairing is only available on the dock's own AP\n");
        fflush(stdout);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
            "{\"error\":\"wrong-network\",\"hint\":\"join the dock's own Wi-Fi to pair\"}");
    }

    const uint8_t *key = secrets_device_key();
    char json[128] = "{\"key\":\"";
    size_t w = strlen(json);
    for (size_t i = 0; i < AUTH_KEY_LEN; i++) {
        w += (size_t)snprintf(json + w, sizeof(json) - w, "%02x", key[i]);
    }
    snprintf(json + w, sizeof(json) - w, "\"}");

    secrets_note_key_handover("web");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// Network state, so the setup page can show where the dock is.
static esp_err_t net_get(httpd_req_t *req)
{
    char query[256];
    const char *why = NULL;
    if (!authorize(req, "GET", "/api/net", query, sizeof(query), &why)) {
        return refuse(req, "/api/net", why);
    }
    session_touch(LINK_HTTP);

    char json[192];
    snprintf(json, sizeof(json),
             "{\"staJoined\":%s,\"staIp\":\"%s\",\"apSsid\":\"%s\","
             "\"pairing\":%s}",
             wifi_link_station_joined() ? "true" : "false",
             wifi_link_station_ip(), WIFI_AP_SSID,
             secrets_pairing_open() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// ---------------------------------------------------------------------------
// Wi-Fi scan (so the operator picks a network instead of spelling one out)
// ---------------------------------------------------------------------------
// An SSID is 32 arbitrary bytes, not a string, and it comes from whatever is
// broadcasting nearby -- so it is untrusted input on the way into a JSON
// document the browser will parse. Two things have to hold:
//
//   * `"` and `\` are escaped, and C0 controls are replaced rather than
//     escaped, because \uXXXX would let one SSID triple in size.
//   * the result is valid UTF-8, or JSON.parse() rejects the whole response
//     and one misbehaving neighbour hides every network from the operator.
//
// Bytes that are not valid UTF-8 become '?'. That loses fidelity on a badly
// encoded SSID, which is why manual entry stays on the page.
static size_t json_escape_ssid(const char *in, char *out, size_t out_len)
{
    size_t w = 0;

    for (size_t r = 0; in[r] != '\0'; r++) {
        const unsigned char c = (unsigned char)in[r];
        char   buf[4];
        size_t len = 1;

        if (c == '"' || c == '\\') {
            buf[0] = '\\';
            buf[1] = (char)c;
            len    = 2;
        } else if (c < 0x20 || c == 0x7F) {
            buf[0] = '?';
        } else if (c < 0x80) {
            buf[0] = (char)c;
        } else {
            // Multi-byte UTF-8: copy the sequence only if it is well formed.
            // 0xC0/0xC1 are overlong two-byte forms and 0xF5+ is out of range,
            // so both are rejected by the table below.
            const size_t need = (c >= 0xC2 && c <= 0xDF) ? 2
                              : (c >= 0xE0 && c <= 0xEF) ? 3
                              : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
            bool ok = need > 0;
            for (size_t k = 1; ok && k < need; k++) {
                const unsigned char cont = (unsigned char)in[r + k];
                ok = cont >= 0x80 && cont <= 0xBF;
            }
            if (ok) {
                memcpy(buf, in + r, need);
                len  = need;
                r   += need - 1;        // the loop's r++ steps past the last
            } else {
                buf[0] = '?';           // advance one byte and resynchronise
            }
        }

        if (w + len >= out_len) {
            break;                      // truncate rather than overrun
        }
        memcpy(out + w, buf, len);
        w += len;
    }

    out[w < out_len ? w : out_len - 1] = '\0';
    return w;
}

static esp_err_t scan_get(httpd_req_t *req)
{
    char query[256];
    const char *why = NULL;

    if (!authorize(req, "GET", "/api/scan", query, sizeof(query), &why)) {
        return refuse(req, "/api/scan", why);
    }
    session_touch(LINK_HTTP);

    // A scan blocks for about WIFI_SCAN_BUDGET_MS, which is longer than
    // SESSION_TIMEOUT_MS -- so running one while the actuator is energised
    // would have the dead-man stop the actuator mid-move. Refuse instead.
    //
    // The test is actuator_direction(), not the "moving" field of the status
    // JSON: that one is positioner_moving(), which covers a timed move but not
    // a manual jog, while actuator_direction() is what session.c's own watchdog
    // looks at. Those have to agree, or this endpoint would wave through
    // exactly the case the watchdog then punishes.
    //
    // It is a point-in-time check, not a lock: a BLE client on its own task
    // could still start a move once the scan is under way. That client pets
    // its own half of the dead-man, so nothing gets stopped out from under it,
    // and the HTTP caller is told the truth about the moment it asked.
    if (actuator_direction() != 's') {
        printf("SCAN:  refused -- actuator is moving (direction %c)\n",
               actuator_direction());
        fflush(stdout);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
            "{\"error\":\"moving\",\"hint\":\"stop the actuator before scanning: a "
            "scan takes longer than the dead-man timeout and would halt the actuator\"}");
    }

    // Both buffers go on the heap: the HTTP task's stack is 4 kB and these
    // come to nearly 3 kB between them.
    wifi_link_ap_t *aps = calloc(WIFI_SCAN_MAX_APS, sizeof(*aps));
    char           *json = calloc(1, SCAN_JSON_MAX);
    if (aps == NULL || json == NULL) {
        free(aps);
        free(json);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    size_t    found = 0;
    esp_err_t err   = wifi_link_scan(aps, WIFI_SCAN_MAX_APS, &found);
    if (err != ESP_OK) {
        free(aps);
        free(json);
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        // INVALID_STATE means the radio is not up yet or a sweep is already
        // running: both are "ask again shortly", not a broken request.
        if (err == ESP_ERR_INVALID_STATE) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req,
                "{\"error\":\"busy\",\"hint\":\"the radio is busy, try again in a moment\"}");
        }
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
            "{\"error\":\"scan-failed\",\"hint\":\"the radio could not scan\"}");
    }

    size_t w = (size_t)snprintf(json, SCAN_JSON_MAX, "{\"networks\":[");
    for (size_t i = 0; i < found; i++) {
        char ssid[SCAN_SSID_JSON_MAX];
        json_escape_ssid(aps[i].ssid, ssid, sizeof(ssid));
        const int n = snprintf(json + w, SCAN_JSON_MAX - w,
                               "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s,\"auth\":\"%s\"}",
                               i == 0 ? "" : ",", ssid, (int)aps[i].rssi,
                               aps[i].open ? "true" : "false",
                               wifi_link_auth_name(aps[i].authmode));
        if (n < 0 || (size_t)n >= SCAN_JSON_MAX - w) {
            break;                      // sized not to happen; stay well formed
        }
        w += (size_t)n;
    }
    snprintf(json + w, SCAN_JSON_MAX - w, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t sent = httpd_resp_sendstr(req, json);

    free(aps);
    free(json);
    return sent;
}

// Provision Wi-Fi. Credentials ride in the signed query rather than an
// unsigned body: the signature covers method, path and query, so putting them
// in a body would leave them alterable in flight. They are never logged.
static esp_err_t wifi_post(httpd_req_t *req)
{
    char query[256], raw[96], ssid[SECRETS_SSID_MAX], pass[SECRETS_PASS_MAX];
    const char *why = NULL;

    if (!authorize(req, "POST", "/api/wifi", query, sizeof(query), &why)) {
        return refuse(req, "/api/wifi", why);
    }
    session_touch(LINK_HTTP);

    if (httpd_query_key_value(query, "ssid", raw, sizeof(raw)) != ESP_OK ||
        command_url_decode(raw, ssid, sizeof(ssid)) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected ssid");
        return ESP_FAIL;
    }
    if (httpd_query_key_value(query, "password", raw, sizeof(raw)) != ESP_OK) {
        raw[0] = '\0';                      // open network
    }
    command_url_decode(raw, pass, sizeof(pass));

    if (secrets_set_wifi(ssid, pass) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not store");
        return ESP_FAIL;
    }
    printf("WiFi:  provisioned \"%s\" from the web UI -- joining\n", ssid);
    fflush(stdout);
    wifi_link_reprovision();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Unauthenticated on purpose: the high-water mark is not a secret, and the app
// needs it to pick its next nonce after a reinstall or a dock swap. Knowing it
// does not help an attacker, who still cannot produce a signature.
static esp_err_t nonce_get(httpd_req_t *req)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"nonce\":%llu}",
             (unsigned long long)secrets_nonce_high_water());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t cmd_post(httpd_req_t *req)
{
    char query[256], value[8], decoded[8];
    const char *why = NULL;

    if (!authorize(req, "POST", "/api/cmd", query, sizeof(query), &why)) {
        return refuse(req, "/api/cmd", why);
    }
    session_touch(LINK_HTTP);

    if (httpd_query_key_value(query, "c", value, sizeof(value)) != ESP_OK ||
        value[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected ?c=<command>");
        return ESP_FAIL;
    }

    // httpd_query_key_value() returns the value still percent-encoded, so
    // "?c=%2B" arrives as "%2B" and must be decoded before it means anything.
    if (command_url_decode(value, decoded, sizeof(decoded)) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty command");
        return ESP_FAIL;
    }

    char reply[CONTROLLER_REPLY_MAX];
    if (controller_handle((uint8_t)decoded[0], reply, sizeof(reply))) {
        ESP_LOGI(TAG, "web: %c -> %s", decoded[0], reply);
    } else {
        ESP_LOGW(TAG, "web: ignored byte 0x%02x", (unsigned)decoded[0]);
    }
    return send_status(req);                // the client always gets fresh state
}

void webserver_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;         // a stale browser must not hog a socket
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    static const httpd_uri_t routes[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = root_get },
        { .uri = "/api/status", .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/nonce",  .method = HTTP_GET,  .handler = nonce_get },
        { .uri = "/api/pair",   .method = HTTP_GET,  .handler = pair_get },
        { .uri = "/api/net",    .method = HTTP_GET,  .handler = net_get },
        { .uri = "/api/scan",   .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/api/wifi",   .method = HTTP_POST, .handler = wifi_post },
        { .uri = "/api/cmd",    .method = HTTP_POST, .handler = cmd_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }

    printf("WiFi:  HTTP server listening\n");
    fflush(stdout);
}
