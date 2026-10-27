// BLE pairing and provisioning over NimBLE. Implements spec FR5, FR6, FR10.
//
// Two jobs in this phase:
//   * hand the app the device key, once, through a deliberately opened window
//   * accept Wi-Fi credentials over an encrypted link
//
// Control over BLE (command + status characteristics) is task T16.
//
// Why BLE for this and not Wi-Fi: handing over credentials for a network the
// dock has not joined yet is a chicken-and-egg problem. BLE needs no network,
// and its link encryption means the Wi-Fi password never crosses the air in
// the clear.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

// How long the pairing window stays open. Short on purpose: while it is open,
// anyone in range who bonds can read the device key. It is a deliberate hole,
// opened by a physical act at the console, and it closes on first read.
//
// Edit the value in board_config.h: `secrets` reads the same one, and the two
// must agree.
#define BLE_PAIRING_WINDOW_S  BOARD_BLE_PAIRING_WINDOW_S

void ble_link_start(void);

// Open the window. Triggered by 'P' on the serial console -- never remotely,
// or an attacker could open their own route in.
void ble_link_open_pairing(uint32_t seconds);
void ble_link_close_pairing(const char *why);
bool ble_link_pairing_open(void);
