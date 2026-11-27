/**
 * @file test_fuse_gauge.cpp
 * @brief Host tests for the fuse geometry (FuseGauge.h).
 *
 * `FUSE <pos> <0-1000>` is how the game Fuse shows the next hold: the Pi sends
 * the time left about every 40 ms, and the firmware shrinks the hold's blue
 * block towards its center without steps, dimming the LED at its end.
 *
 * Built together with test_board_profiles.cpp, which owns main();
 * runFuseGaugeTests() is called from there.
 */

#include <unity.h>

#include "FuseGauge.h"

// A 5-LED core with its rings, as on v1/v2: 6 LEDs either side of the center.
static constexpr uint8_t MAX_LEVEL = 6;

static uint16_t litOnOneSide(uint16_t permille) {
    const uint32_t edge = fuseEdge(permille, MAX_LEVEL);
    uint16_t sum = 0;
    for (uint8_t d = 0; d <= MAX_LEVEL; d++) sum += fuseFill(edge, d);
    return sum;
}

static void test_full_time_lights_the_whole_block() {
    const uint32_t edge = fuseEdge(FUSE_PERMILLE_MAX, MAX_LEVEL);
    for (uint8_t d = 0; d <= MAX_LEVEL; d++) {
        TEST_ASSERT_EQUAL_UINT8(255, fuseFill(edge, d));
    }
}

static void test_no_time_left_keeps_the_center_led_on() {
    // The hold is still the active one; the Pi switches it off.
    const uint32_t edge = fuseEdge(0, MAX_LEVEL);
    TEST_ASSERT_EQUAL_UINT8(255, fuseFill(edge, 0));
    for (uint8_t d = 1; d <= MAX_LEVEL; d++) {
        TEST_ASSERT_EQUAL_UINT8(0, fuseFill(edge, d));
    }
}

static void test_more_than_full_time_is_clamped() {
    TEST_ASSERT_EQUAL_UINT32(fuseEdge(FUSE_PERMILLE_MAX, MAX_LEVEL), fuseEdge(9999, MAX_LEVEL));
}

static void test_on_a_whole_led_the_bar_lights_what_the_gauge_does() {
    // Where the end of the bar sits exactly between two LEDs, it covers the
    // same LEDs as the stepped gauge at that level.
    for (uint8_t level = 0; level <= MAX_LEVEL; level++) {
        const uint32_t edge = ((uint32_t)level + 1u) * 1000u;
        for (uint8_t d = 0; d <= MAX_LEVEL; d++) {
            TEST_ASSERT_EQUAL_UINT8(d <= level ? 255 : 0, fuseFill(edge, d));
        }
    }
}

static void test_the_bar_shrinks_without_steps() {
    // One per mille less never takes away more than a sliver of light.
    uint16_t previous = litOnOneSide(FUSE_PERMILLE_MAX);
    for (int16_t permille = FUSE_PERMILLE_MAX - 1; permille >= 0; permille--) {
        const uint16_t lit = litOnOneSide((uint16_t)permille);
        TEST_ASSERT_TRUE(lit <= previous);
        TEST_ASSERT_TRUE_MESSAGE(previous - lit <= 2, "the bar jumped");
        previous = lit;
    }
}

static void test_only_the_led_at_the_end_is_partly_lit() {
    for (uint16_t permille = 0; permille <= FUSE_PERMILLE_MAX; permille += 7) {
        const uint32_t edge = fuseEdge(permille, MAX_LEVEL);
        uint8_t partly = 0;
        for (uint8_t d = 0; d <= MAX_LEVEL; d++) {
            const uint8_t fill = fuseFill(edge, d);
            if (fill != 0 && fill != 255) partly++;
            // Lit from the center outward: never a gap in the bar
            if (d > 0) TEST_ASSERT_TRUE(fill <= fuseFill(edge, d - 1));
        }
        TEST_ASSERT_TRUE(partly <= 1);
    }
}

static void test_the_end_of_the_bar_fades_out_evenly() {
    // Full cover is the color itself, none is dark
    TEST_ASSERT_EQUAL_UINT8(205, fuseChannel(205, 255));
    TEST_ASSERT_EQUAL_UINT8(0, fuseChannel(205, 0));
    // Half covered gets a quarter of the light, which reads as about half
    TEST_ASSERT_UINT8_WITHIN(1, 51, fuseChannel(205, 128));
    // And never brighter for less cover
    uint8_t previous = fuseChannel(205, 255);
    for (int16_t fill = 254; fill >= 0; fill--) {
        const uint8_t light = fuseChannel(205, (uint8_t)fill);
        TEST_ASSERT_TRUE(light <= previous);
        previous = light;
    }
}

// ---------------------------------------------------------------------------

void runFuseGaugeTests() {
    RUN_TEST(test_full_time_lights_the_whole_block);
    RUN_TEST(test_no_time_left_keeps_the_center_led_on);
    RUN_TEST(test_more_than_full_time_is_clamped);
    RUN_TEST(test_on_a_whole_led_the_bar_lights_what_the_gauge_does);
    RUN_TEST(test_the_bar_shrinks_without_steps);
    RUN_TEST(test_only_the_led_at_the_end_is_partly_lit);
    RUN_TEST(test_the_end_of_the_bar_fades_out_evenly);
}
