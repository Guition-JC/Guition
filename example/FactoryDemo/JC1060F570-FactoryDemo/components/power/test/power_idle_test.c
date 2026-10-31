#include <assert.h>
#include <stdio.h>

#include "power_idle.h"

static void test_default_is_one_minute(void)
{
    power_idle_state_t state;
    power_idle_init(&state, POWER_IDLE_DEFAULT_TIMEOUT_SECONDS, 0);

    assert(POWER_IDLE_DEFAULT_TIMEOUT_SECONDS == 60U);
    assert(power_idle_get_timeout_seconds(&state) == 60U);
    assert(!power_idle_is_sleeping(&state));
    assert(!power_idle_should_sleep(&state, 59999));
    assert(power_idle_should_sleep(&state, 60000));
}

static void test_activity_restarts_the_countdown(void)
{
    power_idle_state_t state;
    power_idle_init(&state, POWER_IDLE_TIMEOUT_30S, 0);

    assert(!power_idle_should_sleep(&state, 29000));
    power_idle_note_activity(&state, 29000);
    assert(!power_idle_should_sleep(&state, 58000));
    assert(power_idle_should_sleep(&state, 59000));
}

static void test_off_never_sleeps(void)
{
    power_idle_state_t state;
    power_idle_init(&state, POWER_IDLE_TIMEOUT_OFF, 0);

    assert(power_idle_get_timeout_seconds(&state) == 0U);
    assert(!power_idle_should_sleep(&state, 10000000));
}

static void test_latch_blocks_repeat_and_wake_rearms(void)
{
    power_idle_state_t state;
    power_idle_init(&state, POWER_IDLE_TIMEOUT_1MIN, 0);

    assert(power_idle_should_sleep(&state, 60000));
    power_idle_mark_sleeping(&state);
    assert(power_idle_is_sleeping(&state));
    assert(!power_idle_should_sleep(&state, 600000));

    power_idle_mark_awake(&state, 600000);
    assert(!power_idle_is_sleeping(&state));
    assert(!power_idle_should_sleep(&state, 659999));
    assert(power_idle_should_sleep(&state, 660000));
}

static void test_note_activity_also_clears_the_latch(void)
{
    power_idle_state_t state;
    power_idle_init(&state, POWER_IDLE_TIMEOUT_2MIN, 0);
    power_idle_mark_sleeping(&state);

    power_idle_note_activity(&state, 5000);
    assert(!power_idle_is_sleeping(&state));
    assert(!power_idle_should_sleep(&state, 124999));
    assert(power_idle_should_sleep(&state, 125000));
}

static void test_only_the_five_settings_steps_are_accepted(void)
{
    assert(power_idle_timeout_supported(0U));
    assert(power_idle_timeout_supported(30U));
    assert(power_idle_timeout_supported(60U));
    assert(power_idle_timeout_supported(120U));
    assert(power_idle_timeout_supported(300U));
    assert(!power_idle_timeout_supported(1U));
    assert(!power_idle_timeout_supported(45U));
    assert(!power_idle_timeout_supported(600U));

    assert(power_idle_sanitize_timeout(45U) == POWER_IDLE_DEFAULT_TIMEOUT_SECONDS);
    assert(power_idle_sanitize_timeout(300U) == 300U);

    power_idle_state_t state;
    power_idle_init(&state, 45U, 0);
    assert(power_idle_get_timeout_seconds(&state) == POWER_IDLE_DEFAULT_TIMEOUT_SECONDS);
}

static void test_timeout_change_keeps_the_activity_stamp(void)
{
    power_idle_state_t state;
    power_idle_init(&state, POWER_IDLE_TIMEOUT_5MIN, 0);
    assert(!power_idle_should_sleep(&state, 100000));

    power_idle_set_timeout(&state, POWER_IDLE_TIMEOUT_30S);
    assert(power_idle_get_timeout_seconds(&state) == 30U);
    assert(power_idle_should_sleep(&state, 100000));
}

static void test_backwards_clock_never_sleeps(void)
{
    power_idle_state_t state;
    power_idle_init(&state, POWER_IDLE_TIMEOUT_1MIN, 120000);

    assert(!power_idle_should_sleep(&state, 119000));
    assert(power_idle_should_sleep(&state, 180000));
}

static void test_null_state_is_a_no_op(void)
{
    power_idle_init(NULL, POWER_IDLE_TIMEOUT_1MIN, 0);
    power_idle_set_timeout(NULL, POWER_IDLE_TIMEOUT_1MIN);
    power_idle_note_activity(NULL, 0);
    power_idle_mark_sleeping(NULL);
    power_idle_mark_awake(NULL, 0);
    assert(power_idle_get_timeout_seconds(NULL) == 0U);
    assert(!power_idle_is_sleeping(NULL));
    assert(!power_idle_should_sleep(NULL, 100000));
}

int main(void)
{
    test_default_is_one_minute();
    test_activity_restarts_the_countdown();
    test_off_never_sleeps();
    test_latch_blocks_repeat_and_wake_rearms();
    test_note_activity_also_clears_the_latch();
    test_only_the_five_settings_steps_are_accepted();
    test_timeout_change_keeps_the_activity_stamp();
    test_backwards_clock_never_sleeps();
    test_null_state_is_a_no_op();
    printf("power_idle_test ok\n");
    return 0;
}
