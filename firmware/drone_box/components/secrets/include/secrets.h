// Persistent device identity: the signing key, the replay high-water mark and
// Wi-Fi credentials. Implements spec FR4, FR5, FR11.
//
// The device key is generated on the device, on first boot, and never appears
// in source or in git. It leaves only through the BLE pairing window.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "auth.h"
#include "board_config.h"
#include "esp_err.h"

#define SECRETS_SSID_MAX  33            // 32 + NUL
#define SECRETS_PASS_MAX  65            // 64 + NUL

// Opens NVS and generates the device key if this is the first boot.
esp_err_t secrets_init(void);

const uint8_t *secrets_device_key(void);

// True when the key was created during this boot, i.e. the dock has never been
// paired. Worth saying loudly on the console.
bool secrets_key_is_new(void);

// Replaces the key with a fresh one. Every previously paired app stops working.
esp_err_t secrets_rotate_key(void);

// --- replay high-water mark -------------------------------------------------
//
// Writing NVS on every request would wear out the sector within days, so the
// counter lives in RAM and is persisted every SECRETS_NONCE_STRIDE increments.
// On boot we resume at (stored + stride), because up to a stride's worth of
// nonces may have been accepted since the last write. Skipping a few unused
// nonces costs nothing; accepting one twice would defeat replay protection.
#define SECRETS_NONCE_STRIDE  256

uint64_t secrets_nonce_high_water(void);
void     secrets_note_nonce(uint64_t nonce);    // call only after auth_verify succeeds

// --- Wi-Fi credentials ------------------------------------------------------
esp_err_t secrets_set_wifi(const char *ssid, const char *password);
bool      secrets_get_wifi(char *ssid, size_t ssid_len,
                           char *password, size_t password_len);
esp_err_t secrets_clear_wifi(void);

// --- pairing window ---------------------------------------------------------
//
// The only time the device key leaves the dock. Opened by a deliberate
// physical act -- the BOOT button -- and never remotely: the Wi-Fi password is
// a compile-time constant in this repository, so "is on our AP" is not proof
// of anything. A button press is.
//
// Closes on the first successful handover, or when the window expires.
//
// The same window `ble_link` advertises for -- one value in board_config.h,
// because a window that shuts early or outlives its advertiser is a bug in
// either direction.
#define PAIRING_WINDOW_S  BOARD_BLE_PAIRING_WINDOW_S

void secrets_open_pairing(uint32_t seconds);
void secrets_close_pairing(const char *why);
bool secrets_pairing_open(void);

// Called by whichever transport handed the key over, so the window shuts
// immediately rather than staying open for the rest of its minute.
void secrets_note_key_handover(const char *how);

// HMAC-SHA256, shaped for auth_init(). Lives here because this component
// already owns the key and links mbedTLS.
void secrets_hmac_sha256(const uint8_t key[AUTH_KEY_LEN],
                         const uint8_t *msg, size_t msg_len,
                         uint8_t out[AUTH_SIG_LEN]);
