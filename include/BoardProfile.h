/**
 * @file BoardProfile.h
 * @brief Per-board-version wiring tables, selectable at runtime.
 *
 * One firmware binary serves every physical Sequenzboard build. What differs
 * between builds is pure table data — hold count, which CAP1188 chip and
 * channel a hold hangs on, which LED index lights it — never logic. Those
 * tables live in a BoardProfile; the Pi names the profile it wants during the
 * startup handshake and the firmware activates it before initializing any
 * hardware.
 *
 * Add a board version by adding one entry to PROFILES[] in BoardProfiles.cpp.
 * Rewiring an existing board is a *new* slug, not an edit of the old one:
 * the Pi has no other way to tell two wirings of "v1" apart.
 *
 * See docs/Board_Version_Firmware_Profiles.md in the Sequenzboard repo.
 */

#ifndef BOARD_PROFILE_H
#define BOARD_PROFILE_H

// Deliberately not <Arduino.h>: a profile is pure wiring data with no
// hardware dependency, which is what lets it be tested on the host.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ============================================================================
// Ceilings
// ============================================================================

// Upper bound on holds across all profiles; every per-hold array is sized to
// it. Capped at 99 because a position token is a fixed 4-byte string ("H01" +
// terminator) throughout the protocol — "H100" would not fit, and widening the
// token is a protocol break, not a profile concern.
constexpr uint8_t MAX_HOLDS = 99;

// Upper bound on physical CAP1188 chips across all profiles.
constexpr uint8_t MAX_SENSORS = 16;

// Longest LED strip across all profiles. Strips are allocated at begin() from
// the active profile, so this only bounds what a profile may ask for.
constexpr uint16_t MAX_LED_STRIP_LENGTH = 600;

// Longest profile slug, matching the server's slug validation (32 chars).
constexpr uint8_t BOARD_SLUG_MAX_LENGTH = 32;

// ============================================================================
// Mapping types
// ============================================================================

enum class StripId : uint8_t { STRIP1 = 0, STRIP2 = 1 };

/// Primary physical LED of a logical position.
struct LedMapping {
    StripId strip;
    uint8_t index;
};

/// Optional second physical LED for a logical position, so a single command
/// (SHOW/SUCCESS/HIDE/...) lights up both sides.
struct LedMirror {
    uint8_t position;   // 0-based logical position (e.g. 3 == H04)
    StripId strip;
    uint8_t index;
};

/// Which CAP1188 chip and channel a logical input hangs on.
struct InputMapping {
    uint8_t sensorIndex;   // 0..sensorCount-1 (index into sensorAddresses)
    uint8_t channel;       // 0..TOUCH_CHANNELS_PER_SENSOR-1 (CAP1188 CS1..CS7)
};

/// Bitset over logical inputs, used by EXPECT_ANY_EXCEPT.
///
/// A uint64_t would silently cap the board at 64 holds, so the mask is sized
/// from MAX_HOLDS instead. Out-of-range indices are ignored rather than
/// wrapping into another hold's bit.
struct HoldMask {
    uint8_t bits[(MAX_HOLDS + 7) / 8];

    void clear() { memset(bits, 0, sizeof(bits)); }

    void set(uint8_t index) {
        if (index < MAX_HOLDS) bits[index >> 3] |= (uint8_t)(1u << (index & 7));
    }

    bool test(uint8_t index) const {
        if (index >= MAX_HOLDS) return false;
        return (bits[index >> 3] & (uint8_t)(1u << (index & 7))) != 0;
    }
};

// ============================================================================
// Profile
// ============================================================================

struct BoardProfile {
    /// Matches the board version's slug on the server. This is the contract.
    const char* slug;

    uint8_t holdCount;      // Logical inputs H01..H{holdCount}
    uint8_t sensorCount;    // Physical CAP1188 chips

    const uint8_t* sensorAddresses;    // [sensorCount]
    const InputMapping* inputMappings; // [holdCount]
    const LedMapping* ledMappings;     // [holdCount]
    const LedMirror* ledMirrors;       // [ledMirrorCount], may be nullptr
    uint8_t ledMirrorCount;

    /// Physical LEDs lit per logical position. Must be ODD: a center LED plus
    /// an equal number of neighbours on each side.
    uint8_t ledPositionWidth;

    uint16_t strip1Length;
    uint16_t strip2Length;

    /// GPIO each strip's data line is on. Board builds sharing an MCU do not
    /// share their LED wiring — the bench board drives its single strip from
    /// GPIO25 — so the pin cannot live in the per-MCU build flags.
    uint8_t ledPin1;
    uint8_t ledPin2;
};

// ============================================================================
// Registry
// ============================================================================

/// Number of profiles this firmware ships.
uint8_t boardProfileCount();

/// Profile at `index`, or the default profile if `index` is out of range.
const BoardProfile& boardProfileAt(uint8_t index);

/// Profile whose slug matches, or nullptr. Comparison is case-sensitive —
/// server slugs are always lowercase.
const BoardProfile* findBoardProfile(const char* slug);

/// Profile used when the Pi sends no BOARD_VERSION, or sends an unknown one.
const BoardProfile& defaultBoardProfile();

/// Profile currently driving the hardware. Never null; the default profile is
/// active from boot until selectBoardProfile() says otherwise.
const BoardProfile& activeBoardProfile();

/// Activate the profile with this slug.
///
/// Returns false and leaves the default profile active when the slug is
/// unknown, so the board still boots and can report the mismatch instead of
/// going dark. Must be called before any controller's begin().
bool selectBoardProfile(const char* slug);

/// Slug the Pi asked for and did not get, or "" when there was no mismatch.
const char* requestedBoardProfile();

/// True when selectBoardProfile() rejected a slug and fell back to the default.
bool boardProfileMismatch();

// ============================================================================
// INFO line
// ============================================================================

/// Write the `INFO ...` line describing this firmware into `buffer`.
///
/// Single source of the INFO format: the startup banner and the INFO command
/// response must not drift apart, because the Pi parses both.
///
/// `withSelection` adds `boardVersion=` / `holds=` (and `requested=` /
/// `mismatch=1` after a rejected slug). Pass false before a profile has been
/// chosen — the banner that tells the Pi which profiles it may ask for.
///
/// Returns the number of characters written, excluding the terminator. Output
/// is truncated rather than overflowed when `size` is too small.
size_t buildBoardInfoLine(char* buffer, size_t size, bool withSelection);

#endif // BOARD_PROFILE_H
