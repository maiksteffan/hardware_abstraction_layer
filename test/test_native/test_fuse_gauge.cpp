/**
 * @file test_fuse_gauge.cpp
 * @brief Host tests for the fuse geometry (FuseGauge.h).
 *
 * `FUSE <pos> <0-1000>` is how the game Fuse shows the next hold: the Pi sends
 * the time left about every 40 ms, and the firmware shrinks the hold's block
 * towards its center without steps, with a glowing spark at the burning end.
 *
 * Built together with test_board_profiles.cpp, which owns main();
 * runFuseGaugeTests() is called from there.
 */

#include <unity.h>

#include "Config.h"
#include "FuseGauge.h"
#include "HoldGauge.h"

// A 5-LED core with its rings, as on v1/v2: 6 LEDs either side of the center.
static constexpr uint8_t MAX_LEVEL = 6;

static uint16_t litOnOneSide(uint16_t permille) {
    const uint32_t edge = fuseEdge(permille, MAX_LEVEL);
    uint16_t sum = 0;
    for (uint8_t d = 0; d <= MAX_LEVEL; d++) sum += fuseFill(edge, d);
    return sum;
}

static void test_full_time_lights_the_whole_block_with_the_spark_at_its_ends() {
    const uint32_t edge = fuseEdge(FUSE_PERMILLE_MAX, MAX_LEVEL);
    for (uint8_t d = 0; d <= MAX_LEVEL; d++) {
        TEST_ASSERT_EQUAL_UINT8(255, fuseFill(edge, d));
    }
    TEST_ASSERT_EQUAL_UINT8(255, fuseSpark(edge, MAX_LEVEL));
    TEST_ASSERT_EQUAL_UINT8(0, fuseSpark(edge, MAX_LEVEL - 1));
}

static void test_no_time_left_leaves_the_center_glowing() {
    // The hold is still the active one; the Pi switches it off.
    const uint32_t edge = fuseEdge(0, MAX_LEVEL);
    TEST_ASSERT_EQUAL_UINT8(255, fuseFill(edge, 0));
    TEST_ASSERT_EQUAL_UINT8(255, fuseSpark(edge, 0));
    for (uint8_t d = 1; d <= MAX_LEVEL; d++) {
        TEST_ASSERT_EQUAL_UINT8(0, fuseFill(edge, d));
    }
}

static void test_more_than_full_time_is_clamped() {
    TEST_ASSERT_EQUAL_UINT32(fuseEdge(FUSE_PERMILLE_MAX, MAX_LEVEL), fuseEdge(9999, MAX_LEVEL));
}

static void test_on_a_whole_led_the_fuse_lights_what_the_gauge_does() {
    // Where the burning end sits exactly between two LEDs, the fuse covers the
    // same LEDs as the stepped gauge at that level.
    for (uint8_t level = 0; level <= MAX_LEVEL; level++) {
        const uint32_t edge = ((uint32_t)level + 1u) * 1000u;
        for (uint8_t d = 0; d <= MAX_LEVEL; d++) {
            TEST_ASSERT_EQUAL_UINT8(d <= level ? 255 : 0, fuseFill(edge, d));
        }
    }
}

static void test_the_fuse_shrinks_without_steps() {
    // One per mille less never takes away more than a sliver of light.
    uint16_t previous = litOnOneSide(FUSE_PERMILLE_MAX);
    for (int16_t permille = FUSE_PERMILLE_MAX - 1; permille >= 0; permille--) {
        const uint16_t lit = litOnOneSide((uint16_t)permille);
        TEST_ASSERT_TRUE(lit <= previous);
        TEST_ASSERT_TRUE_MESSAGE(previous - lit <= 2, "the fuse jumped");
        previous = lit;
    }
}

