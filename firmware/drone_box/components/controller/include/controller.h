// One command path for every client.
//
// The serial console and the web UI both send the same single-character
// commands and both land here, so there is exactly one place where a command
// changes the machine's state. Adding a client does not add a second
// interpretation of what 'e' means.
//
// Nothing here prints: handlers return their reply as text, and the caller
// decides whether it goes to a serial port or into an HTTP response.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define CONTROLLER_REPLY_MAX  96

void controller_init(void);

// Apply one command byte. Writes a human-readable reply into `reply`, which
// is always NUL-terminated. Returns false for bytes that mean nothing (CR, LF,
// unrecognised), in which case `reply` is empty.
bool controller_handle(uint8_t c, char *reply, size_t reply_len);

// Current state as JSON, for the web UI's status poll.
void controller_status_json(char *out, size_t out_len);

// Stop the actuator now and invalidate dead reckoning. Used by the web client's
// heartbeat watchdog when a browser stops polling mid-move.
void controller_emergency_stop(const char *why);

// Print the device key over the serial console so a bench client can sign
// requests before BLE pairing exists (spec T14 replaces this).
//
// This needs physical USB access, which the spec explicitly places out of
// scope -- an attacker at the board can reflash it anyway. It is still a
// deliberate hole: it is triggered only by an explicit command and it says so
// loudly in the log.
void controller_reveal_key(void);
