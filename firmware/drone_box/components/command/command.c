#include "command.h"

command_t command_parse(uint8_t c)
{
    switch (c) {
    case 'e': case 'r': case 's':
        return (command_t){ CMD_DIRECTION, (int)c };
    case 'h': case 't':
        return (command_t){ CMD_LEVEL_END, (int)c };
    case 'A': case 'B':
        return (command_t){ CMD_RAW_HOLD, (int)c };
    case 'c': case 'w': case 'f':
        // Clockwise, counter-clockWise, oFf.
        //
        // Not 'o' for on: on a console 'o' and '0' are one keystroke apart and
        // '0' means full speed, which is the last thing a mistyped stepper
        // command should do. Not 'n'/'m' for the two directions either -- they
        // are keyboard neighbours, and the typo that reverses a running motor
        // is the one worth designing out. 'c' and 'w' are three keys apart and
        // read the way CW/CCW does.
        return (command_t){ CMD_STEPPER, (int)c };
    case '+':
        return (command_t){ CMD_LEVEL_STEP, +1 };
    case '-':
        return (command_t){ CMD_LEVEL_STEP, -1 };
    case 'p':
        return (command_t){ CMD_LEVEL_SHOW, 0 };
    case 'd':
        return (command_t){ CMD_DECAY_TOGGLE, 0 };
    case 'K':
        return (command_t){ CMD_REVEAL_KEY, 0 };
    case 'W':
        return (command_t){ CMD_PROVISION, 0 };
    case 'X':
        return (command_t){ CMD_FORGET_WIFI, 0 };
    case 'P':
        return (command_t){ CMD_PAIR, 0 };
    default:
        break;
    }

    if (c >= '0' && c <= '9') {
        // '0' means full speed, not zero -- same as the Arduino sketch.
        return (command_t){ CMD_SPEED, (c == '0') ? 100 : (c - '0') * 10 };
    }
    return (command_t){ CMD_IGNORE, 0 };
}

bool command_energises_actuator(command_t cmd)
{
    switch (cmd.kind) {
    case CMD_DIRECTION:
        return cmd.value != 's';            // stopping is always allowed
    case CMD_LEVEL_END:
    case CMD_LEVEL_STEP:
    case CMD_RAW_HOLD:                      // holds one input at full scale
        return true;
    default:
        // CMD_SPEED only writes a duty the current direction already implies,
        // so at a standstill it drives nothing -- and the stepper can only be
        // running if the actuator was already stopped.
        return false;
    }
}

bool command_energises_stepper(command_t cmd)
{
    // Either direction energises it; 'f' is the only CMD_STEPPER that does not.
    return cmd.kind == CMD_STEPPER && (cmd.value == 'c' || cmd.value == 'w');
}

int speed_clamp(int pct)
{
    if (pct > 100)            return 100;
    if (pct < SPEED_MIN_PCT)  return SPEED_MIN_PCT;
    return pct;
}

bool speed_is_clamped(int pct)
{
    return pct < SPEED_MIN_PCT || pct > 100;
}

uint32_t speed_to_duty(int pct)
{
    const uint32_t d = (uint32_t)speed_clamp(pct) * (DUTY_FULL_SCALE - 1) / 100;
    return (d >= DUTY_FULL_SCALE - 1) ? DUTY_FULL_SCALE : d;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

size_t command_url_decode(const char *in, char *out, size_t out_len)
{
    size_t w = 0;

    if (out_len == 0) return 0;

    for (size_t r = 0; in[r] != '\0' && w + 1 < out_len; r++) {
        if (in[r] == '%') {
            const int hi = hex_value(in[r + 1]);
            // Only read the second digit once the first proved valid, so a
            // trailing "%" or "%A" cannot walk off the end of the string.
            const int lo = (hi >= 0) ? hex_value(in[r + 2]) : -1;
            if (lo >= 0) {
                out[w++] = (char)((hi << 4) | lo);
                r += 2;
                continue;
            }
            out[w++] = in[r];               // malformed escape: pass it through
        } else if (in[r] == '+') {
            out[w++] = ' ';                 // form encoding: '+' means space
        } else {
            out[w++] = in[r];
        }
    }

    out[w] = '\0';
    return w;
}

bool level_step_valid(int level, int step, int levels)
{
    const int next = level + step;
    return next >= 0 && next <= levels;
}
