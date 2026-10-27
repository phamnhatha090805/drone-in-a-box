// Dock lift firmware: one linear actuator on ZK-5AD channel B (motor 2), ESP32-S3.
// Wiring: GPIO4 -> D2, GPIO5 -> D3, ESP32 GND -> driver GND.
//
// Two clients, one command set:
//   serial console at 115200
//   Wi-Fi: join the AP "drone-in-a-box", open http://192.168.4.1/
//
//   e = extend, r = retract, s = stop
//   5..9 = speed 50%..90%, 0 = full speed (100%); 1..4 clamp to 50%
//   h = home retracted (level 0), t = top extended (level 5)
//   +/- = move one level and stop, p = show level
//   d = toggle slow/fast decay, A/B = hold one pin high (diagnostic)
//
// This file is wiring only. The work lives in the components:
//   board_config/ every pin, radio name and tunable timing. Header only.
//   command/     byte -> command, duty and clamp arithmetic.
//                No ESP-IDF headers: unit tested on the host.
//   actuator/    the two PWM pins, LEDC, decay modes.
//   positioner/  dead-reckoned levels, non-blocking timed moves.
//   controller/  the single command path both clients share.
//   auth/        request signing and replay window. No ESP-IDF headers.
//   secrets/     device key, replay high-water mark, Wi-Fi credentials in NVS.
//   session/     the dead-man switch, shared by every remote transport.
//   serial_io/   UART0 or native USB, whichever menuconfig selects.
//   wifi_link/   APSTA: own AP always up, joins a stored network too, mDNS.
//   ble_link/    NimBLE: pairing and Wi-Fi provisioning.
//   pair_button/ BOOT button: the physical gate on handing out the key.
//   webserver/   signed REST endpoints and the diagnostic page.
//
// See README.md. BOARD_TRAVEL_MS in board_config.h must be measured on your
// actuator before the level commands mean anything.

#include <stdio.h>

#include "esp_err.h"

#include "auth.h"
#include "ble_link.h"
#include "command.h"
#include "controller.h"
#include "positioner.h"
#include "actuator.h"
#include "pair_button.h"
#include "secrets.h"
#include "serial_io.h"
#include "session.h"
#include "webserver.h"
#include "wifi_link.h"

// Serial-only: reads an SSID and password and rejoins without a reboot.
// Implements task T11; BLE provisioning (T15) replaces it as the user-facing
// path, but this stays for service.
static void provision_wifi(void)
{
    char ssid[SECRETS_SSID_MAX], pass[SECRETS_PASS_MAX];

    printf("\nSSID (blank to cancel): ");
    fflush(stdout);
    if (serial_io_read_line(ssid, sizeof(ssid), false) <= 0) {
        printf("cancelled\n");
        return;
    }

    printf("Password (blank for an open network): ");
    fflush(stdout);
    if (serial_io_read_line(pass, sizeof(pass), true) < 0) {
        printf("cancelled\n");
        return;
    }

    if (secrets_set_wifi(ssid, pass) == ESP_OK) {
        printf("stored -- joining \"%s\"\n", ssid);
        wifi_link_reprovision();
    } else {
        printf("could not store credentials\n");
    }
    fflush(stdout);
}

static void banner(void)
{
    printf("Ready: e/r/s, speed 5-9 (50-90%%), 0 = 100%%; 1-4 clamp to %d%%\n",
           SPEED_MIN_PCT);
    printf("Diag:  A = hold GPIO4 high (no PWM), B = hold GPIO5 high\n");
    printf("Decay: slow (brake off-phase). Press d to toggle fast/slow.\n");
    printf("Level: h = home retracted, t = top extended, +/- = one step, p = show\n");
    printf("       %d steps of %d ms over a %d ms stroke\n", LEVELS, STEP_MS, TRAVEL_MS);
    printf("PWM:   %lu Hz on %s\n",
           (unsigned long)actuator_pwm_freq(), serial_io_transport_name());
    printf("Web:   join \"%s\" then open http://192.168.4.1/\n", WIFI_AP_SSID);
    printf("Net:   W = set wifi, X = forget wifi, K = reveal device key\n");
    printf("Pair:  P = open the BLE pairing window for %d s\n", BLE_PAIRING_WINDOW_S);
    printf("Auth:  API requires X-Dock-Nonce + X-Dock-Sig%s\n",
           secrets_key_is_new() ? " -- NEW KEY, dock has never been paired" : "");
    fflush(stdout);
}

void app_main(void)
{
    controller_init();                      // actuator off before anything can command it

    // Identity first: NVS, the device key, and the replay high-water mark. The
    // HTTP gate and the pairing flow are both meaningless without it.
    ESP_ERROR_CHECK(secrets_init());
    auth_init(secrets_hmac_sha256);

    session_init();                         // dead-man, armed by any remote link
    serial_io_init();
    wifi_link_start();                      // AP always up; joins a stored net too
    webserver_start();
    ble_link_start();                       // pairing + provisioning over BLE
    pair_button_start();                    // BOOT button opens the pairing window
    banner();

    for (;;) {
        uint8_t c;
        char reply[CONTROLLER_REPLY_MAX];

        // Blocks until the RX interrupt delivers a byte. No polling, no delay.
        if (!serial_io_read_byte(&c)) {
            continue;
        }

        // Provisioning is interactive and serial-only, so it is handled here
        // rather than in the shared command path. The parse is still shared.
        const command_t cmd = command_parse(c);
        if (cmd.kind == CMD_PROVISION) {
            provision_wifi();
            continue;
        }
        if (cmd.kind == CMD_PAIR) {
            ble_link_open_pairing(BLE_PAIRING_WINDOW_S);
            continue;
        }
        if (cmd.kind == CMD_FORGET_WIFI) {
            secrets_clear_wifi();
            wifi_link_reprovision();
            printf("wifi credentials cleared -- AP only\n");
            fflush(stdout);
            continue;
        }

        if (controller_handle(c, reply, sizeof(reply))) {
            printf("%s\n", reply);
            fflush(stdout);
        }
    }
}
