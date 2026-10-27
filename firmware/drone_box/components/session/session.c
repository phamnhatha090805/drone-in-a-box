#include "session.h"

#include "controller.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "actuator.h"

#define TICK_US  (250 * 1000)

// 0 = this link has never spoken, so it is not armed and its silence proves
// nothing. A link arms itself the first time an authorised message arrives.
static volatile int64_t s_last_us[LINK_COUNT];
static esp_timer_handle_t s_timer;

static const char *link_name(link_t link)
{
    return link == LINK_HTTP ? "web client" : "BLE client";
}

bool session_any_live(void)
{
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < LINK_COUNT; i++) {
        if (s_last_us[i] == 0) {
            continue;
        }
        if ((now - s_last_us[i]) / 1000 <= SESSION_TIMEOUT_MS) {
            return true;
        }
    }
    return false;
}

static void tick(void *arg)
{
    (void)arg;

    if (actuator_direction() == 's') {
        return;                             // nothing is moving
    }

    const int64_t now = esp_timer_get_time();
    bool armed = false, live = false;
    link_t stale = LINK_HTTP;

    for (int i = 0; i < LINK_COUNT; i++) {
        if (s_last_us[i] == 0) {
            continue;                       // never spoke: not our business
        }
        armed = true;
        if ((now - s_last_us[i]) / 1000 <= SESSION_TIMEOUT_MS) {
            live = true;                    // somebody is still watching
        } else {
            stale = (link_t)i;
        }
    }

    // Only stop when a client that *was* talking has gone quiet and no other
    // remote client is live. A serial-only session never reaches here, because
    // serial does not arm anything.
    if (armed && !live) {
        for (int i = 0; i < LINK_COUNT; i++) {
            s_last_us[i] = 0;               // disarm until a client returns
        }
        char why[64];
        snprintf(why, sizeof(why), "%s stopped responding", link_name(stale));
        controller_emergency_stop(why);
    }
}

void session_touch(link_t link)
{
    if (link < LINK_COUNT) {
        s_last_us[link] = esp_timer_get_time();
    }
}

void session_init(void)
{
    const esp_timer_create_args_t args = { .callback = tick, .name = "deadman" };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer, TICK_US));
}
