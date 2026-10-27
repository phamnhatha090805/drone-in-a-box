// Console I/O. Which transport is compiled in follows menuconfig:
// Component config -> ESP System Settings -> Channel for console output.
// Both paths are interrupt-driven and block in the scheduler; neither polls.
//
// Named serial_io rather than "console" because ESP-IDF already ships a
// component called console (esp_console.h) and component names share one
// global namespace -- shadowing it breaks unrelated IDF components.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void serial_io_init(void);

// Blocks until the RX interrupt delivers one byte. Returns false only on a
// driver-level read failure.
bool serial_io_read_byte(uint8_t *c);

// "UART0" or "USB-Serial-JTAG", for the boot banner.
const char *serial_io_transport_name(void);

// Read one CR/LF-terminated line, blocking. Echoes unless `secret` is set, in
// which case asterisks are echoed so a Wi-Fi password never lands in the
// console scrollback. Returns the length, or -1 on read failure.
int serial_io_read_line(char *buf, size_t buf_len, bool secret);
