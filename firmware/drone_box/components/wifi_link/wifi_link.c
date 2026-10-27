#include "wifi_link.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "secrets.h"

#if __has_include("mdns.h")
#include "mdns.h"
#define HAVE_MDNS 1
#endif

static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static esp_timer_handle_t s_retry_timer;
static uint32_t s_retry_ms = WIFI_RETRY_MIN_MS;
static bool s_joined;
static bool s_started;                  // esp_wifi_start() has returned
static bool s_scanning;                 // one sweep at a time
static char s_sta_ip[16] = "-";

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("WiFi:  ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}

static bool configure_station(void)
{
    char ssid[SECRETS_SSID_MAX] = {0};
    char pass[SECRETS_PASS_MAX] = {0};

    if (!secrets_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
        return false;
    }

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    say("station configured for \"%s\"", ssid);   // never log the password
    return true;
}

static void retry_now(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void schedule_retry(void)
{
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)s_retry_ms * 1000);

    s_retry_ms *= 2;                        // back off, but never give up: the
    if (s_retry_ms > WIFI_RETRY_MAX_MS) {   // hotspot may be switched on later
        s_retry_ms = WIFI_RETRY_MAX_MS;
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_AP_START:
            say("AP \"%s\" up at 192.168.4.1", WIFI_AP_SSID);
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            const wifi_event_ap_staconnected_t *e = data;
            say("client joined %02x:%02x:%02x:%02x:%02x:%02x (aid %d)",
                e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            const wifi_event_ap_stadisconnected_t *e = data;
            say("client left   %02x:%02x:%02x:%02x:%02x:%02x",
                e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
            break;
        }
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *e = data;
            if (s_joined) {
                say("station lost (reason %d) -- AP still up, retrying", e->reason);
            }
            s_joined = false;
            snprintf(s_sta_ip, sizeof(s_sta_ip), "-");
            schedule_retry();
            break;
        }
        default:
            break;
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        s_joined   = true;
        s_retry_ms = WIFI_RETRY_MIN_MS;     // a good join resets the backoff
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
        say("station joined, address %s (also reachable as %s.local)",
            s_sta_ip, WIFI_HOSTNAME);
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        const ip_event_assigned_ip_to_client_t *e = data;
        say("gave out IP " IPSTR, IP2STR(&e->ip));
    }
}

static void start_mdns(void)
{
#ifdef HAVE_MDNS
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(WIFI_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set("Drone-in-a-Box dock"));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
    say("mDNS responding as %s.local", WIFI_HOSTNAME);
#else
    say("mDNS unavailable in this build -- use the IP address");
#endif
}

void wifi_link_start(void)
{
    // IDF's own chatter duplicates these lines and buries them.
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    wifi_config_t ap = {
        .ap = {
            .ssid           = WIFI_AP_SSID,
            .ssid_len       = strlen(WIFI_AP_SSID),
            .channel        = WIFI_AP_CHANNEL,
            .password       = WIFI_AP_PASSWORD,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        },
    };
    if (strlen(WIFI_AP_PASSWORD) < 8) {
        ap.ap.authmode = WIFI_AUTH_OPEN;    // WPA2 needs 8 characters
    }

    // APSTA, always. The AP is the guarantee that the dock stays reachable.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    const esp_timer_create_args_t args = { .callback = retry_now, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_retry_timer));

    const bool has_station = configure_station();
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_sta_netif, WIFI_HOSTNAME));

    if (!has_station) {
        say("no stored network -- AP only. Provision with 'W' on the console.");
    }
    start_mdns();
}

void wifi_link_reprovision(void)
{
    s_retry_ms = WIFI_RETRY_MIN_MS;
    esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();

    if (configure_station()) {
        esp_wifi_connect();
    } else {
        say("credentials cleared -- AP only");
    }
}

bool        wifi_link_station_joined(void) { return s_joined; }
const char *wifi_link_station_ip(void)     { return s_sta_ip; }

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------
const char *wifi_link_auth_name(uint8_t authmode)
{
    switch ((wifi_auth_mode_t)authmode) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI";
    default:                        return "secured";
    }
}