static void test_the_spark_slides_without_jumping() {
    for (uint8_t d = 0; d <= MAX_LEVEL; d++) {
        uint8_t previous = fuseSpark(fuseEdge(FUSE_PERMILLE_MAX, MAX_LEVEL), d);
        for (int16_t permille = FUSE_PERMILLE_MAX - 1; permille >= 0; permille--) {
            const uint8_t spark = fuseSpark(fuseEdge((uint16_t)permille, MAX_LEVEL), d);
            const int16_t step = (int16_t)spark - (int16_t)previous;
            TEST_ASSERT_TRUE_MESSAGE(step >= -2 && step <= 2, "the spark jumped");
            previous = spark;
        }
    }
}

static void test_the_spark_is_always_at_the_burning_end() {
    for (uint16_t permille = 0; permille <= FUSE_PERMILLE_MAX; permille += 7) {
        const uint32_t edge = fuseEdge(permille, MAX_LEVEL);
        uint8_t outermostLit = 0;
        uint8_t brightest = 0;
        for (uint8_t d = 0; d <= MAX_LEVEL; d++) {
            if (fuseFill(edge, d) > 0) outermostLit = d;
            if (fuseSpark(edge, d) > fuseSpark(edge, brightest)) brightest = d;
        }
        // The spark peaks on the outermost lit LED or the one just inside it
        TEST_ASSERT_TRUE(brightest == outermostLit || brightest + 1 == outermostLit);
        TEST_ASSERT_TRUE(fuseSpark(edge, brightest) >= 127);
        // and is nowhere else
        for (uint8_t d = 0; d + 1 < outermostLit; d++) {
            TEST_ASSERT_EQUAL_UINT8(0, fuseSpark(edge, d));
        }
    }
}

static void test_the_color_mixes_base_and_spark_and_fades_with_the_cover() {
    // No spark: the base at full cover
    TEST_ASSERT_EQUAL_UINT8(205, fuseChannel(205, 20, 0, 255, 255));
    // All spark at full flicker: the spark color
    TEST_ASSERT_EQUAL_UINT8(255, fuseChannel(80, 255, 255, 255, 255));
    // A flicker dims only the spark
    TEST_ASSERT_EQUAL_UINT8(127, fuseChannel(80, 255, 255, 127, 255));
    TEST_ASSERT_EQUAL_UINT8(205, fuseChannel(205, 20, 0, 0, 255));
    // Uncovered is dark, half covered a quarter of the light
    TEST_ASSERT_EQUAL_UINT8(0, fuseChannel(205, 255, 255, 255, 0));
    TEST_ASSERT_UINT8_WITHIN(1, 51, fuseChannel(205, 20, 0, 255, 128));
}

static void test_the_flicker_stays_in_its_range_and_varies() {
    uint32_t state = 0x2545F491u;
    uint8_t lowest = 255;
    uint8_t highest = 0;
    for (int i = 0; i < 1000; i++) {
        const uint8_t flicker = fuseFlicker(state, LED_FUSE_FLICKER_MIN);
        TEST_ASSERT_TRUE(flicker >= LED_FUSE_FLICKER_MIN);
        if (flicker < lowest) lowest = flicker;
        if (flicker > highest) highest = flicker;
    }
    TEST_ASSERT_TRUE(state != 0);
    // Spread over most of its range, not stuck
    TEST_ASSERT_TRUE(lowest < LED_FUSE_FLICKER_MIN + 20);
    TEST_ASSERT_TRUE(highest > 235);
}

// ---------------------------------------------------------------------------

void runFuseGaugeTests() {
    RUN_TEST(test_full_time_lights_the_whole_block_with_the_spark_at_its_ends);
    RUN_TEST(test_no_time_left_leaves_the_center_glowing);
    RUN_TEST(test_more_than_full_time_is_clamped);
    RUN_TEST(test_on_a_whole_led_the_fuse_lights_what_the_gauge_does);
    RUN_TEST(test_the_fuse_shrinks_without_steps);
    RUN_TEST(test_the_spark_slides_without_jumping);
    RUN_TEST(test_the_spark_is_always_at_the_burning_end);
    RUN_TEST(test_the_color_mixes_base_and_spark_and_fades_with_the_cover);
    RUN_TEST(test_the_flicker_stays_in_its_range_and_varies);
}
