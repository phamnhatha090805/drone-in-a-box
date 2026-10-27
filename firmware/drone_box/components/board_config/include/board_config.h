// Every pin, radio name and tunable timing for this board, in one place.
//
// Change the dock's wiring, its Wi-Fi identity, its BLE identity or the
// mechanical numbers here and nowhere else. The components below own the
// behaviour; this header owns the values they run on.
//
// Two rules keep it that way:
//
//   1. Plain integers and string literals only -- no #include, and no ESP-IDF
//      enums such as GPIO_NUM_4 or LEDC_TIMER_0. That is what lets `command`
//      and `auth` include it and still build with the host compiler in
//      test/host, with no board and no IDF. Driver-enum choices stay in the
//      .c file that makes them.
//   2. Nothing derived from a measurement of *software* belongs here. Buffer
//      sizes, canonical-form limits and protocol constants live with the code
//      that has to agree with the clients about them.
//
// Anything a second unit built from this repository would need changed is a
// candidate for this file. Anything that would break the wire format is not.
#pragma once

// ===========================================================================
// Actuator: ZK-5AD driver outputs
// ===========================================================================
// Wiring: BOARD_ACTUATOR_PIN_A -> driver D2, BOARD_ACTUATOR_PIN_B -> driver D3,
// ESP32 GND -> driver GND. Both pins are driven by LEDC; which direction is
// "extend" is decided by which one carries the duty, in actuator.c.
//
// TODO: GPIO4/5 are ADC1_CH3/CH4. Wi-Fi owns ADC2 on the S3, so ADC1 is the
// only ADC left for actuator position feedback -- move these to GPIO38+ once
// the potentiometer goes in, and re-check them against the DevKitC pinout.
#define BOARD_ACTUATOR_PIN_A       4
#define BOARD_ACTUATOR_PIN_B       5

// Above hearing, so the actuator does not whine. Raising this past ~40 kHz
// starts to cost duty resolution at BOARD_ACTUATOR_PWM_BITS.
#define BOARD_ACTUATOR_PWM_FREQ_HZ 20000

// LEDC duty resolution. Must stay equal to DUTY_BITS in command.h, which does
// the duty arithmetic on the host where LEDC's enums do not exist. Changing
// one without the other silently rescales every speed.
#define BOARD_ACTUATOR_PWM_BITS    8

// ===========================================================================
// Pairing button
// ===========================================================================
// GPIO0 is the BOOT button on the ESP32-S3-DevKitC. It is a strapping pin, so
// it is only read after boot has finished -- holding it at reset puts the chip
// in download mode, which is unrelated and harmless here.
//
// Pairing is gated on this press because it is the one thing an attacker in
// radio range cannot do. See pair_button.h.
#define BOARD_PAIR_BUTTON_GPIO      0
#define BOARD_PAIR_BUTTON_DEBOUNCE_MS  250

// ===========================================================================
// Wi-Fi: the dock's own access point
// ===========================================================================
// This AP is always up, on every unit. Local control must never depend on
// infrastructure -- a field has no SSID to join -- so station mode is strictly
// additive and never affects these.
#define BOARD_WIFI_AP_SSID      "drone-in-a-box"

// SECURITY: a compile-time constant, shared by every unit built from this
// repository, and public in git. It keeps casual neighbours off the AP and
// proves nothing about who is connected -- which is why every control request
// is signed with the per-device key, and why pairing is gated on the BOOT
// button rather than on AP membership. Spec acceptance criterion 5 is still
// open against this value; see specs/001-secure-control-interface/spec.md.
//
// Must be >= 8 characters, or the AP comes up open.
#define BOARD_WIFI_AP_PASSWORD  "liftmotor"

#define BOARD_WIFI_AP_CHANNEL   1
#define BOARD_WIFI_AP_MAX_CONN  4

// Advertised over mDNS, so the app can reach the dock as dock.local. Also the
// stem of the BLE advertised name below.
#define BOARD_WIFI_HOSTNAME     "dock"

