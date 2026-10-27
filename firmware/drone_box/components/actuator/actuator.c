#include "actuator.h"

#include "board_config.h"
#include "command.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"

// Pin numbers and the carrier come from board_config.h; the LEDC enum choices
// below stay here, because board_config.h must not include ESP-IDF headers.
#define ACTUATOR_PIN_A         ((gpio_num_t)BOARD_ACTUATOR_PIN_A)   // to D2
#define ACTUATOR_PIN_B         ((gpio_num_t)BOARD_ACTUATOR_PIN_B)   // to D3

#define PWM_FREQ_HZ   BOARD_ACTUATOR_PWM_FREQ_HZ
#define PWM_MODE      LEDC_LOW_SPEED_MODE   // the S3 has low-speed channels only

// The one place the configured bit count has to become an LEDC enum. Adding a
// resolution here means adding the matching case, by design: a silent fallback
// would rescale every duty.
#if   BOARD_ACTUATOR_PWM_BITS == 8
#define PWM_RES       LEDC_TIMER_8_BIT
#elif BOARD_ACTUATOR_PWM_BITS == 10
#define PWM_RES       LEDC_TIMER_10_BIT
#elif BOARD_ACTUATOR_PWM_BITS == 12
#define PWM_RES       LEDC_TIMER_12_BIT
#else
#error "BOARD_ACTUATOR_PWM_BITS has no LEDC resolution here -- add the case"
#endif

// Both channels share one timer: identical frequency, and on the S3 all
// low-speed LEDC timers share a clock source, so separate timers would invite
// cross-interaction for no benefit.
#define PWM_TIMER     LEDC_TIMER_0
#define CH_A          LEDC_CHANNEL_0
#define CH_B          LEDC_CHANNEL_1

static char     s_direction  = 's';
static int      s_speed_pct  = 100;
static bool     s_slow_decay = true;
static uint32_t s_duty[2];                  // what we last wrote, per channel

static void write_channel(ledc_channel_t channel, uint32_t value)
{
    ESP_ERROR_CHECK(ledc_set_duty(PWM_MODE, channel, value));
    ESP_ERROR_CHECK(ledc_update_duty(PWM_MODE, channel));
    s_duty[channel == CH_A ? 0 : 1] = value;
}

static void bind_pin(gpio_num_t pin, ledc_channel_t channel)
{
    ledc_channel_config_t cfg = {
        .speed_mode = PWM_MODE,
        .channel    = channel,
        .timer_sel  = PWM_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = pin,
        .duty       = 0,                    // pin low from the first cycle
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&cfg));
}

// Two ways to PWM an H-bridge, and they are not equivalent under load.
//
// Fast decay: drive one input with PWM, hold the other low. The off-phase
// leaves both inputs low, the bridge goes high-impedance and the motor coasts.
// Through a gearbox the torque collapses and anything below full speed does
// not move at all.
//
// Slow decay: hold the direction input at full scale and PWM the *other* input
// inverted. The off-phase puts both inputs high, shorting the motor terminals
// and braking, so current keeps circulating and low-speed torque holds up.
//
// Both collapse to the same thing at 100%: one input high, the other low.
static void apply(void)
{
    const uint32_t d = speed_to_duty(s_speed_pct);

    if (s_direction == 's') {
        write_channel(CH_A, 0);             // both low: coast
        write_channel(CH_B, 0);
    } else if (s_slow_decay) {
        const uint32_t off = DUTY_FULL_SCALE - d;
        if (s_direction == 'e') { write_channel(CH_A, DUTY_FULL_SCALE); write_channel(CH_B, off); }
        else                    { write_channel(CH_A, off);             write_channel(CH_B, DUTY_FULL_SCALE); }
    } else {
        if (s_direction == 'e') { write_channel(CH_A, d); write_channel(CH_B, 0); }
        else                    { write_channel(CH_A, 0); write_channel(CH_B, d); }
    }
}

void actuator_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = PWM_MODE,
        .timer_num       = PWM_TIMER,
        .duty_resolution = PWM_RES,
        .freq_hz         = PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    bind_pin(ACTUATOR_PIN_A, CH_A);
    bind_pin(ACTUATOR_PIN_B, CH_B);
    apply();                                // actuator off right after boot
}

void actuator_set(char direction, int speed_pct)
{
    s_direction = direction;
    s_speed_pct = speed_clamp(speed_pct);
    apply();
}

void actuator_hold_raw(char which)
{
    write_channel(CH_A, which == 'A' ? DUTY_FULL_SCALE : 0);
    write_channel(CH_B, which == 'B' ? DUTY_FULL_SCALE : 0);
    s_direction = 's';                      // keep state honest for the next command
}

void actuator_set_slow_decay(bool slow) { s_slow_decay = slow; apply(); }

bool        actuator_slow_decay(void) { return s_slow_decay; }
char        actuator_direction(void)  { return s_direction; }
int         actuator_speed_pct(void)  { return s_speed_pct; }
// The duty we set, not ledc_get_duty(): the hardware only latches a new duty
// at the start of the next PWM period, so reading it straight after a command
// returned the *previous* state -- "s -> [A=0 B=256]" in the log looked like a
// actuator still driving after STOP when it was not.
uint32_t    actuator_duty_a(void)     { return s_duty[0]; }
uint32_t    actuator_duty_b(void)     { return s_duty[1]; }
uint32_t    actuator_pwm_freq(void)   { return ledc_get_freq(PWM_MODE, PWM_TIMER); }
// Stringify through two levels so the macro expands before # sees it.
#define STR_(x)  #x
#define STR(x)   STR_(x)
const char *actuator_pin_name(char which)
{
    return which == 'A' ? "ACTUATOR_PIN_A/GPIO" STR(BOARD_ACTUATOR_PIN_A)
                        : "ACTUATOR_PIN_B/GPIO" STR(BOARD_ACTUATOR_PIN_B);
}
