// The ZK-5AD / actuator driver layer: owns the two PWM pins and nothing else.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void actuator_init(void);

// direction: 'e' extend, 'r' retract, 's' stop (coast). speed_pct is clamped
// to the stall floor by the command layer's speed_clamp().
void actuator_set(char direction, int speed_pct);

// Diagnostic: hold one input at full scale and the other low, with no PWM at
// all. 'A' and 'B' are the two actuator pins set in board_config.h; use
// actuator_pin_name() to print which GPIO that is. Splits a PWM-path fault from a
// pin, wire or driver-module fault in one keypress.
void actuator_hold_raw(char which);

// Slow decay brakes during the off-phase; fast decay coasts. See README.
void actuator_set_slow_decay(bool slow);
bool actuator_slow_decay(void);

char     actuator_direction(void);
int      actuator_speed_pct(void);
uint32_t actuator_duty_a(void);
uint32_t actuator_duty_b(void);
uint32_t actuator_pwm_freq(void);
const char *actuator_pin_name(char which);
