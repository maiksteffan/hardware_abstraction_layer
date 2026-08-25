/**
 * @file test_board_profiles.cpp
 * @brief Host tests for the board profile registry and the INFO line.
 *
 * Two things here are contracts rather than implementation details:
 *
 *  - An unknown slug must never stop the board from booting. It keeps the
 *    default profile and records the mismatch, because a board that runs the
 *    wrong wiring but says so is recoverable, and one that stays dark is a
 *    drive to the gym.
 *  - The INFO line is parsed by the Pi (`esp32_protocol.py::_handle_info`).
 *    Its shape is a cross-repo contract, so it is asserted here key by key.
 */

#include <unity.h>

#include <stdio.h>

#include "../../src/BoardProfiles.cpp"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static char g_line[EVENT_MESSAGE_BUFFER_SIZE];
static char g_value[EVENT_MESSAGE_BUFFER_SIZE];

static const char* infoLine(bool withSelection) {
    buildBoardInfoLine(g_line, sizeof(g_line), withSelection);
    return g_line;
}

/// Value of ` key=` in an INFO line, or "" when the key is absent.
///
/// Deliberately hand-rolled over C strings: the firmware has no libstdc++, and
/// a parser that only understands what the firmware can emit is the right
/// yardstick for what the firmware emits.
static const char* infoValue(const char* line, const char* key) {
    char needle[64];
    snprintf(needle, sizeof(needle), " %s=", key);

    const char* start = strstr(line, needle);
    if (!start) {
        g_value[0] = '\0';
        return g_value;
    }
    start += strlen(needle);

    const char* end = strchr(start, ' ');
    size_t length = end ? (size_t)(end - start) : strlen(start);
    if (length >= sizeof(g_value)) length = sizeof(g_value) - 1;
    memcpy(g_value, start, length);
    g_value[length] = '\0';
    return g_value;
}

void setUp() {
    // Every test starts from a clean selection.
    selectBoardProfile(defaultBoardProfile().slug);
}

void tearDown() {}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

void test_a_default_profile_is_active_before_anything_is_selected() {
    TEST_ASSERT_GREATER_THAN(0, boardProfileCount());
    TEST_ASSERT_EQUAL_STRING(defaultBoardProfile().slug, activeBoardProfile().slug);
    TEST_ASSERT_FALSE(boardProfileMismatch());
}

void test_every_registered_profile_is_findable_by_its_slug() {
    for (uint8_t i = 0; i < boardProfileCount(); i++) {
        const BoardProfile& profile = boardProfileAt(i);
        const BoardProfile* found = findBoardProfile(profile.slug);
        TEST_ASSERT_NOT_NULL(found);
        TEST_ASSERT_EQUAL_STRING(profile.slug, found->slug);
    }
}

void test_every_profile_is_internally_consistent() {
    for (uint8_t i = 0; i < boardProfileCount(); i++) {
        const BoardProfile& profile = boardProfileAt(i);
        TEST_ASSERT_GREATER_THAN(0, profile.holdCount);
        TEST_ASSERT_LESS_OR_EQUAL(MAX_HOLDS, profile.holdCount);
        TEST_ASSERT_GREATER_THAN(0, profile.sensorCount);
        TEST_ASSERT_LESS_OR_EQUAL(MAX_SENSORS, profile.sensorCount);
        // An even width has no centre LED, so a position could not be centred.
        TEST_ASSERT_TRUE(profile.ledPositionWidth % 2 == 1);
        TEST_ASSERT_LESS_OR_EQUAL(MAX_LED_STRIP_LENGTH, profile.strip1Length);
        TEST_ASSERT_LESS_OR_EQUAL(MAX_LED_STRIP_LENGTH, profile.strip2Length);
    }
}

void test_every_mapping_points_at_hardware_the_profile_has() {
    for (uint8_t i = 0; i < boardProfileCount(); i++) {
        const BoardProfile& profile = boardProfileAt(i);
        for (uint8_t hold = 0; hold < profile.holdCount; hold++) {
            TEST_ASSERT_LESS_THAN(profile.sensorCount, profile.inputMappings[hold].sensorIndex);
            TEST_ASSERT_LESS_THAN(TOUCH_CHANNELS_PER_SENSOR, profile.inputMappings[hold].channel);

            const LedMapping& led = profile.ledMappings[hold];
            uint16_t length = (led.strip == StripId::STRIP1) ? profile.strip1Length
                                                             : profile.strip2Length;
            TEST_ASSERT_LESS_THAN(length, led.index);
        }
        for (uint8_t m = 0; m < profile.ledMirrorCount; m++) {
            TEST_ASSERT_LESS_THAN(profile.holdCount, profile.ledMirrors[m].position);
        }
    }
}

