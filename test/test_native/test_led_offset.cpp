/**
 * @file test_led_offset.cpp
 * @brief Host tests for the per-board LED offset (LedOffset.h, LED_OFFSET).
 *
 * The offset is this board's own correction of where a hold's block sits. The
 * Pi stores it and counts it in track LEDs, the firmware adds it to the
 * profile's index — so which way "+1" goes on each strip, and what TRACK
 * reports for a moved hold, are contracts between the two repos.
 *
 * Built together with test_board_profiles.cpp, which owns main() and pulls in
 * BoardProfiles.cpp; runLedOffsetTests() is called from there.
 */

#include <unity.h>

#include "BoardProfile.h"
#include "Config.h"
#include "LedOffset.h"
#include "SnakeTrack.h"

static const BoardProfile& offsetProfile(const char* slug) {
    const BoardProfile* profile = findBoardProfile(slug);
    TEST_ASSERT_NOT_NULL_MESSAGE(profile, slug);
    return *profile;
}

// ---------------------------------------------------------------------------
// Direction
// ---------------------------------------------------------------------------

static void test_a_step_along_the_track_is_up_on_strip_1_and_down_on_strip_2() {
    TEST_ASSERT_EQUAL_INT32(103, shiftAlongTrack(StripId::STRIP1, 100, 3));
    TEST_ASSERT_EQUAL_INT32(97, shiftAlongTrack(StripId::STRIP1, 100, -3));
    // Strip 2 runs backwards on the track.
    TEST_ASSERT_EQUAL_INT32(97, shiftAlongTrack(StripId::STRIP2, 100, 3));
    TEST_ASSERT_EQUAL_INT32(103, shiftAlongTrack(StripId::STRIP2, 100, -3));
    TEST_ASSERT_EQUAL_INT32(100, shiftAlongTrack(StripId::STRIP2, 100, 0));
}

static void test_a_shift_may_leave_the_strip_and_says_so() {
    // Callers clip or refuse; the arithmetic itself must not wrap.
    TEST_ASSERT_EQUAL_INT32(-2, shiftAlongTrack(StripId::STRIP1, 3, -5));
    TEST_ASSERT_EQUAL_INT32(-2, shiftAlongTrack(StripId::STRIP2, 3, 5));

    uint16_t t = 0;
    const BoardProfile& dev = offsetProfile("dev-5hold");
    TEST_ASSERT_FALSE(shiftedLedToTrack(dev, StripId::STRIP1, 3, -5, t));
    TEST_ASSERT_FALSE(shiftedLedToTrack(dev, StripId::STRIP1, 62, 5, t));
}

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------

static void test_every_hold_starts_at_its_profile_position() {
    LedOffsets offsets;
    for (uint8_t hold = 0; hold < MAX_HOLDS; hold++) {
        TEST_ASSERT_EQUAL_INT8(0, offsets.get(hold));
    }
    // Past the ceiling there is nothing to read, and nothing to read wrongly.
    TEST_ASSERT_EQUAL_INT8(0, offsets.get(MAX_HOLDS));
    TEST_ASSERT_EQUAL_INT8(0, offsets.get(255));
}

static void test_an_offset_is_kept_per_hold() {
    const BoardProfile& v2 = offsetProfile("v2");
    LedOffsets offsets;

    TEST_ASSERT_TRUE(offsets.set(v2, 4, -2));
    TEST_ASSERT_TRUE(offsets.set(v2, 20, 3));

    TEST_ASSERT_EQUAL_INT8(-2, offsets.get(4));
    TEST_ASSERT_EQUAL_INT8(3, offsets.get(20));
    TEST_ASSERT_EQUAL_INT8(0, offsets.get(5));

    // Zero is a value like any other: it puts the hold back on its profile LED.
    TEST_ASSERT_TRUE(offsets.set(v2, 4, 0));
    TEST_ASSERT_EQUAL_INT8(0, offsets.get(4));
}

static void test_clearing_returns_every_hold_to_the_profile() {
    const BoardProfile& v2 = offsetProfile("v2");
    LedOffsets offsets;
    offsets.set(v2, 0, 5);
    offsets.set(v2, 34, -5);

    offsets.clear();

    TEST_ASSERT_EQUAL_INT8(0, offsets.get(0));
    TEST_ASSERT_EQUAL_INT8(0, offsets.get(34));
}

static void test_an_offset_beyond_the_bound_is_refused() {
    const BoardProfile& v2 = offsetProfile("v2");
    LedOffsets offsets;
    offsets.set(v2, 4, 2);

    TEST_ASSERT_TRUE(LedOffsets::accepts(v2, 4, LED_OFFSET_MAX));
    TEST_ASSERT_TRUE(LedOffsets::accepts(v2, 4, -LED_OFFSET_MAX));
    TEST_ASSERT_FALSE(offsets.set(v2, 4, LED_OFFSET_MAX + 1));
    TEST_ASSERT_FALSE(offsets.set(v2, 4, -LED_OFFSET_MAX - 1));
    // A refused offset leaves the last good one in place.
    TEST_ASSERT_EQUAL_INT8(2, offsets.get(4));
}

static void test_a_hold_the_profile_lacks_takes_no_offset() {
    const BoardProfile& dev = offsetProfile("dev-5hold");
    LedOffsets offsets;

    TEST_ASSERT_FALSE(offsets.set(dev, 5, 1));
    TEST_ASSERT_FALSE(offsets.set(dev, 255, 0));
    TEST_ASSERT_EQUAL_INT8(0, offsets.get(5));
}

