/**
 * @file test_snake_track.cpp
 * @brief Host tests for the LED track (SnakeTrack.h) and the TRACK reply lines.
 *
 * The backend plays Snake on track indices only, so the mapping between a
 * track index and a physical LED is a cross-repo contract: the Pi's
 * `snake_track.py` computes the same thing for its mock board. The lines
 * `TRACK` answers with are parsed by `snake_track.py::TrackCollector`.
 *
 * Built together with test_board_profiles.cpp, which owns main() and pulls in
 * BoardProfiles.cpp; runSnakeTrackTests() is called from there.
 */

#include <unity.h>

#include "BoardProfile.h"
#include "Config.h"
#include "SnakeTrack.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const BoardProfile& profileNamed(const char* slug) {
    const BoardProfile* profile = findBoardProfile(slug);
    TEST_ASSERT_NOT_NULL_MESSAGE(profile, slug);
    return *profile;
}

static uint16_t trackOf(const BoardProfile& profile, StripId strip, uint16_t index) {
    uint16_t t = 0xFFFF;
    TEST_ASSERT_TRUE(stripToTrack(profile, strip, index, t));
    return t;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

static void test_the_track_is_both_strips_long() {
    TEST_ASSERT_EQUAL_UINT16(520, trackLength(profileNamed("v1")));
    TEST_ASSERT_EQUAL_UINT16(520, trackLength(profileNamed("v2")));
    TEST_ASSERT_EQUAL_UINT16(64, trackLength(profileNamed("dev-5hold")));
}

static void test_the_track_runs_strip_2_backwards_then_strip_1() {
    const BoardProfile& v1 = profileNamed("v1");
    TrackPixel pixel;

    // t = 0 is the far end of strip 2 ...
    TEST_ASSERT_TRUE(trackToStrip(v1, 0, pixel));
    TEST_ASSERT_EQUAL(StripId::STRIP2, pixel.strip);
    TEST_ASSERT_EQUAL_UINT16(259, pixel.index);

    // ... its start sits right before the start of strip 1 ...
    TEST_ASSERT_TRUE(trackToStrip(v1, 259, pixel));
    TEST_ASSERT_EQUAL(StripId::STRIP2, pixel.strip);
    TEST_ASSERT_EQUAL_UINT16(0, pixel.index);
    TEST_ASSERT_TRUE(trackToStrip(v1, 260, pixel));
    TEST_ASSERT_EQUAL(StripId::STRIP1, pixel.strip);
    TEST_ASSERT_EQUAL_UINT16(0, pixel.index);

    // ... and the last index is the far end of strip 1.
    TEST_ASSERT_TRUE(trackToStrip(v1, 519, pixel));
    TEST_ASSERT_EQUAL(StripId::STRIP1, pixel.strip);
    TEST_ASSERT_EQUAL_UINT16(259, pixel.index);
}

static void test_indices_past_the_track_are_rejected() {
    const BoardProfile& v1 = profileNamed("v1");
    TrackPixel pixel;
    uint16_t t;

    TEST_ASSERT_FALSE(trackToStrip(v1, 520, pixel));
    TEST_ASSERT_FALSE(trackToStrip(v1, 0xFFFF, pixel));
    TEST_ASSERT_FALSE(stripToTrack(v1, StripId::STRIP1, 260, t));
    TEST_ASSERT_FALSE(stripToTrack(v1, StripId::STRIP2, 260, t));
}

static void test_a_profile_without_strip_2_is_only_strip_1() {
    const BoardProfile& dev = profileNamed("dev-5hold");
    TrackPixel pixel;
    uint16_t t;

    TEST_ASSERT_TRUE(trackToStrip(dev, 0, pixel));
    TEST_ASSERT_EQUAL(StripId::STRIP1, pixel.strip);
    TEST_ASSERT_EQUAL_UINT16(0, pixel.index);
    TEST_ASSERT_TRUE(trackToStrip(dev, 63, pixel));
    TEST_ASSERT_EQUAL(StripId::STRIP1, pixel.strip);
    TEST_ASSERT_EQUAL_UINT16(63, pixel.index);
    TEST_ASSERT_FALSE(trackToStrip(dev, 64, pixel));

    // Nothing on the track maps to the missing strip.
    TEST_ASSERT_FALSE(stripToTrack(dev, StripId::STRIP2, 0, t));
}

static void test_every_track_index_round_trips() {
    for (uint8_t p = 0; p < boardProfileCount(); p++) {
        const BoardProfile& profile = boardProfileAt(p);
        const uint16_t length = trackLength(profile);
        for (uint16_t t = 0; t < length; t++) {
            TrackPixel pixel;
            TEST_ASSERT_TRUE(trackToStrip(profile, t, pixel));
            TEST_ASSERT_EQUAL_UINT16(t, trackOf(profile, pixel.strip, pixel.index));
        }
    }
}

static void test_a_strip_span_is_clipped_and_flipped_onto_the_track() {
    const BoardProfile& v1 = profileNamed("v1");
    uint16_t lo, hi;

    // Strip 1 keeps its direction.
    TEST_ASSERT_TRUE(stripSpanToTrack(v1, StripId::STRIP1, 10, 14, lo, hi));
    TEST_ASSERT_EQUAL_UINT16(270, lo);
    TEST_ASSERT_EQUAL_UINT16(274, hi);

    // Strip 2 runs backwards: its higher indices come first.
    TEST_ASSERT_TRUE(stripSpanToTrack(v1, StripId::STRIP2, 10, 14, lo, hi));
    TEST_ASSERT_EQUAL_UINT16(245, lo);
    TEST_ASSERT_EQUAL_UINT16(249, hi);

    // A block hanging off the strip's start is clipped to what exists.
    TEST_ASSERT_TRUE(stripSpanToTrack(v1, StripId::STRIP1, -3, 1, lo, hi));
    TEST_ASSERT_EQUAL_UINT16(260, lo);
    TEST_ASSERT_EQUAL_UINT16(261, hi);

    // Entirely past the end: nothing.
    TEST_ASSERT_FALSE(stripSpanToTrack(v1, StripId::STRIP1, 300, 305, lo, hi));
    TEST_ASSERT_FALSE(stripSpanToTrack(profileNamed("dev-5hold"), StripId::STRIP2, 0, 4, lo, hi));
}

// ---------------------------------------------------------------------------
// Holds on the track
// ---------------------------------------------------------------------------

static void test_v1_holds_sit_where_their_leds_are() {
    const BoardProfile& v1 = profileNamed("v1");
    uint16_t t[2];

    // H01 = STRIP2 216 -> 260 - 1 - 216
    TEST_ASSERT_EQUAL_UINT8(1, holdTrackIndices(v1, 0, t));
    TEST_ASSERT_EQUAL_UINT16(43, t[0]);

    // H05 = STRIP1 193 -> 260 + 193
    TEST_ASSERT_EQUAL_UINT8(1, holdTrackIndices(v1, 4, t));
    TEST_ASSERT_EQUAL_UINT16(453, t[0]);
}

static void test_v1_mirrored_holds_report_both_leds() {
    const BoardProfile& v1 = profileNamed("v1");
    uint16_t t[2];

    // H04 = STRIP2 184, mirrored onto STRIP1 184
    TEST_ASSERT_EQUAL_UINT8(2, holdTrackIndices(v1, 3, t));
    TEST_ASSERT_EQUAL_UINT16(75, t[0]);
    TEST_ASSERT_EQUAL_UINT16(444, t[1]);

    // H19 = STRIP2 90, mirrored onto STRIP1 90
    TEST_ASSERT_EQUAL_UINT8(2, holdTrackIndices(v1, 18, t));
    TEST_ASSERT_EQUAL_UINT16(169, t[0]);
    TEST_ASSERT_EQUAL_UINT16(350, t[1]);
}

static void test_v2_holds_sit_where_their_leds_are() {
    const BoardProfile& v2 = profileNamed("v2");
    uint16_t t[2];

    // H01 = STRIP1 225
    TEST_ASSERT_EQUAL_UINT8(1, holdTrackIndices(v2, 0, t));
    TEST_ASSERT_EQUAL_UINT16(485, t[0]);

    // H04 = STRIP1 190, mirrored onto STRIP2 189 — primary first
    TEST_ASSERT_EQUAL_UINT8(2, holdTrackIndices(v2, 3, t));
    TEST_ASSERT_EQUAL_UINT16(450, t[0]);
    TEST_ASSERT_EQUAL_UINT16(70, t[1]);

    // H19 is mirrored on v1 only
    TEST_ASSERT_EQUAL_UINT8(1, holdTrackIndices(v2, 18, t));
}

static void test_dev_5hold_holds_are_on_strip_1() {
    const BoardProfile& dev = profileNamed("dev-5hold");
    uint16_t t[2];
    const uint16_t expected[] = {39, 31, 23, 3, 59};

    for (uint8_t hold = 0; hold < dev.holdCount; hold++) {
        TEST_ASSERT_EQUAL_UINT8(1, holdTrackIndices(dev, hold, t));
        TEST_ASSERT_EQUAL_UINT16(expected[hold], t[0]);
    }
}

static void test_every_hold_of_every_profile_is_on_its_track() {
    for (uint8_t p = 0; p < boardProfileCount(); p++) {
        const BoardProfile& profile = boardProfileAt(p);
        for (uint8_t hold = 0; hold < profile.holdCount; hold++) {
            uint16_t t[2];
            uint8_t count = holdTrackIndices(profile, hold, t);
            TEST_ASSERT_GREATER_OR_EQUAL_UINT8(1, count);
            for (uint8_t i = 0; i < count; i++) {
                TEST_ASSERT_LESS_THAN_UINT16(trackLength(profile), t[i]);
            }
        }
    }
}

static void test_a_hold_the_profile_lacks_has_no_index() {
    uint16_t t[2];
    TEST_ASSERT_EQUAL_UINT8(0, holdTrackIndices(profileNamed("dev-5hold"), 5, t));
}

// ---------------------------------------------------------------------------
// TRACK reply — parsed by the Pi, so its shape is a contract
// ---------------------------------------------------------------------------

static void test_the_header_names_length_and_width() {
    char line[EVENT_MESSAGE_BUFFER_SIZE];

    buildTrackHeaderLine(line, sizeof(line), profileNamed("v1"));
    TEST_ASSERT_EQUAL_STRING("TRACK length=520 width=5", line);

    buildTrackHeaderLine(line, sizeof(line), profileNamed("dev-5hold"));
    TEST_ASSERT_EQUAL_STRING("TRACK length=64 width=1", line);
}

static void test_a_hold_line_lists_its_indices() {
    char line[EVENT_MESSAGE_BUFFER_SIZE];
    const BoardProfile& v1 = profileNamed("v1");

    buildTrackHoldLine(line, sizeof(line), v1, 0);
    TEST_ASSERT_EQUAL_STRING("TRACK_HOLD H01 43", line);

    buildTrackHoldLine(line, sizeof(line), v1, 3);
    TEST_ASSERT_EQUAL_STRING("TRACK_HOLD H04 75,444", line);

    buildTrackHoldLine(line, sizeof(line), v1, 33);
    TEST_ASSERT_EQUAL_STRING("TRACK_HOLD H34 290", line);
}

static void test_a_hold_line_for_a_missing_hold_is_empty() {
    char line[EVENT_MESSAGE_BUFFER_SIZE] = "garbage";
    TEST_ASSERT_EQUAL_UINT(0, buildTrackHoldLine(line, sizeof(line), profileNamed("dev-5hold"), 9));
    TEST_ASSERT_EQUAL_STRING("", line);
}

static void test_the_track_lines_truncate_instead_of_overflowing() {
    char guarded[9];
    guarded[8] = 0x7f;

    size_t length = buildTrackHoldLine(guarded, 8, profileNamed("v1"), 3);
    TEST_ASSERT_EQUAL_UINT(7, length);
    TEST_ASSERT_EQUAL_STRING("TRACK_H", guarded);
    TEST_ASSERT_EQUAL_CHAR(0x7f, guarded[8]);

    length = buildTrackHeaderLine(guarded, 8, profileNamed("v1"));
    TEST_ASSERT_EQUAL_UINT(7, length);
    TEST_ASSERT_EQUAL_CHAR(0x7f, guarded[8]);
}

static void test_info_announces_the_snake_feature() {
    char line[EVENT_MESSAGE_BUFFER_SIZE];
    buildBoardInfoLine(line, sizeof(line), false);
    TEST_ASSERT_NOT_NULL(strstr(line, " features=snake"));
    TEST_ASSERT_NOT_NULL(strstr(line, "holdgauge"));
    buildBoardInfoLine(line, sizeof(line), true);
    TEST_ASSERT_NOT_NULL(strstr(line, " features=snake"));
    TEST_ASSERT_NOT_NULL(strstr(line, "holdgauge"));
}

// ---------------------------------------------------------------------------

void runSnakeTrackTests() {
    RUN_TEST(test_the_track_is_both_strips_long);
    RUN_TEST(test_the_track_runs_strip_2_backwards_then_strip_1);
    RUN_TEST(test_indices_past_the_track_are_rejected);
    RUN_TEST(test_a_profile_without_strip_2_is_only_strip_1);
    RUN_TEST(test_every_track_index_round_trips);
    RUN_TEST(test_a_strip_span_is_clipped_and_flipped_onto_the_track);

    RUN_TEST(test_v1_holds_sit_where_their_leds_are);
    RUN_TEST(test_v1_mirrored_holds_report_both_leds);
    RUN_TEST(test_v2_holds_sit_where_their_leds_are);
    RUN_TEST(test_dev_5hold_holds_are_on_strip_1);
    RUN_TEST(test_every_hold_of_every_profile_is_on_its_track);
    RUN_TEST(test_a_hold_the_profile_lacks_has_no_index);

    RUN_TEST(test_the_header_names_length_and_width);
    RUN_TEST(test_a_hold_line_lists_its_indices);
    RUN_TEST(test_a_hold_line_for_a_missing_hold_is_empty);
    RUN_TEST(test_the_track_lines_truncate_instead_of_overflowing);
    RUN_TEST(test_info_announces_the_snake_feature);
}