void test_no_two_holds_share_a_sensor_channel() {
    for (uint8_t i = 0; i < boardProfileCount(); i++) {
        const BoardProfile& profile = boardProfileAt(i);
        for (uint8_t a = 0; a < profile.holdCount; a++) {
            for (uint8_t b = (uint8_t)(a + 1); b < profile.holdCount; b++) {
                bool same = profile.inputMappings[a].sensorIndex
                                == profile.inputMappings[b].sensorIndex
                            && profile.inputMappings[a].channel
                                   == profile.inputMappings[b].channel;
                TEST_ASSERT_FALSE_MESSAGE(same, "two holds wired to the same channel");
            }
        }
    }
}

void test_slugs_are_unique() {
    for (uint8_t a = 0; a < boardProfileCount(); a++) {
        for (uint8_t b = (uint8_t)(a + 1); b < boardProfileCount(); b++) {
            TEST_ASSERT_TRUE(strcmp(boardProfileAt(a).slug, boardProfileAt(b).slug) != 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Selection and fallback
// ---------------------------------------------------------------------------

void test_selecting_a_known_slug_activates_it() {
    const char* slug = boardProfileAt(0).slug;

    TEST_ASSERT_TRUE(selectBoardProfile(slug));
    TEST_ASSERT_EQUAL_STRING(slug, activeBoardProfile().slug);
    TEST_ASSERT_FALSE(boardProfileMismatch());
    TEST_ASSERT_EQUAL_STRING("", requestedBoardProfile());
}

void test_an_unknown_slug_keeps_the_board_running_on_the_default() {
    TEST_ASSERT_FALSE(selectBoardProfile("does-not-exist"));

    TEST_ASSERT_EQUAL_STRING(defaultBoardProfile().slug, activeBoardProfile().slug);
    TEST_ASSERT_TRUE(boardProfileMismatch());
    TEST_ASSERT_EQUAL_STRING("does-not-exist", requestedBoardProfile());
}

void test_a_later_known_slug_clears_the_mismatch() {
    selectBoardProfile("does-not-exist");

    TEST_ASSERT_TRUE(selectBoardProfile(defaultBoardProfile().slug));
    TEST_ASSERT_FALSE(boardProfileMismatch());
    TEST_ASSERT_EQUAL_STRING("", requestedBoardProfile());
}

void test_an_over_long_slug_is_truncated_not_overflowed() {
    char slug[BOARD_SLUG_MAX_LENGTH * 2];
    memset(slug, 'x', sizeof(slug) - 1);
    slug[sizeof(slug) - 1] = '\0';

    TEST_ASSERT_FALSE(selectBoardProfile(slug));
    TEST_ASSERT_EQUAL_UINT(BOARD_SLUG_MAX_LENGTH, strlen(requestedBoardProfile()));
}

void test_an_empty_or_null_slug_finds_nothing() {
    TEST_ASSERT_NULL(findBoardProfile(""));
    TEST_ASSERT_NULL(findBoardProfile(nullptr));
}

void test_an_out_of_range_index_falls_back_to_the_default() {
    TEST_ASSERT_EQUAL_STRING(defaultBoardProfile().slug,
                             boardProfileAt(boardProfileCount()).slug);
}

// ---------------------------------------------------------------------------
// INFO line — parsed by the Pi, so its shape is a contract
// ---------------------------------------------------------------------------

void test_the_banner_reports_capabilities_but_no_selection_yet() {
    const char* line = infoLine(false);

    TEST_ASSERT_EQUAL_STRING_LEN("INFO", line, 4);
    TEST_ASSERT_EQUAL_STRING(FIRMWARE_VERSION, infoValue(line, "firmware"));
    TEST_ASSERT_EQUAL_STRING(PROTOCOL_VERSION, infoValue(line, "protocol"));
    TEST_ASSERT_EQUAL_STRING(BOARD_TYPE, infoValue(line, "board"));
    TEST_ASSERT_EQUAL_STRING(defaultBoardProfile().slug, infoValue(line, "default"));
    // Before the Pi has answered there is nothing to report about a selection.
    TEST_ASSERT_EQUAL_STRING("", infoValue(line, "boardVersion"));
    TEST_ASSERT_EQUAL_STRING("", infoValue(line, "holds"));
}

void test_the_banner_lists_every_profile_comma_separated() {
    char profiles[EVENT_MESSAGE_BUFFER_SIZE];
    snprintf(profiles, sizeof(profiles), "%s", infoValue(infoLine(false), "profiles"));

    for (uint8_t i = 0; i < boardProfileCount(); i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(profiles, boardProfileAt(i).slug),
                                     "a registered profile is missing from INFO");
    }
    // One separator fewer than there are entries.
    size_t commas = 0;
    for (const char* c = profiles; *c; c++) if (*c == ',') commas++;
    TEST_ASSERT_EQUAL_UINT(boardProfileCount() - 1, commas);
}

void test_the_resolved_line_reports_the_active_profile_and_its_hold_count() {
    selectBoardProfile(defaultBoardProfile().slug);
    const char* line = infoLine(true);
    char holds[8];
    snprintf(holds, sizeof(holds), "%u", (unsigned)activeBoardProfile().holdCount);

    TEST_ASSERT_EQUAL_STRING(activeBoardProfile().slug, infoValue(line, "boardVersion"));
    TEST_ASSERT_EQUAL_STRING(holds, infoValue(line, "holds"));
    TEST_ASSERT_EQUAL_STRING("", infoValue(line, "mismatch"));
}

void test_the_resolved_line_reports_a_mismatch() {
    selectBoardProfile("nope");
    const char* line = infoLine(true);

    TEST_ASSERT_EQUAL_STRING("nope", infoValue(line, "requested"));
    TEST_ASSERT_EQUAL_STRING("1", infoValue(line, "mismatch"));
    // Still reports what it actually runs, not what was asked for.
    TEST_ASSERT_EQUAL_STRING(defaultBoardProfile().slug, infoValue(line, "boardVersion"));
}

void test_the_line_fits_the_event_buffer() {
    selectBoardProfile("nope");  // longest variant: adds requested= and mismatch=
    char buffer[EVENT_MESSAGE_BUFFER_SIZE];
    size_t length = buildBoardInfoLine(buffer, sizeof(buffer), true);

    TEST_ASSERT_LESS_THAN(EVENT_MESSAGE_BUFFER_SIZE, length);
    TEST_ASSERT_EQUAL_UINT(length, strlen(buffer));
}

void test_a_short_buffer_truncates_instead_of_overflowing() {
    char buffer[16];
    const char canary = 0x7f;
    char guarded[sizeof(buffer) + 1];
    guarded[sizeof(buffer)] = canary;

    size_t length = buildBoardInfoLine(guarded, sizeof(buffer), true);

    TEST_ASSERT_LESS_THAN(sizeof(buffer), length);
    TEST_ASSERT_EQUAL_CHAR(canary, guarded[sizeof(buffer)]);
    TEST_ASSERT_EQUAL_UINT(length, strlen(guarded));
}

void test_a_zero_length_buffer_writes_nothing() {
    char buffer[1] = {'A'};
    TEST_ASSERT_EQUAL_UINT(0, buildBoardInfoLine(buffer, 0, true));
    TEST_ASSERT_EQUAL_CHAR('A', buffer[0]);
}

// ---------------------------------------------------------------------------

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_a_default_profile_is_active_before_anything_is_selected);
    RUN_TEST(test_every_registered_profile_is_findable_by_its_slug);
    RUN_TEST(test_every_profile_is_internally_consistent);
    RUN_TEST(test_every_mapping_points_at_hardware_the_profile_has);
    RUN_TEST(test_no_two_holds_share_a_sensor_channel);
    RUN_TEST(test_slugs_are_unique);

    RUN_TEST(test_selecting_a_known_slug_activates_it);
    RUN_TEST(test_an_unknown_slug_keeps_the_board_running_on_the_default);
    RUN_TEST(test_a_later_known_slug_clears_the_mismatch);
    RUN_TEST(test_an_over_long_slug_is_truncated_not_overflowed);
    RUN_TEST(test_an_empty_or_null_slug_finds_nothing);
    RUN_TEST(test_an_out_of_range_index_falls_back_to_the_default);

    RUN_TEST(test_the_banner_reports_capabilities_but_no_selection_yet);
    RUN_TEST(test_the_banner_lists_every_profile_comma_separated);
    RUN_TEST(test_the_resolved_line_reports_the_active_profile_and_its_hold_count);
    RUN_TEST(test_the_resolved_line_reports_a_mismatch);
    RUN_TEST(test_the_line_fits_the_event_buffer);
    RUN_TEST(test_a_short_buffer_truncates_instead_of_overflowing);
    RUN_TEST(test_a_zero_length_buffer_writes_nothing);

    return UNITY_END();
}
