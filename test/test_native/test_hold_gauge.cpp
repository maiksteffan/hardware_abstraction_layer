/**
 * @file test_hold_gauge.cpp
 * @brief Host tests for the hold-time gauge geometry (HoldGauge.h).
 *
 * `HOLD_GAUGE <pos> <percent>` is how the board shows how much longer a hold
 * must stay gripped: the backend ticks the remaining percentage down and the
 * firmware shrinks the hold's green block towards its center. The mapping is a
 * cross-repo contract — the backend sizes its tick interval from the number of
 * levels this produces (`backend/app/services/led_service.py`).
 *
 * Built together with test_board_profiles.cpp, which owns main();
 * runHoldGaugeTests() is called from there.
 */

#include <unity.h>

#include "Config.h"
#include "HoldGauge.h"

// The board builds that exist today: a 5-LED core (v1/v2) and the single-LED
// core of dev-5hold.
static constexpr uint8_t WIDTH_WIDE = 5;
static constexpr uint8_t WIDTH_NARROW = 1;
static constexpr uint8_t RINGS = LED_SUCCESS_EXPANSION_RADIUS;

static void test_the_block_reaches_past_the_core_by_its_rings() {
    // 5-LED core = 2 LEDs each side of the center, plus 4 rings = 6.
    TEST_ASSERT_EQUAL_UINT8(6, holdGaugeMaxLevel(WIDTH_WIDE, RINGS));
    TEST_ASSERT_EQUAL_UINT8(4, holdGaugeMaxLevel(WIDTH_NARROW, RINGS));
}

static void test_full_time_lights_the_whole_block() {
    TEST_ASSERT_EQUAL_UINT8(holdGaugeMaxLevel(WIDTH_WIDE, RINGS),
                            holdGaugeLevel(100, WIDTH_WIDE, RINGS));
    TEST_ASSERT_EQUAL_UINT8(holdGaugeMaxLevel(WIDTH_NARROW, RINGS),
                            holdGaugeLevel(100, WIDTH_NARROW, RINGS));
}

static void test_no_time_left_keeps_the_center_led_on() {
    // The hold is still the active one, so it must not go dark; the backend
    // decides when the step is over.
    TEST_ASSERT_EQUAL_UINT8(0, holdGaugeLevel(0, WIDTH_WIDE, RINGS));
    TEST_ASSERT_EQUAL_UINT8(0, holdGaugeLevel(0, WIDTH_NARROW, RINGS));
}

static void test_a_percentage_above_full_is_clamped() {
    TEST_ASSERT_EQUAL_UINT8(holdGaugeMaxLevel(WIDTH_WIDE, RINGS),
                            holdGaugeLevel(255, WIDTH_WIDE, RINGS));
}

static void test_the_block_only_ever_shrinks_as_time_runs_out() {
    uint8_t previous = holdGaugeLevel(100, WIDTH_WIDE, RINGS);
    for (int16_t percent = 100; percent >= 0; percent--) {
        const uint8_t level = holdGaugeLevel((uint8_t)percent, WIDTH_WIDE, RINGS);
        TEST_ASSERT_TRUE(level <= previous);
        previous = level;
    }
    TEST_ASSERT_EQUAL_UINT8(0, previous);
}

static void test_every_level_is_reachable_by_ticking_down() {
    // Rounding, not truncation: a backend stepping the percentage down must
    // walk the block LED by LED instead of skipping one.
    const uint8_t maxLevel = holdGaugeMaxLevel(WIDTH_WIDE, RINGS);
    bool seen[16] = {false};
    for (int16_t percent = 0; percent <= 100; percent++) {
        seen[holdGaugeLevel((uint8_t)percent, WIDTH_WIDE, RINGS)] = true;
    }
    for (uint8_t level = 0; level <= maxLevel; level++) {
        TEST_ASSERT_TRUE_MESSAGE(seen[level], "a gauge level is never shown");
    }
}

static void test_the_midpoint_is_half_the_block() {
    TEST_ASSERT_EQUAL_UINT8(3, holdGaugeLevel(50, WIDTH_WIDE, RINGS));
    TEST_ASSERT_EQUAL_UINT8(2, holdGaugeLevel(50, WIDTH_NARROW, RINGS));
}

// ---------------------------------------------------------------------------

void runHoldGaugeTests() {
    RUN_TEST(test_the_block_reaches_past_the_core_by_its_rings);
    RUN_TEST(test_full_time_lights_the_whole_block);
    RUN_TEST(test_no_time_left_keeps_the_center_led_on);
    RUN_TEST(test_a_percentage_above_full_is_clamped);
    RUN_TEST(test_the_block_only_ever_shrinks_as_time_runs_out);
    RUN_TEST(test_every_level_is_reachable_by_ticking_down);
    RUN_TEST(test_the_midpoint_is_half_the_block);
}