// Insert one scan record into `out`, which is kept sorted by RSSI descending
// and holds at most one entry per SSID. Returns the new entry count.
//
// Deduplicating matters more than it looks: a mesh or an extender answers on
// several channels and bands, so one network routinely appears four or five
// times. Without this, a 16-entry list could show a single SSID and hide every
// other network from the operator.
static size_t insert_ap(wifi_link_ap_t *out, size_t count, size_t cap,
                        const wifi_ap_record_t *rec)
{
    const char *ssid = (const char *)rec->ssid;

    for (size_t i = 0; i < count; i++) {
        if (strncmp(out[i].ssid, ssid, WIFI_SSID_TEXT_MAX - 1) != 0) {
            continue;
        }
        if (rec->rssi > out[i].rssi) {
            // Same network on a better radio: take the stronger reading, and
            // its auth mode with it, since that is the one we would join.
            out[i].rssi     = rec->rssi;
            out[i].open     = rec->authmode == WIFI_AUTH_OPEN;
            out[i].authmode = (uint8_t)rec->authmode;

            // It may now outrank the entries above it.
            while (i > 0 && out[i].rssi > out[i - 1].rssi) {
                const wifi_link_ap_t tmp = out[i - 1];
                out[i - 1] = out[i];
                out[i]     = tmp;
                i--;
            }
        }
        return count;                   // seen before either way
    }

    // A full list keeps the strongest: a network weaker than everything we
    // already hold is not worth evicting one of them for.
    if (count == cap && rec->rssi <= out[cap - 1].rssi) {
        return count;
    }

    size_t at = count < cap ? count : cap - 1;
    while (at > 0 && rec->rssi > out[at - 1].rssi) {
        out[at] = out[at - 1];
        at--;
    }
    memset(&out[at], 0, sizeof(out[at]));
    strlcpy(out[at].ssid, ssid, sizeof(out[at].ssid));
    out[at].rssi     = rec->rssi;
    out[at].open     = rec->authmode == WIFI_AUTH_OPEN;
    out[at].authmode = (uint8_t)rec->authmode;

    return count < cap ? count + 1 : cap;
}

esp_err_t wifi_link_scan(wifi_link_ap_t *out, size_t cap, size_t *found)
{
    if (out == NULL || cap == 0 || found == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *found = 0;

    if (!s_started || s_scanning) {
        return ESP_ERR_INVALID_STATE;
    }
    s_scanning = true;

    // Dwell times are pinned rather than left to the default, for the sake of
    // our own SoftAP clients. home_chan_dwell_time parks the radio back on
    // WIFI_AP_CHANNEL between consecutive probes, so the browser driving this
    // scan gets a window to be served in instead of one unbroken blackout.
    // IDF allows 30-150 ms and defaults to 30; 60 buys a usable gap for the
    // price of roughly a second of extra scan, which is the right way round
    // when the alternative is the operator's page appearing to hang.
    //
    // show_hidden stays off: a hidden AP yields an empty SSID, which is
    // nothing the operator could pick out of a list.
    const wifi_scan_config_t cfg = {
        .show_hidden          = false,
        .scan_type            = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time            = { .active = { .min = 60, .max = 100 } },
        .home_chan_dwell_time = 60,
    };

    esp_err_t err = esp_wifi_scan_start(&cfg, true);   // blocks until done
    if (err != ESP_OK) {
        say("scan failed to start (%s)", esp_err_to_name(err));
        esp_wifi_clear_ap_list();
        s_scanning = false;
        return err;
    }

    // Fetch more records than we return, so the dedup above has duplicates to
    // collapse before the cap bites. On the stack this would be ~2.5 kB, and
    // the HTTP task's stack is 4 kB, so it goes on the heap.
    //
    // The driver hands them back sorted by RSSI already, but insert_ap() sorts
    // anyway: dedup has to reorder as it merges, so the ordering has to be
    // maintained here rather than inherited.
    uint16_t want = (uint16_t)(cap * 3 > 48 ? 48 : cap * 3);
    wifi_ap_record_t *recs = calloc(want, sizeof(*recs));
    if (recs == NULL) {
        esp_wifi_clear_ap_list();       // or the driver holds it until the next scan
        s_scanning = false;
        return ESP_ERR_NO_MEM;
    }

    // Frees the driver's list, including any records beyond `want`.
    err = esp_wifi_scan_get_ap_records(&want, recs);
    if (err != ESP_OK) {
        free(recs);
        esp_wifi_clear_ap_list();
        s_scanning = false;
        return err;
    }

    size_t count = 0;
    for (uint16_t i = 0; i < want; i++) {
        if (recs[i].ssid[0] == '\0') {
            continue;                   // hidden, or a malformed beacon
        }
        count = insert_ap(out, count, cap, &recs[i]);
    }

    free(recs);
    *found     = count;
    s_scanning = false;
    say("scan found %u network%s (%u beacon%s)",
        (unsigned)count, count == 1 ? "" : "s",
        (unsigned)want, want == 1 ? "" : "s");
    return ESP_OK;
}
