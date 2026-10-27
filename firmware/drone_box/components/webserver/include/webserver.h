// HTTP control API.
//
// Reachable on whatever interfaces `wifi_link` has up: always the dock's own
// access point at 192.168.4.1, and also a joined network, where the app finds
// it as dock.local. Every API request must be signed; see `auth`.
//
// Endpoints:
//   GET  /            the control page
//   GET  /api/status  current state as JSON
//   POST /api/cmd?c=X apply one command byte, returns the new state
#pragma once

// Wi-Fi itself lives in `wifi_link` -- the AP credentials and station logic
// are there, because this component only serves HTTP over whatever interfaces
// happen to be up.

// The dead-man switch lives in `session` now, because BLE needs exactly the
// same guarantee. See SESSION_TIMEOUT_MS in session.h.

void webserver_start(void);
