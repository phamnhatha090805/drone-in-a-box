#include "positioner.h"

#include "esp_err.h"
#include "esp_timer.h"
#include "actuator.h"

// Slack for the due-time check: esp_timer never fires early, but the deadline
// is taken a moment before the timer is armed.
#define DUE_SLACK_US  2000

static esp_timer_handle_t s_timer;
static positioner_due_cb_t s_on_due;
static int     s_level        = LEVEL_UNKNOWN;
static int     s_target_level = LEVEL_UNKNOWN;
static bool    s_moving;
static int64_t s_due_us;                    // when the move in flight should end

// Runs in the esp_timer task. Hands over to the caller, who finishes the move
// under its own lock -- see positioner_due_cb_t.
static void move_due(void *arg)
{
    (void)arg;
    if (s_on_due) {
        s_on_due();
    } else {
        positioner_complete();
    }
}

bool positioner_complete(void)
{
    if (!s_moving || esp_timer_get_time() + DUE_SLACK_US < s_due_us) {
        return false;                       // cancelled, or a newer move is running
    }
    actuator_set('s', actuator_speed_pct());      // stop at the level, do not coast on
    s_level  = s_target_level;
    s_moving = false;
    return true;
}

static void start_move(char dir, uint32_t ms, int to_level)
{
    esp_timer_stop(s_timer);                // harmless if it is not running
    s_target_level = to_level;
    s_moving       = true;
    s_due_us       = esp_timer_get_time() + (int64_t)ms * 1000;
    actuator_set(dir, STEP_SPEED_PCT);         // dead reckoning is only valid at one speed
    ESP_ERROR_CHECK(esp_timer_start_once(s_timer, (uint64_t)ms * 1000));
}

void positioner_init(positioner_due_cb_t on_due)
{
    s_on_due = on_due;
    const esp_timer_create_args_t args = { .callback = move_due, .name = "move" };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer));
}

bool positioner_go_end(char which)
{
    const int to = which == 'h' ? 0 : LEVELS;
    if (s_moving && s_target_level == to) {
        return false;                       // already on its way there
    }
    start_move(which == 'h' ? 'r' : 'e', HOME_MS, to);
    return true;
}

pos_result_t positioner_step(int step, int *from, int *to)
{
    // Stepping mid-move would start from a stale level and quietly corrupt the
    // dead reckoning, so refuse rather than guess.
    if (s_moving)                                      return POS_BUSY;
    if (s_level == LEVEL_UNKNOWN)                      return POS_UNKNOWN;
    if (s_level + step < 0 || s_level + step > LEVELS) return POS_AT_END;

    *from = s_level;
    *to   = s_level + step;
    start_move(step > 0 ? 'e' : 'r', STEP_MS, *to);
    return POS_OK;
}

void positioner_cancel(void)
{
    esp_timer_stop(s_timer);
    s_level        = LEVEL_UNKNOWN;
    s_target_level = LEVEL_UNKNOWN;
    s_moving       = false;
}

int  positioner_level(void)  { return s_level; }
bool positioner_moving(void) { return s_moving; }
