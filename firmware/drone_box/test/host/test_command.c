// Host unit tests for the `command` component.
//
// These run on the development machine, not the ESP32-S3. They cover the parts
// of the firmware that can be got wrong by reasoning rather than by wiring:
// the parse table, the stall-floor clamp, the LEDC full-scale correction and
// level bounds. Test IDs match the table in README.md.

#include <string.h>

#include "command.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// U01-U04: command_parse
// ---------------------------------------------------------------------------

static void assert_cmd(uint8_t c, cmd_kind_t kind, int value)
{
    const command_t cmd = command_parse(c);
    TEST_ASSERT_EQUAL_INT(kind, cmd.kind);
    TEST_ASSERT_EQUAL_INT(value, cmd.value);
}

static void test_parse_directions(void)
{
    assert_cmd('e', CMD_DIRECTION, 'e');
    assert_cmd('r', CMD_DIRECTION, 'r');
    assert_cmd('s', CMD_DIRECTION, 's');
}

static void test_parse_levels_and_modes(void)
{
    assert_cmd('h', CMD_LEVEL_END,    'h');
    assert_cmd('t', CMD_LEVEL_END,    't');
    assert_cmd('+', CMD_LEVEL_STEP,   +1);
    assert_cmd('-', CMD_LEVEL_STEP,   -1);
    assert_cmd('p', CMD_LEVEL_SHOW,    0);
    assert_cmd('d', CMD_DECAY_TOGGLE,  0);
    assert_cmd('A', CMD_RAW_HOLD,     'A');
    assert_cmd('B', CMD_RAW_HOLD,     'B');
    assert_cmd('c', CMD_STEPPER,      'c');  // stepper on, clockwise
    assert_cmd('w', CMD_STEPPER,      'w');  // stepper on, counter-clockwise
    assert_cmd('f', CMD_STEPPER,      'f');  // stepper off
    assert_cmd('K', CMD_REVEAL_KEY,    0);   // serial-only bench affordance
    assert_cmd('W', CMD_PROVISION,     0);   // serial-only, interactive
    assert_cmd('X', CMD_FORGET_WIFI,   0);
    assert_cmd('P', CMD_PAIR,          0);   // serial-only: opens the BLE window
}

static void test_parse_digits(void)
{
    // '0' means full speed, not zero -- inherited from the Arduino sketch.
    assert_cmd('0', CMD_SPEED, 100);
    assert_cmd('1', CMD_SPEED,  10);
    assert_cmd('5', CMD_SPEED,  50);
    assert_cmd('9', CMD_SPEED,  90);
}

static void test_parse_ignores_noise(void)
{
    assert_cmd('\r', CMD_IGNORE, 0);        // line endings must not act
    assert_cmd('\n', CMD_IGNORE, 0);
    assert_cmd(' ',  CMD_IGNORE, 0);
    assert_cmd('z',  CMD_IGNORE, 0);
    assert_cmd('o',  CMD_IGNORE, 0);        // 'o' is not stepper-on: see command.c
    assert_cmd('n',  CMD_IGNORE, 0);        // 'n'/'m' were rejected as neighbours
    assert_cmd('m',  CMD_IGNORE, 0);
    assert_cmd('E',  CMD_IGNORE, 0);        // case matters: 'E' is not 'e'
    assert_cmd(0x00, CMD_IGNORE, 0);
    assert_cmd(0xFF, CMD_IGNORE, 0);
}

// ---------------------------------------------------------------------------
// U17-U19: the interlock predicates
//
// The actuator and the stepper share one mechanism and one supply, so the
// firmware allows exactly one of them to be energised. These decide which side
// a command falls on, and `controller` enforces the rule using them -- so this
// is the one place the rule itself can be checked without a board.
// ---------------------------------------------------------------------------

static bool energises_actuator(uint8_t c)
{
    return command_energises_actuator(command_parse(c));
}

static bool energises_stepper(uint8_t c)
{
    return command_energises_stepper(command_parse(c));
}

static void test_actuator_commands_are_recognised(void)
{
    TEST_ASSERT_TRUE(energises_actuator('e'));      // jog extend
    TEST_ASSERT_TRUE(energises_actuator('r'));      // jog retract
    TEST_ASSERT_TRUE(energises_actuator('h'));      // home
    TEST_ASSERT_TRUE(energises_actuator('t'));      // top
    TEST_ASSERT_TRUE(energises_actuator('+'));      // one level
    TEST_ASSERT_TRUE(energises_actuator('-'));
    TEST_ASSERT_TRUE(energises_actuator('A'));      // raw hold drives a pin
    TEST_ASSERT_TRUE(energises_actuator('B'));
}

