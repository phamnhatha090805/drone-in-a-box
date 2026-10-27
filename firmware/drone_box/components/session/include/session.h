// The dead-man switch, generalised across transports. Implements spec FR8.
//
// Remote control of something that moves needs a client-liveness check: a
// browser tab gets closed mid-cycle, an app is killed, a phone walks out of
// range. The firmware is authoritative and never waits for a client to send
// the stop -- it notices the silence and stops the actuator itself.
//
// This lived inside `webserver` while HTTP was the only remote transport.
// BLE needs exactly the same guarantee, so it moved here before BLE arrives
// rather than being duplicated afterwards.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

typedef enum {
    LINK_HTTP,
    LINK_BLE,
    LINK_COUNT,
} link_t;

// Serial is deliberately absent: a cable implies someone physically present at
// the bench, and a service technician must not have the actuator stopped out
// from under them by a timer.

#define SESSION_TIMEOUT_MS  BOARD_SESSION_TIMEOUT_MS

void session_init(void);

// Call on every *authorised* message. An unauthorised one must never pet the
// watchdog, or a stranger could hold it open after the operator has gone.
void session_touch(link_t link);

// True while at least one remote link has spoken within the timeout.
bool session_any_live(void);
