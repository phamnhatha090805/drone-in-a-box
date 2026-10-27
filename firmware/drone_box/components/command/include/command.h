// Command parsing and the arithmetic behind it.
//
// This component contains NO hardware access and NO ESP-IDF headers on
// purpose: it is plain C17 and can be compiled and unit tested on a host with
// nothing but a C compiler. Everything that can be got wrong by reasoning --
// the duty full-scale correction, the stall-floor clamp, level bounds -- lives
// here rather than in the driver layer.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// board_config.h is deliberately include-free and ESP-IDF-free, so taking the
// tunables from it costs this component nothing: it still builds on the host.
#include "board_config.h"

// Below this the actuator stalls under load instead of moving, and commanding
// a stall is how half-bridges die. Slower speed keys clamp up to it.
#define SPEED_MIN_PCT    BOARD_SPEED_MIN_PCT

// LEDC duty at 8-bit resolution spans 0..256, NOT 0..255. 255 is 99.6% -- the
// pin dips low for ~195 ns every period, which the ZK-5AD's forward input
// ignores entirely. Only 256 holds the pin solidly high. Arduino's ledcWrite()
// applies the same correction internally, which is why the original sketch
// worked and a literal port did not.
#define DUTY_BITS        BOARD_ACTUATOR_PWM_BITS
#define DUTY_FULL_SCALE  (1u << DUTY_BITS)

typedef enum {
    CMD_IGNORE,         // unrecognised byte, CR, LF
    CMD_DIRECTION,      // value: 'e', 'r' or 's'
    CMD_SPEED,          // value: requested percent, before clamping
    CMD_LEVEL_STEP,     // value: +1 or -1
    CMD_LEVEL_END,      // value: 'h' (retracted) or 't' (extended)
    CMD_LEVEL_SHOW,
    CMD_DECAY_TOGGLE,
    CMD_RAW_HOLD,       // value: 'A' or 'B'
    CMD_REVEAL_KEY,     // 'K' -- bench affordance, see controller_reveal_key()
    CMD_PROVISION,      // 'W' -- interactive, serial only
    CMD_FORGET_WIFI,    // 'X'
    CMD_PAIR,           // 'P' -- opens the BLE pairing window, serial only
} cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    int        value;
} command_t;

// One input byte to one command. Never fails; unknown bytes yield CMD_IGNORE.
command_t command_parse(uint8_t c);

// Raise a speed below the stall floor up to it. Returns the usable percent.
int speed_clamp(int pct);

// True when speed_clamp() would change the value, so the caller can say so.
bool speed_is_clamped(int pct);

// Speed percent to LEDC duty, including the full-scale correction above.
uint32_t speed_to_duty(int pct);

// Would level + step stay within 0..levels?
bool level_step_valid(int level, int step, int levels);

// Percent-decode a query-string value in place of copying it out raw.
//
// ESP-IDF's httpd_query_key_value() hands back the value still encoded, so
// "?c=%2B" yields the three characters "%2B" rather than '+'. Any command that
// is not an unreserved URL character therefore arrives mangled -- which is why
// every button worked except Step +.
//
// Decodes %XX escapes and '+' as space, per application/x-www-form-urlencoded.
// Writes at most out_len-1 characters plus a NUL. Returns the decoded length.
size_t command_url_decode(const char *in, char *out, size_t out_len);