// ===========================================================================
// Wi-Fi: joining an existing network (station mode)
// ===========================================================================
// There is deliberately no SSID or password here. Station credentials are
// provisioned at runtime -- over the serial console, or over an encrypted BLE
// link -- and kept in NVS by `secrets`, so a Wi-Fi password never enters this
// repository and differs per unit. See secrets_set_wifi().
//
// Reconnect backoff: fast at first so a brief glitch recovers quickly, then
// slow, so a hotspot that is simply switched off does not cost power forever.
#define BOARD_WIFI_RETRY_MIN_MS 1000
#define BOARD_WIFI_RETRY_MAX_MS 30000

// ===========================================================================
// BLE
// ===========================================================================
// The advertised name is BOARD_BLE_NAME_PREFIX plus the last two bytes of the
// MAC, e.g. "dock-3f8a", so two docks on a bench are tellable apart without
// configuring either. Keep the prefix short: the name shares the 31-byte
// advertising payload with the 128-bit service UUID.
#define BOARD_BLE_NAME_PREFIX   BOARD_WIFI_HOSTNAME

// How long the pairing window stays open. Short on purpose: while it is open,
// anyone in range who bonds can read the device key. It is a deliberate hole,
// opened by a physical act at the console, and it closes on first read.
//
// One value, two readers: `ble_link` advertises connectably for this long and
// `secrets` will not hand the key out past it. They must agree, or the window
// either shuts early or stays open with nothing listening.
#define BOARD_BLE_PAIRING_WINDOW_S  60

// Base of the five custom 128-bit UUIDs (service, pairing, provision,
// command, status), which differ only in their first byte. Change this and
// every paired app stops finding the dock, so change it only to fork the
// protocol deliberately. The byte order NimBLE wants is handled in ble_link.c.
#define BOARD_BLE_UUID_BASE                                                   \
    0x5e, 0x4d, 0x3c, 0x2b, 0x1a, 0x3f, 0x8d,                                 \
    0x7c, 0x4b, 0x1e, 0x9a, 0x20, 0x8c, 0x6d, 0x5f

// ===========================================================================
// Mechanics: stroke, levels, speed
// ===========================================================================
// MEASURE BOARD_TRAVEL_MS ON YOUR ACTUATOR before the level commands mean
// anything: press 'h' to home, then time 't' with a stopwatch. The value here
// is a placeholder and every level depends on it.
//
// There is no position feedback on this rig, so level is dead-reckoned from
// how long the actuator has run. That is only valid when every timed move runs
// BOARD_STEP_SPEED_PCT from a known datum, which homing establishes.
#define BOARD_TRAVEL_MS         10000
#define BOARD_LEVELS            5
#define BOARD_STEP_SPEED_PCT    100

// Overtravel into the end stop when homing, as a percentage of a full stroke.
// 130 means "run 30% longer than the stroke", which absorbs an unknown
// starting position and any error in BOARD_TRAVEL_MS.
#define BOARD_HOME_OVERTRAVEL_PCT  130

// Below this the actuator stalls and buzzes instead of moving, so the command
// layer clamps every requested speed up to it. Measure it the same way: jog
// down until the motion stops being reliable.
#define BOARD_SPEED_MIN_PCT     50

// ===========================================================================
// Safety: the dead-man switch
// ===========================================================================
// A remote client must refresh its hold inside this window or the actuator stops.
// It bounds how far the actuator travels after a dropped Wi-Fi link, a closed
// laptop or a crashed app, so it is a mechanical-clearance number, not a
// networking one: raise it only if you have the travel to spare.
//
// Must stay well above the slowest client's refresh interval, and above
// BOARD_WIFI_SCAN_BUDGET_MS is not possible -- which is why wifi_link.h warns
// never to scan while the actuator is energised.
#define BOARD_SESSION_TIMEOUT_MS  1500
