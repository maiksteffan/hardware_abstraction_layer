/**
 * @file BoardProfiles.cpp
 * @brief The board profiles this firmware ships, and the active selection.
 *
 * Everything below is wiring data lifted out of Config.h and LedController.cpp
 * so that one binary can drive more than one board build. To add a board
 * version: add the four tables, add a PROFILES[] entry, done — no new build
 * environment, no branch.
 */

#include "BoardProfile.h"
#include "Config.h"

#include <stdarg.h>
#include <stdio.h>

// ============================================================================
// Profile "v1" — the 34-hold board this project shipped with
// ============================================================================

// ---- Sensor I2C addresses (one per physical CAP1188 chip) ----
static const uint8_t V1_SENSOR_ADDRESSES[] = {
    0x28, 0x29, 0x2A, 0x2B, 0x2C
};

// ---- Input mapping: H01..H34 -> (sensor index, channel) ----
// sensorIndex indexes V1_SENSOR_ADDRESSES; channel is CAP1188 CS1..CS7 as 0..6.
// Entry i corresponds to input "H{i+1:02}" (so entry 0 is H01).
static const InputMapping V1_INPUT_MAPPINGS[] = {
    // H01..H07
    {1,3}, {1,5}, {1,6}, {2,1}, {2,6}, {3,1}, {3,5},
    // H08..H14
    {1,0}, {1,2}, {1,4}, {2,0}, {2,3}, {3,0}, {3,3},
    // H15..H21
    {3,6}, {1,1}, {0,3}, {2,2}, {2,5}, {4,2}, {3,2},
    // H22..H28
    {3,4}, {0,6}, {0,4}, {0,5}, {4,5}, {4,6}, {4,4},
    // H29..H34 (channel 6 of sensor 4 is unused)
    {0,0}, {0,2}, {0,1}, {4,0}, {4,1}, {4,3}
};

// ---- LED mapping: H01..H34 -> (strip, index) ----
static const LedMapping V1_LED_MAPPINGS[] = {
    { StripId::STRIP2, 216 },  //H01
    { StripId::STRIP2, 204 },  //H02
    { StripId::STRIP2, 193 },  //H03
    { StripId::STRIP2, 184 },  //H04
    { StripId::STRIP1, 193 },  //H05
    { StripId::STRIP1, 204 },  //H06
    { StripId::STRIP1, 216 },  //H07
    { StripId::STRIP2, 141 },  //H08
    { StripId::STRIP2, 152 },  //H09
    { StripId::STRIP2, 164 },  //H10
    { StripId::STRIP2, 172 },  //H11
    { StripId::STRIP1, 171 },  //H12
    { StripId::STRIP1, 163 },  //H13
    { StripId::STRIP1, 152 },  //H14
    { StripId::STRIP1, 141 },  //H15
    { StripId::STRIP2, 122 },  //H16
    { StripId::STRIP2, 110 },  //H17
    { StripId::STRIP2, 100 },  //H18
    { StripId::STRIP2, 90  },  //H19
    { StripId::STRIP1, 99  },  //H20
    { StripId::STRIP1, 111 },  //H21
    { StripId::STRIP1, 122 },  //H22
    { StripId::STRIP2, 49  },  //H23
    { StripId::STRIP2, 60  },  //H24
    { StripId::STRIP2, 72  },  //H25
    { StripId::STRIP1, 72  },  //H26
    { StripId::STRIP1, 60  },  //H27
    { StripId::STRIP1, 49  },  //H28
    { StripId::STRIP2, 30  },  //H29
    { StripId::STRIP2, 18  },  //H30
    { StripId::STRIP2, 7   },  //H31
    { StripId::STRIP1, 7   },  //H32
    { StripId::STRIP1, 18  },  //H33
    { StripId::STRIP1, 30  },  //H34
};

// ---- LED mirrors: positions wired to an LED on both strips ----
// Every command for such a position lights both LEDs automatically.
static const LedMirror V1_LED_MIRRORS[] = {
    { 3,  StripId::STRIP1, 184 },  // H04 -> mirror on strip 1
    { 18, StripId::STRIP1, 90  },  // H19 -> mirror on strip 1
};

// ============================================================================
// Registry
// ============================================================================

#define COUNT_OF(array) ((uint8_t)(sizeof(array) / sizeof((array)[0])))

