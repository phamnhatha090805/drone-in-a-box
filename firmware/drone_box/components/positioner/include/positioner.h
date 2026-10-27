// Dead-reckoned positioning: the stroke is split into LEVELS steps and timed.
//
// There is no position feedback on this rig, so level is inferred from how
// long the actuator has run. That is only valid when every timed move runs at
// same speed (STEP_SPEED_PCT) from a known datum, which homing establishes.
// Any manual jog must call positioner_cancel().
//
// Moves are non-blocking: an esp_timer one-shot stops the actuator when the step
// is due, so the console stays responsive and nothing sleeps or spins.
#pragma once

#include <stdbool.h>

#include "board_config.h"

// MEASURE TRAVEL_MS on your actuator: press 'h', then time 't' with a
// stopwatch. The shipped value is a placeholder and every level depends on it.
// Edit it, and the level count and step speed, in board_config.h.
#define TRAVEL_MS       BOARD_TRAVEL_MS      // full stroke, ms, at STEP_SPEED_PCT
#define LEVELS          BOARD_LEVELS
#define STEP_SPEED_PCT  BOARD_STEP_SPEED_PCT // timed moves always run at this speed

// Derived, so they cannot drift out of step with the stroke above.
#define STEP_MS         (TRAVEL_MS / LEVELS)
#define HOME_MS         ((TRAVEL_MS * BOARD_HOME_OVERTRAVEL_PCT) / 100)

#define LEVEL_UNKNOWN   (-1)

typedef enum {
    POS_OK,             // move started
    POS_UNKNOWN,        // level not established: home first
    POS_AT_END,         // already at the requested end of travel
    POS_BUSY,           // a move is still running; the level is not settled yet
} pos_result_t;

// Called from the esp_timer task when a timed move is due to finish. It does
// not finish the move itself: the callee takes whatever lock guards the actuator
// and then calls positioner_complete(), so finishing a move can never
// interleave with a command arriving on another task.
typedef void (*positioner_due_cb_t)(void);

void positioner_init(positioner_due_cb_t on_due);

// Finish the move that is due: stop the actuator and settle the level. Returns
// false, doing nothing, when no move is due -- the timer callback had already
// been dispatched when a cancel or a newer move overtook it, and
// esp_timer_stop() cannot recall it.
bool positioner_complete(void);

// 'h' = retracted end (level 0), 't' = extended end (level LEVELS).
// Returns false, changing nothing, when a move to that same end is already
// running: a repeated tap must not restart the 13 s timer each time.
bool positioner_go_end(char which);

// step is +1 or -1. On POS_OK, *from and *to carry the levels for reporting.
pos_result_t positioner_step(int step, int *from, int *to);

// Abort any move in flight and mark the level unknown. Call on every manual
// jog, because dead reckoning no longer holds once the actuator is driven by hand.
void positioner_cancel(void);

int  positioner_level(void);
bool positioner_moving(void);
