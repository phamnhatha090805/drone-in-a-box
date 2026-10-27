// The BOOT button as the pairing trigger.
//
// Pairing must be gated on something an attacker in radio range cannot do.
// Joining the dock's AP is not that: its password is a compile-time constant
// in this repository, shared by every unit built from it. Pressing a button on
// the board is.
//
// GPIO0 is the BOOT button on the ESP32-S3-DevKitC. It is a strapping pin, so
// it is only read after boot has finished -- holding it at reset puts the chip
// in download mode, which is unrelated and harmless here.
#pragma once

#include "board_config.h"

// Which pin, and how long to settle. Edit the values in board_config.h.
#define PAIR_BUTTON_GPIO         BOARD_PAIR_BUTTON_GPIO
#define PAIR_BUTTON_DEBOUNCE_MS  BOARD_PAIR_BUTTON_DEBOUNCE_MS

void pair_button_start(void);
