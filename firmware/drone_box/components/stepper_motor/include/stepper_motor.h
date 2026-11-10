// The stepper driver layer: owns four GPIOs and nothing else.
//
// Ported from ../../StepperMotorTest/StepperMotorTest.ino, which used
// Arduino's `Stepper` library at 200 steps/rev and 100 RPM. The phase
// sequence is that library's and the step interval is its setSpeed()
// arithmetic, so the motor turns as it did on the bench sketch. The pins and
// both numbers now come from board_config.h.
//
// The difference is that nothing here blocks. Arduino's `step()` busy-waits
// through `delayMicroseconds()` for the whole move, which would freeze the
// serial console and the HTTP server for as long as the motor ran -- and with
// the dead-man watching, a frozen HTTP server means a stopped motor. An
// esp_timer periodic advances one phase per tick instead, so "on" means
// "turning" rather than "the firmware is inside a loop".
//
// The stepper and the actuator must never run together -- one mechanism, one
// supply. That interlock is NOT here: it lives in `controller`, the one place
// every client's commands pass through. See controller.c.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

#define STEPPER_STEPS_PER_REV  BOARD_STEPPER_STEPS_PER_REV
#define STEPPER_RPM            BOARD_STEPPER_RPM
#define STEPPER_PHASES         4

// Arduino's setSpeed() arithmetic, which is what made the sketch's motion what
// it was: 60 s / 200 steps / 100 rpm = 3000 us per step.
#define STEPPER_STEP_INTERVAL_US  (60UL * 1000000UL / STEPPER_STEPS_PER_REV / STEPPER_RPM)

// Which way the phase sequence is walked. Clockwise walks it forwards, which
// is the direction the Arduino sketch's `motor.step(+200)` turned.
//
// Whether that is clockwise *at the output shaft* depends on how the coils are
// wired and which end you are looking at. If CW and CCW come out swapped on
// your motor, swap one coil pair at the driver (IN1 with IN2, say) rather than
// editing the sequence: the sequence is the Arduino library's and is the one
// thing in here that should not drift.
typedef enum {
    STEPPER_CW  = +1,
    STEPPER_CCW = -1,
} stepper_dir_t;

// What a start request actually did, so the caller can say so precisely
// instead of guessing from a bool.
typedef enum {
    STEPPER_STARTED,     // was idle, now turning in the requested direction
    STEPPER_REVERSED,    // was already turning, the other way
    STEPPER_UNCHANGED,   // already turning that way; nothing to do
} stepper_start_t;

void stepper_motor_init(void);

// Turn continuously in `dir` until stopped.
//
// Asking for the direction it is already turning is not an error -- a repeated
// tap on a web button returns STEPPER_UNCHANGED. Asking for the opposite
// direction reverses it where it stands: the sequence is simply walked the
// other way from the phase it is on. There is no ramp, so under load a
// reversal can slip steps, and nothing here detects that.
stepper_start_t stepper_motor_start(stepper_dir_t dir);

// Stop and release the coils. Returns false when it was already idle.
//
// Releasing matters: full-step energises both coils in every phase, so a
// stopped-but-still-driven motor would sit at its rated current indefinitely,
// heating the windings and the driver for a holding torque this mechanism does
// not need. The cost is that the shaft is free to be turned by hand when off.
bool stepper_motor_stop(void);

bool stepper_motor_running(void);

// The direction it is turning, or the one it last turned, when stopped.
stepper_dir_t stepper_motor_dir(void);

// "clockwise" / "counter-clockwise", for logs and replies.
const char *stepper_motor_dir_name(stepper_dir_t dir);

// Steps taken since the current run started, for the status page.
//
// A count of steps taken, NOT net rotation: a reversal keeps adding to it
// rather than winding it back, because what it answers is "how long has this
// been turning", and with no feedback there is nothing to call a position.
// Reset by a start from idle, not by a reversal and not by stop, so the count
// survives being read after the motor has stopped.
uint64_t stepper_motor_steps(void);