static const BoardProfile PROFILES[] = {
    {
        /* slug              */ "v1",
        /* holdCount         */ COUNT_OF(V1_INPUT_MAPPINGS),
        /* sensorCount       */ COUNT_OF(V1_SENSOR_ADDRESSES),
        /* sensorAddresses   */ V1_SENSOR_ADDRESSES,
        /* inputMappings     */ V1_INPUT_MAPPINGS,
        /* ledMappings       */ V1_LED_MAPPINGS,
        /* ledMirrors        */ V1_LED_MIRRORS,
        /* ledMirrorCount    */ COUNT_OF(V1_LED_MIRRORS),
        /* ledPositionWidth  */ 5,
        /* strip1Length      */ 260,
        /* strip2Length      */ 260,
    },
};

// Index into PROFILES of the profile used when the Pi says nothing.
static constexpr uint8_t DEFAULT_PROFILE_INDEX = 0;

static const BoardProfile* s_active = &PROFILES[DEFAULT_PROFILE_INDEX];
static char s_requested[BOARD_SLUG_MAX_LENGTH + 1] = "";
static bool s_mismatch = false;

// A profile whose tables overflow the firmware's ceilings would corrupt memory
// silently, so the mismatch is caught at compile time instead.
static_assert(COUNT_OF(V1_INPUT_MAPPINGS) == COUNT_OF(V1_LED_MAPPINGS),
              "v1: input and LED mappings must cover the same holds");
static_assert(COUNT_OF(V1_INPUT_MAPPINGS) <= MAX_HOLDS, "v1: too many holds");
static_assert(COUNT_OF(V1_SENSOR_ADDRESSES) <= MAX_SENSORS, "v1: too many sensors");

uint8_t boardProfileCount() {
    return COUNT_OF(PROFILES);
}

const BoardProfile& boardProfileAt(uint8_t index) {
    if (index >= COUNT_OF(PROFILES)) return PROFILES[DEFAULT_PROFILE_INDEX];
    return PROFILES[index];
}

const BoardProfile* findBoardProfile(const char* slug) {
    if (!slug || slug[0] == '\0') return nullptr;
    for (uint8_t i = 0; i < COUNT_OF(PROFILES); i++) {
        if (strcmp(PROFILES[i].slug, slug) == 0) return &PROFILES[i];
    }
    return nullptr;
}

const BoardProfile& defaultBoardProfile() {
    return PROFILES[DEFAULT_PROFILE_INDEX];
}

const BoardProfile& activeBoardProfile() {
    return *s_active;
}

bool selectBoardProfile(const char* slug) {
    const BoardProfile* profile = findBoardProfile(slug);
    if (profile) {
        s_active = profile;
        s_requested[0] = '\0';
        s_mismatch = false;
        return true;
    }

    // Unknown slug: stay on the default and remember what was asked for, so
    // INFO can report the mismatch and the Pi can prompt a firmware update.
    s_active = &PROFILES[DEFAULT_PROFILE_INDEX];
    if (slug) {
        strncpy(s_requested, slug, BOARD_SLUG_MAX_LENGTH);
        s_requested[BOARD_SLUG_MAX_LENGTH] = '\0';
    } else {
        s_requested[0] = '\0';
    }
    s_mismatch = true;
    return false;
}

const char* requestedBoardProfile() {
    return s_requested;
}

bool boardProfileMismatch() {
    return s_mismatch;
}

// ============================================================================
// INFO line
// ============================================================================

/// Append to `buffer` at `offset`, keeping the result terminated and bounded.
static size_t appendTo(char* buffer, size_t size, size_t offset, const char* fmt, ...) {
    if (offset >= size) return offset;
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buffer + offset, size - offset, fmt, args);
    va_end(args);
    if (written < 0) return offset;
    size_t next = offset + (size_t)written;
    return (next >= size) ? size - 1 : next;
}

size_t buildBoardInfoLine(char* buffer, size_t size, bool withSelection) {
    if (!buffer || size == 0) return 0;
    buffer[0] = '\0';

    size_t length = appendTo(buffer, size, 0, "INFO firmware=%s protocol=%s board=%s",
                             FIRMWARE_VERSION, PROTOCOL_VERSION, BOARD_TYPE);

    length = appendTo(buffer, size, length, " profiles=");
    for (uint8_t i = 0; i < boardProfileCount(); i++) {
        length = appendTo(buffer, size, length, "%s%s", i ? "," : "", boardProfileAt(i).slug);
    }
    length = appendTo(buffer, size, length, " default=%s", defaultBoardProfile().slug);

    if (withSelection) {
        const BoardProfile& profile = activeBoardProfile();
        length = appendTo(buffer, size, length, " boardVersion=%s holds=%u",
                          profile.slug, (unsigned)profile.holdCount);
        if (boardProfileMismatch()) {
            length = appendTo(buffer, size, length, " requested=%s mismatch=1",
                              requestedBoardProfile());
        }
    }

    return length;
}