static void test_an_offset_that_leaves_the_strip_is_refused() {
    // dev-5hold H04 sits on STRIP1 index 3: four LEDs back is index -1.
    const BoardProfile& dev = offsetProfile("dev-5hold");
    TEST_ASSERT_TRUE(LedOffsets::accepts(dev, 3, -3));
    TEST_ASSERT_FALSE(LedOffsets::accepts(dev, 3, -4));
    // H05 sits on index 59 of 64: four LEDs on is the last LED, five is past it.
    TEST_ASSERT_TRUE(LedOffsets::accepts(dev, 4, 4));
    TEST_ASSERT_FALSE(LedOffsets::accepts(dev, 4, 5));
}

static void test_every_accepted_offset_keeps_every_hold_on_its_track() {
    for (uint8_t p = 0; p < boardProfileCount(); p++) {
        const BoardProfile& profile = boardProfileAt(p);
        for (uint8_t hold = 0; hold < profile.holdCount; hold++) {
            uint16_t base[2];
            const uint8_t leds = holdTrackIndices(profile, hold, base);

            for (int16_t offset = -LED_OFFSET_MAX; offset <= LED_OFFSET_MAX; offset++) {
                if (!LedOffsets::accepts(profile, hold, offset)) continue;
                uint16_t t[2];
                // Accepted means no LED of the hold is lost: the mirror stays too.
                TEST_ASSERT_EQUAL_UINT8(leds, holdTrackIndices(profile, hold, t, (int8_t)offset));
                for (uint8_t i = 0; i < leds; i++) {
                    TEST_ASSERT_LESS_THAN_UINT16(trackLength(profile), t[i]);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// TRACK — what the Pi's snake sees
// ---------------------------------------------------------------------------

static void test_a_holds_track_index_moves_by_its_offset() {
    const BoardProfile& v1 = offsetProfile("v1");
    uint16_t t[2];

    // H01 = STRIP2 216 -> t 43
    TEST_ASSERT_EQUAL_UINT8(1, holdTrackIndices(v1, 0, t, 2));
    TEST_ASSERT_EQUAL_UINT16(45, t[0]);
    // H34 = STRIP1 30 -> t 290
    TEST_ASSERT_EQUAL_UINT8(1, holdTrackIndices(v1, 33, t, -3));
    TEST_ASSERT_EQUAL_UINT16(287, t[0]);
}

static void test_a_mirrored_holds_two_leds_move_the_same_way_along_the_track() {
    const BoardProfile& v1 = offsetProfile("v1");
    uint16_t t[2];

    // H04 = STRIP2 184 (t 75), mirrored onto STRIP1 184 (t 444). The strips run
    // in opposite directions past the hold; along the track both blocks move
    // together, which is the same direction on the wall.
    TEST_ASSERT_EQUAL_UINT8(2, holdTrackIndices(v1, 3, t, 2));
    TEST_ASSERT_EQUAL_UINT16(77, t[0]);
    TEST_ASSERT_EQUAL_UINT16(446, t[1]);

    // ... which is index 182 on strip 2 and index 186 on strip 1.
    TEST_ASSERT_EQUAL_INT32(182, shiftAlongTrack(StripId::STRIP2, 184, 2));
    TEST_ASSERT_EQUAL_INT32(186, shiftAlongTrack(StripId::STRIP1, 184, 2));
}

static void test_the_track_line_reports_the_moved_hold() {
    char line[EVENT_MESSAGE_BUFFER_SIZE];
    const BoardProfile& v1 = offsetProfile("v1");

    buildTrackHoldLine(line, sizeof(line), v1, 0, -1);
    TEST_ASSERT_EQUAL_STRING("TRACK_HOLD H01 42", line);

    buildTrackHoldLine(line, sizeof(line), v1, 3, 2);
    TEST_ASSERT_EQUAL_STRING("TRACK_HOLD H04 77,446", line);

    // No offset is the line every older firmware sends.
    buildTrackHoldLine(line, sizeof(line), v1, 3, 0);
    TEST_ASSERT_EQUAL_STRING("TRACK_HOLD H04 75,444", line);
}

static void test_info_announces_the_led_offset_feature() {
    char line[EVENT_MESSAGE_BUFFER_SIZE];
    buildBoardInfoLine(line, sizeof(line), false);
    TEST_ASSERT_NOT_NULL(strstr(line, "ledoffset"));
    buildBoardInfoLine(line, sizeof(line), true);
    TEST_ASSERT_NOT_NULL(strstr(line, "ledoffset"));
}

// ---------------------------------------------------------------------------

void runLedOffsetTests() {
    RUN_TEST(test_a_step_along_the_track_is_up_on_strip_1_and_down_on_strip_2);
    RUN_TEST(test_a_shift_may_leave_the_strip_and_says_so);

    RUN_TEST(test_every_hold_starts_at_its_profile_position);
    RUN_TEST(test_an_offset_is_kept_per_hold);
    RUN_TEST(test_clearing_returns_every_hold_to_the_profile);
    RUN_TEST(test_an_offset_beyond_the_bound_is_refused);
    RUN_TEST(test_a_hold_the_profile_lacks_takes_no_offset);
    RUN_TEST(test_an_offset_that_leaves_the_strip_is_refused);
    RUN_TEST(test_every_accepted_offset_keeps_every_hold_on_its_track);

    RUN_TEST(test_a_holds_track_index_moves_by_its_offset);
    RUN_TEST(test_a_mirrored_holds_two_leds_move_the_same_way_along_the_track);
    RUN_TEST(test_the_track_line_reports_the_moved_hold);
    RUN_TEST(test_info_announces_the_led_offset_feature);
}
