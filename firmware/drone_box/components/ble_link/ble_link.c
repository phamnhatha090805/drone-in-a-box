#include "ble_link.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "controller.h"
#include "secrets.h"
#include "board_config.h"
#include "session.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "wifi_link.h"

// Custom 128-bit UUIDs. NimBLE takes the bytes least-significant first, so
// these read backwards against the canonical string form:
//   service   5f6d8c20-9a1e-4b7c-8d3f-1a2b3c4d5e01
//   pairing   ...5e02      read,   only while the window is open
//   provision ...5e03      write,  signed
//   command   ...5e04      write,  signed
//   status    ...5e05      read + notify
// The 15 shared bytes are BOARD_BLE_UUID_BASE in board_config.h; only the
// leading byte, which selects the characteristic, is chosen here.
#define UUID128(last)  BLE_UUID128_INIT(last, BOARD_BLE_UUID_BASE)

static const ble_uuid128_t UUID_SERVICE   = UUID128(0x01);
static const ble_uuid128_t UUID_PAIRING   = UUID128(0x02);
static const ble_uuid128_t UUID_PROVISION = UUID128(0x03);
static const ble_uuid128_t UUID_COMMAND   = UUID128(0x04);
static const ble_uuid128_t UUID_STATUS    = UUID128(0x05);

// Signed write framing: 8-byte little-endian nonce, 32-byte signature, then
// the payload. Link encryption alone is not enough -- the dock pairs "just
// works", so any device in range can bond. Only a client that was handed the
// device key through the pairing window can produce the signature.
#define BLE_WRITE_HEADER  (8 + AUTH_SIG_LEN)

static uint8_t  s_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_status_handle;
static char    s_name[20];

static void say(const char *what)
{
    printf("BLE:   %s\n", what);
    fflush(stdout);
}

static void advertise(void);

// The pairing window lives in `secrets` now: both BLE and the web UI hand the
// key over, so neither transport can own it.
void ble_link_open_pairing(uint32_t seconds) { secrets_open_pairing(seconds); }
void ble_link_close_pairing(const char *why) { secrets_close_pairing(why); }
bool ble_link_pairing_open(void)             { return secrets_pairing_open(); }

// ---------------------------------------------------------------------------
// GATT
// ---------------------------------------------------------------------------
static int on_pairing_read(struct ble_gatt_access_ctxt *ctxt)
{
    if (!secrets_pairing_open()) {
        say("key read refused -- pairing window is closed");
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    if (os_mbuf_append(ctxt->om, secrets_device_key(), AUTH_KEY_LEN) != 0) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    // One-shot: the app only ever needs it once, and leaving the window open
    // after a successful handover widens the hole for no benefit.
    secrets_note_key_handover("BLE");
    return 0;
}

// Unwrap and verify a signed write. On success *payload/*payload_len point
// into `buf` at the bytes that were signed.
static int unwrap_signed(struct ble_gatt_access_ctxt *ctxt, const char *path,
                         uint8_t *buf, size_t buf_len,
                         const uint8_t **payload, size_t *payload_len)
{
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, buf_len, &len) != 0 ||
        len <= BLE_WRITE_HEADER) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint64_t nonce = 0;
    for (int i = 7; i >= 0; i--) {
        nonce = (nonce << 8) | buf[i];      // little-endian on the wire
    }
    const uint8_t *sig = buf + 8;
    *payload     = buf + BLE_WRITE_HEADER;
    *payload_len = len - BLE_WRITE_HEADER;

    if (!auth_verify_blob(secrets_device_key(), "BLE", path,
                          *payload, *payload_len, nonce, sig,
                          secrets_nonce_high_water())) {
        printf("BLE:   refused %s -- bad signature or replayed nonce\n", path);
        fflush(stdout);
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    secrets_note_nonce(nonce);
    session_touch(LINK_BLE);                // only an authorised write counts
    return 0;
}

static void notify_status(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_status_handle == 0) {
        return;
    }
    char json[320];
    controller_status_json(json, sizeof(json));
    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, strlen(json));
    if (om != NULL) {
        ble_gatts_notify_custom(s_conn, s_status_handle, om);
    }
}

static int on_command_write(struct ble_gatt_access_ctxt *ctxt)
{
    uint8_t buf[BLE_WRITE_HEADER + 8];
    const uint8_t *payload = NULL;
    size_t payload_len = 0;

    const int err = unwrap_signed(ctxt, "/ble/cmd", buf, sizeof(buf),
                                  &payload, &payload_len);
    if (err != 0) {
        return err;
    }
    if (payload_len != 1) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    char reply[CONTROLLER_REPLY_MAX];
    if (controller_handle(payload[0], reply, sizeof(reply))) {
        printf("BLE:   %c -> %s\n", payload[0], reply);
    } else {
        printf("BLE:   ignored byte 0x%02x\n", payload[0]);
    }
    fflush(stdout);
    notify_status();                        // the client sees the result at once
    return 0;
}