static void test_stop_and_queries_are_never_blocked(void)
{
    // The whole point of the predicate: stop must pass the interlock gate, or
    // a running stepper would make the actuator's own STOP unreachable.
    TEST_ASSERT_FALSE(energises_actuator('s'));
    TEST_ASSERT_FALSE(energises_actuator('p'));     // level query
    TEST_ASSERT_FALSE(energises_actuator('d'));     // decay toggle, no drive
    TEST_ASSERT_FALSE(energises_actuator('5'));     // speed only
    TEST_ASSERT_FALSE(energises_actuator('0'));
    TEST_ASSERT_FALSE(energises_actuator('K'));     // not about the motor at all
    TEST_ASSERT_FALSE(energises_actuator('W'));
    TEST_ASSERT_FALSE(energises_actuator('\r'));    // CMD_IGNORE
}

static void test_the_two_sides_never_overlap(void)
{
    // Both directions are on the stepper's side and no command is on both --
    // if one ever were, the interlock would deadlock itself, refusing a
    // command because of the state it would itself create.
    TEST_ASSERT_TRUE(energises_stepper('c'));       // clockwise
    TEST_ASSERT_TRUE(energises_stepper('w'));       // counter-clockwise
    TEST_ASSERT_FALSE(energises_stepper('f'));      // off: always allowed
    TEST_ASSERT_FALSE(energises_actuator('c'));
    TEST_ASSERT_FALSE(energises_actuator('w'));
    TEST_ASSERT_FALSE(energises_actuator('f'));

    for (int c = 0; c < 256; c++) {
        const command_t cmd = command_parse((uint8_t)c);
        TEST_ASSERT_FALSE(command_energises_actuator(cmd) &&
                          command_energises_stepper(cmd));
    }
}

// ---------------------------------------------------------------------------
// U05-U07: the stall-floor clamp
// ---------------------------------------------------------------------------

static void test_speed_clamp_raises_weak_speeds(void)
{
    TEST_ASSERT_EQUAL_INT(SPEED_MIN_PCT, speed_clamp(10));
    TEST_ASSERT_EQUAL_INT(SPEED_MIN_PCT, speed_clamp(30));
    TEST_ASSERT_EQUAL_INT(SPEED_MIN_PCT, speed_clamp(0));
    TEST_ASSERT_EQUAL_INT(SPEED_MIN_PCT, speed_clamp(-5));
}

static void test_speed_clamp_passes_usable_speeds(void)
{
    TEST_ASSERT_EQUAL_INT(50,  speed_clamp(50));    // exactly at the floor
    TEST_ASSERT_EQUAL_INT(70,  speed_clamp(70));
    TEST_ASSERT_EQUAL_INT(100, speed_clamp(100));
    TEST_ASSERT_EQUAL_INT(100, speed_clamp(150));   // and caps the top
}

static void test_speed_is_clamped_reports_accurately(void)
{
    TEST_ASSERT_TRUE(speed_is_clamped(10));
    TEST_ASSERT_TRUE(speed_is_clamped(49));
    TEST_ASSERT_FALSE(speed_is_clamped(50));
    TEST_ASSERT_FALSE(speed_is_clamped(100));
    TEST_ASSERT_TRUE(speed_is_clamped(101));
}

// ---------------------------------------------------------------------------
// U08-U10: duty conversion -- the regression tests that matter most
// ---------------------------------------------------------------------------

static void test_full_speed_is_full_scale_not_255(void)
{
    // THE bug that cost an afternoon: LEDC's 8-bit duty range is 0..256.
    // 255 is 99.6% and the ZK-5AD's forward input ignores it. Only 256 holds
    // the pin solidly high. Arduino's ledcWrite() applies the same correction.
    TEST_ASSERT_EQUAL_UINT32(256, speed_to_duty(100));
    TEST_ASSERT_EQUAL_UINT32(DUTY_FULL_SCALE, speed_to_duty(100));
    TEST_ASSERT_NOT_EQUAL_UINT32(255, speed_to_duty(100));
}

static void test_duty_matches_arduino_map(void)
{
    // Integer truncation is intentional: it matches Arduino's map().
    TEST_ASSERT_EQUAL_UINT32(127, speed_to_duty(50));
    TEST_ASSERT_EQUAL_UINT32(153, speed_to_duty(60));
    TEST_ASSERT_EQUAL_UINT32(178, speed_to_duty(70));
    TEST_ASSERT_EQUAL_UINT32(204, speed_to_duty(80));
    TEST_ASSERT_EQUAL_UINT32(229, speed_to_duty(90));
}

