#include "controller.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "command.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "secrets.h"
#include "actuator.h"
#include "positioner.h"

static char s_last_event[CONTROLLER_REPLY_MAX] = "ready";

// One lock for every path that changes the actuator or the positioner.
//
// Four tasks get here: the HTTP server, BLE, the serial console, and the
// esp_timer task (a timed move finishing, the dead-man firing). Without a lock
// a move finishing while a command arrives could interleave -- the timer
// stopping an actuator that a jog had just started, or one channel written by each
// -- which showed on the bench as control that was "sometimes not stable".
//
// Recursive, because a handler may call back into code that takes it again.
static SemaphoreHandle_t s_lock;
#define LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_lock)

static void note(const char *text)
{
    snprintf(s_last_event, sizeof(s_last_event), "%s", text);
}

static void describe_actuator(char *out, size_t n)
{
    snprintf(out, n, "Direction: %c, speed: %d%%  [A=%lu B=%lu]",
             actuator_direction(), actuator_speed_pct(),
             (unsigned long)actuator_duty_a(), (unsigned long)actuator_duty_b());
}

static void describe_level(char *out, size_t n)
{
    const int level = positioner_level();
    if (positioner_moving())         snprintf(out, n, "Level: moving...");
    else if (level == LEVEL_UNKNOWN) snprintf(out, n, "Level: unknown (press h to home)");
    else                             snprintf(out, n, "Level: %d/%d", level, LEVELS);
}

// Runs in the esp_timer task when a timed move is due to finish.
static void on_move_due(void)
{
    LOCK();
    if (positioner_complete()) {
        char text[CONTROLLER_REPLY_MAX];
        describe_level(text, sizeof(text));
        note(text);
        printf("%s\n", text);               // the serial client is always listening
        fflush(stdout);
    }
    UNLOCK();
}

void controller_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    configASSERT(s_lock);
    actuator_init();                        // actuator off before anything can command it
    positioner_init(on_move_due);
}

static bool handle_locked(uint8_t c, char *reply, size_t reply_len);

bool controller_handle(uint8_t c, char *reply, size_t reply_len)
{
    LOCK();
    const bool handled = handle_locked(c, reply, reply_len);
    UNLOCK();
    return handled;
}

static bool handle_locked(uint8_t c, char *reply, size_t reply_len)
{
    const command_t cmd = command_parse(c);
    reply[0] = '\0';

    switch (cmd.kind) {

    case CMD_DIRECTION:
        positioner_cancel();                // manual jog: position no longer known
        actuator_set((char)cmd.value, actuator_speed_pct());
        describe_actuator(reply, reply_len);
        break;

    case CMD_SPEED:
        // A timed move only knows where it is because it runs at
        // STEP_SPEED_PCT. Changing speed mid-move left the level wrong by
        // however much slower the rest of the move went.
        if (positioner_moving()) {
            snprintf(reply, reply_len,
                     "Move in progress at %d%% -- set speed after it, or press s",
                     STEP_SPEED_PCT);
            break;
        }
        actuator_set(actuator_direction(), cmd.value);
        if (speed_is_clamped(cmd.value)) {
            char state[CONTROLLER_REPLY_MAX];
            describe_actuator(state, sizeof(state));
            snprintf(reply, reply_len, "%d%% below the %d%% stall floor -- clamped. %s",
                     cmd.value, SPEED_MIN_PCT, state);
        } else {
            describe_actuator(reply, reply_len);
        }
        break;

    case CMD_LEVEL_END:
        if (positioner_go_end((char)cmd.value)) {
            snprintf(reply, reply_len, "Homing to %s end (%d ms)...",
                     cmd.value == 'h' ? "retracted" : "extended", HOME_MS);
        } else {
            snprintf(reply, reply_len, "Already homing to the %s end",
                     cmd.value == 'h' ? "retracted" : "extended");
        }
        break;

    case CMD_LEVEL_STEP: {
        int from = 0, to = 0;
        switch (positioner_step(cmd.value, &from, &to)) {
        case POS_OK:
            snprintf(reply, reply_len, "Level %d -> %d (%d ms)...", from, to, STEP_MS);
            break;
        case POS_UNKNOWN:
            snprintf(reply, reply_len, "Level unknown -- press h or t first");
            break;
        case POS_AT_END:
            snprintf(reply, reply_len, "Already at %s end (level %d/%d)",
                     cmd.value > 0 ? "extended" : "retracted",
                     positioner_level(), LEVELS);
            break;
        case POS_BUSY:
            snprintf(reply, reply_len, "Move in progress -- wait for it, or press s to abort");
            break;
        }
        break;
    }

    case CMD_LEVEL_SHOW:
        describe_level(reply, reply_len);
        break;

    case CMD_DECAY_TOGGLE:
        actuator_set_slow_decay(!actuator_slow_decay());
        snprintf(reply, reply_len, "Decay: %s",
                 actuator_slow_decay() ? "slow (brake off-phase)" : "fast (coast off-phase)");
        break;

    case CMD_RAW_HOLD:
        positioner_cancel();
        actuator_hold_raw((char)cmd.value);
        snprintf(reply, reply_len, "RAW: %s held high, other low. Press s to stop.",
                 actuator_pin_name((char)cmd.value));
        break;

    case CMD_REVEAL_KEY:
        controller_reveal_key();
        snprintf(reply, reply_len, "device key printed above");
        break;

    case CMD_PROVISION:
        // Interactive, and it would mean handing credentials to whichever
        // transport asked. Serial runs it directly; BLE gets its own
        // characteristic in Phase 3.
        snprintf(reply, reply_len, "provisioning is serial-only on this build");
        break;

    case CMD_FORGET_WIFI:
        snprintf(reply, reply_len, "forget-wifi is serial-only on this build");
        break;

    case CMD_PAIR:
        // Opening the pairing window remotely would let an attacker create
        // their own way in. It takes a physical act at the console.
        snprintf(reply, reply_len, "pairing is opened from the serial console only");
        break;

    case CMD_IGNORE:
        return false;                       // CR, LF, anything unrecognised
    }

    note(reply);
    return true;
}

void controller_status_json(char *out, size_t out_len)
{
    LOCK();                                 // one consistent snapshot, not a mix of two states
    const int level = positioner_level();
    char level_text[CONTROLLER_REPLY_MAX];
    describe_level(level_text, sizeof(level_text));

    snprintf(out, out_len,
             "{\"dir\":\"%c\",\"speed\":%d,\"dutyA\":%lu,\"dutyB\":%lu,"
             "\"level\":%d,\"levels\":%d,\"moving\":%s,\"slowDecay\":%s,"
             "\"levelText\":\"%s\",\"event\":\"%s\"}",
             actuator_direction(), actuator_speed_pct(),
             (unsigned long)actuator_duty_a(), (unsigned long)actuator_duty_b(),
             level, LEVELS,
             positioner_moving() ? "true" : "false",
             actuator_slow_decay() ? "true" : "false",
             level_text, s_last_event);
    UNLOCK();
}

void controller_reveal_key(void)
{
    const uint8_t *key = secrets_device_key();
    printf("\n*** DEVICE KEY REVEALED ON SERIAL -- bench use only ***\n");
    printf("key: ");
    for (size_t i = 0; i < 32; i++) {
        printf("%02x", key[i]);
    }
    printf("\n*** anyone reading this console can now control the dock ***\n\n");
    fflush(stdout);
}

void controller_emergency_stop(const char *why)
{
    LOCK();
    positioner_cancel();
    actuator_set('s', actuator_speed_pct());
    note(why);
    UNLOCK();
    printf("STOP: %s\n", why);
    fflush(stdout);
}