static int on_status_read(struct ble_gatt_access_ctxt *ctxt)
{
    char json[320];
    controller_status_json(json, sizeof(json));
    return os_mbuf_append(ctxt->om, json, strlen(json)) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int on_provision_write(struct ble_gatt_access_ctxt *ctxt)
{
    uint8_t raw[BLE_WRITE_HEADER + SECRETS_SSID_MAX + SECRETS_PASS_MAX];
    char buf[SECRETS_SSID_MAX + SECRETS_PASS_MAX + 1] = {0};
    const uint8_t *payload = NULL;
    size_t payload_len = 0;

    const int err = unwrap_signed(ctxt, "/ble/provision", raw, sizeof(raw),
                                  &payload, &payload_len);
    if (err != 0) {
        return err;
    }
    if (payload_len >= sizeof(buf)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    memcpy(buf, payload, payload_len);
    const uint16_t len = (uint16_t)payload_len;

    // Payload is "ssid\0password". The NUL keeps SSIDs containing any
    // character unambiguous, which a delimiter like ':' would not.
    const char *ssid = buf;
    const size_t ssid_len = strnlen(ssid, len);
    if (ssid_len == 0 || ssid_len >= SECRETS_SSID_MAX || ssid_len == (size_t)len) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    const char *pass = buf + ssid_len + 1;

    if (secrets_set_wifi(ssid, pass) != ESP_OK) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    printf("BLE:   provisioned \"%s\" -- joining\n", ssid);   // never the password
    fflush(stdout);
    wifi_link_reprovision();
    return 0;
}

static int gatt_access(uint16_t conn, uint16_t attr,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;

    if (ble_uuid_cmp(ctxt->chr->uuid, &UUID_PAIRING.u) == 0) {
        return on_pairing_read(ctxt);
    }
    if (ble_uuid_cmp(ctxt->chr->uuid, &UUID_PROVISION.u) == 0) {
        return on_provision_write(ctxt);
    }
    if (ble_uuid_cmp(ctxt->chr->uuid, &UUID_COMMAND.u) == 0) {
        return on_command_write(ctxt);
    }
    if (ble_uuid_cmp(ctxt->chr->uuid, &UUID_STATUS.u) == 0) {
        return on_status_read(ctxt);
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def GATT_SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SERVICE.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid      = &UUID_PAIRING.u,
                .access_cb = gatt_access,
                // Encryption required: the key must not cross the air in clear.
                .flags     = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            },
            {
                .uuid      = &UUID_PROVISION.u,
                .access_cb = gatt_access,
                // Encrypted AND signed: encryption hides the password, the
                // signature proves it came from the paired app.
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid      = &UUID_COMMAND.u,
                .access_cb = gatt_access,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid      = &UUID_STATUS.u,
                .access_cb = gatt_access,
                .val_handle = &s_status_handle,
                .flags     = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                             BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

// ---------------------------------------------------------------------------
// GAP
// ---------------------------------------------------------------------------
static int on_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        say(event->connect.status == 0 ? "connected" : "connection failed");
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        say("disconnected");
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        // The dead-man is not disarmed here on purpose: session/ notices the
        // silence and stops the actuator, exactly as it does for Wi-Fi.
        advertise();                        // always be findable again
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        say(event->enc_change.status == 0 ? "link encrypted" : "encryption failed");
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_name;
    fields.name_len = strlen(s_name);
    fields.name_is_complete = 1;
    fields.uuids128 = (ble_uuid128_t *)&UUID_SERVICE;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, on_gap_event, NULL);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_addr_type);

    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(s_addr_type, addr, NULL);
    snprintf(s_name, sizeof(s_name), BOARD_BLE_NAME_PREFIX "-%02x%02x",
             addr[1], addr[0]);
    ble_svc_gap_device_name_set(s_name);

    printf("BLE:   advertising as \"%s\" -- press P to open pairing\n", s_name);
    fflush(stdout);
    advertise();
}

static void on_reset(int reason)
{
    printf("BLE:   stack reset, reason %d\n", reason);
    fflush(stdout);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();                      // returns only on nimble_port_stop()
    nimble_port_freertos_deinit();
}

void ble_link_start(void)
{
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // Just-works pairing: the dock has no display or keypad, so there is no
    // way to confirm a passkey. The link is encrypted and bonded, but not
    // authenticated -- a determined attacker in range during the window could
    // man-in-the-middle it. That is why the window is short, operator-opened
    // and one-shot. Noted in the spec's non-goals.
    ble_hs_cfg.sm_io_cap     = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding    = 1;
    ble_hs_cfg.sm_sc         = 1;           // LE Secure Connections
    ble_hs_cfg.sm_our_key_dist  = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(GATT_SERVICES));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(GATT_SERVICES));

    nimble_port_freertos_init(host_task);
}