static void test_duty_applies_the_clamp(void)
{
    // A weak speed must come out as the floor's duty, never a stalling one.
    TEST_ASSERT_EQUAL_UINT32(speed_to_duty(SPEED_MIN_PCT), speed_to_duty(10));
    TEST_ASSERT_EQUAL_UINT32(speed_to_duty(SPEED_MIN_PCT), speed_to_duty(0));
}

static void test_duty_never_exceeds_full_scale(void)
{
    for (int pct = -20; pct <= 150; pct++) {
        const uint32_t d = speed_to_duty(pct);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(DUTY_FULL_SCALE, d);
        TEST_ASSERT_GREATER_THAN_UINT32(0, d);   // never a silent zero
    }
}

// ---------------------------------------------------------------------------
// U11-U12: level bounds
// ---------------------------------------------------------------------------

static void test_level_steps_inside_range(void)
{
    TEST_ASSERT_TRUE(level_step_valid(0, +1, 5));
    TEST_ASSERT_TRUE(level_step_valid(4, +1, 5));
    TEST_ASSERT_TRUE(level_step_valid(5, -1, 5));
    TEST_ASSERT_TRUE(level_step_valid(1, -1, 5));
}

static void test_level_steps_refuse_past_the_ends(void)
{
    TEST_ASSERT_FALSE(level_step_valid(5, +1, 5));   // past full extension
    TEST_ASSERT_FALSE(level_step_valid(0, -1, 5));   // past full retraction
}

// ---------------------------------------------------------------------------
// U14-U16: URL decoding -- regression for the Step + button
// ---------------------------------------------------------------------------

static void decode_equals(const char *in, const char *expect)
{
    char out[32];
    const size_t n = command_url_decode(in, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(expect, out);
    TEST_ASSERT_EQUAL_size_t(strlen(expect), n);
}

static void test_decode_plus_reaches_the_parser(void)
{
    // THE bug: the web UI sends '+' as %2B, ESP-IDF does not decode it, and
    // the handler saw '%'. Every other button worked because e/r/s/h/t and
    // digits are unreserved and never get encoded.
    char out[8];
    command_url_decode("%2B", out, sizeof(out));
    TEST_ASSERT_EQUAL_CHAR('+', out[0]);
    assert_cmd((uint8_t)out[0], CMD_LEVEL_STEP, +1);
}

static void test_decode_handles_ordinary_values(void)
{
    decode_equals("e", "e");
    decode_equals("-", "-");                // unreserved: passes through
    decode_equals("%2D", "-");              // but also survives being encoded
    decode_equals("%41%42", "AB");
    decode_equals("a%2bb", "a+b");          // lowercase hex digits
    decode_equals("", "");
}

static void test_decode_is_robust_to_junk(void)
{
    decode_equals("+", " ");                // form encoding: '+' means space
    decode_equals("%", "%");                // truncated escape, passed through
    decode_equals("%A", "%A");              // one digit only
    decode_equals("%ZZ", "%ZZ");            // not hex at all

    // Must not overrun a short buffer.
    char small[4];
    const size_t n = command_url_decode("%41%42%43%44%45", small, sizeof(small));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_STRING("ABC", small);

    char none[1];
    TEST_ASSERT_EQUAL_size_t(0, command_url_decode("abc", none, 0));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_parse_directions);
    RUN_TEST(test_parse_levels_and_modes);
    RUN_TEST(test_parse_digits);
    RUN_TEST(test_parse_ignores_noise);

    RUN_TEST(test_actuator_commands_are_recognised);
    RUN_TEST(test_stop_and_queries_are_never_blocked);
    RUN_TEST(test_the_two_sides_never_overlap);

    RUN_TEST(test_speed_clamp_raises_weak_speeds);
    RUN_TEST(test_speed_clamp_passes_usable_speeds);
    RUN_TEST(test_speed_is_clamped_reports_accurately);

    RUN_TEST(test_full_speed_is_full_scale_not_255);
    RUN_TEST(test_duty_matches_arduino_map);
    RUN_TEST(test_duty_applies_the_clamp);
    RUN_TEST(test_duty_never_exceeds_full_scale);

    RUN_TEST(test_level_steps_inside_range);
    RUN_TEST(test_level_steps_refuse_past_the_ends);

    RUN_TEST(test_decode_plus_reaches_the_parser);
    RUN_TEST(test_decode_handles_ordinary_values);
    RUN_TEST(test_decode_is_robust_to_junk);

    return UNITY_END();
}
