/**
 * @file TouchIntentTypes.h
 * @brief Shared plain-data types for touch intent classification & telemetry
 *
 * This header is deliberately Arduino-free (plain stdint types only) so the
 * intent classifier and hold geometry can be compiled and unit-tested on the
 * host (pio test -e native).
 *
 * Terminology (see claude.md): a reported CAP1188 activation is a real
 * PHYSICAL CONTACT. The classifier's job is to distinguish INTENTIONAL
 * touches from INCIDENTAL / SUPPORT / AMBIGUOUS contacts — never to treat
 * them as sensor noise.
 */

#ifndef TOUCH_INTENT_TYPES_H
#define TOUCH_INTENT_TYPES_H

#include "Config.h"

// ============================================================================
// Contexts, classifications, decisions (kept as enums — no strings on Core 0)
// ============================================================================

// Which expectation context a contact occurred in. Inferred ONLY from the
// currently armed expectation type — the firmware has no game-mode state.
enum class EpisodeContext : uint8_t {
    NONE = 0,      // contact with no active expectation (counted, not tracked)
    OPEN_ANY,      // EXPECT_ANY armed (open selection)
    OPEN_EXCEPT,   // EXPECT_ANY_EXCEPT armed (open selection with exclusions)
    TARGET         // EXPECT <pos> armed (expected-target context)
};

enum class CandidateClass : uint8_t {
    UNKNOWN = 0,
    INTENTIONAL,   // high-confidence deliberate grab
    INCIDENTAL,    // high-confidence brief incidental body/clothing contact
    SUPPORT,       // possible stabilization contact (sustained, weaker, secondary)
    AMBIGUOUS,     // cannot decide with confidence — never aggressively suppressed
    EXCLUDED       // hold excluded by EXPECT_ANY_EXCEPT
};

// What the classifier decided to DO (separate from the classification and
// from whether the contact was physically real — see claude.md §data model).
enum class CandidateAction : uint8_t {
    NONE = 0,      // no action taken by the classifier (existing pipeline decided)
    EMIT,          // allow / would allow emission
    SUPPRESS,      // suppress / would suppress emission
    DEFER,         // postpone the decision (bounded)
    IGNORE         // diagnostic-only candidate (target context, excluded, episode end)
};

enum class CandidateReason : uint8_t {
    NONE = 0,
    STRONG_SUSTAINED,   // strong, persistent grip
    BRIEF_CONTACT,      // brief isolated contact
    BRIEF_VS_STRONG,    // brief contact dominated by a stronger competitor
    WEAK_VS_STRONG,     // weak but latched contact facing a stronger competitor
    VALID_CHORD,        // two-hold chord evidence
    SWEEP_PATTERN,      // part of an arm-sweep episode
    NO_CLEAR_WINNER,    // ambiguous — emitted conservatively
    EXCLUDED_HOLD,      // excluded by EXPECT_ANY_EXCEPT
    QUEUE_UNAVAILABLE,  // no expectation slot was available
    EPISODE_TIMEOUT,    // classified at episode end without an earlier decision
    CANDIDATE_LIMIT,    // candidate array was full
    MISSING_DELTA,      // no delta samples available (I2C degradation)
    POSSIBLE_SUPPORT    // sustained secondary contact alongside a stronger grip
};

enum class EpisodeEndReason : uint8_t {
    NONE = 0,
    IDLE,           // no activity for TOUCH_EPISODE_IDLE_END_MS
    MAX_DURATION,   // TOUCH_EPISODE_MAX_MS reached
    CLEANED,        // CLEAN_QUEUE received
    CONTEXT_CHANGE  // expectation context switched (e.g. EXPECT after EXPECT_ANY)
};

// Result the TouchController acts on in ACTIVE mode (ignored in SHADOW).
enum class EmissionDecision : uint8_t {
    ALLOW = 0,
    DEFER,
    SUPPRESS
};

// ============================================================================
// Candidate & episode records (fixed size, no heap)
// ============================================================================

struct TouchCandidate {
    bool used;                       // slot occupied
    uint8_t inputIndex;              // 0..INPUT_COUNT-1
    uint32_t pressStartMs;           // debounced press edge time
    uint32_t lastObservedMs;         // last sample time
    uint32_t releaseObservedMs;      // raw-release time (0 while touched)
    int8_t firstDelta;               // first valid delta sample
    int8_t peakDelta;                // strongest delta sample
    int32_t deltaSum;                // for mean delta (bounded by sampleCount)
    uint16_t sampleCount;            // poll samples observed
    uint16_t touchedSampleCount;     // samples with raw contact
    uint16_t deltaSampleCount;       // samples with a valid delta read
    uint16_t strongDeltaSampleCount; // delta >= INTENT_STRONG_DELTA_THRESHOLD
    uint16_t missingDeltaSampleCount;// delta read failed / unavailable
    uint8_t untouchedConsecutive;    // consecutive raw-untouched samples
    bool currentlyTouched;           // latest raw contact state
    bool released;                   // raw contact ended (dropout threshold)
    bool excluded;                   // excluded by EXPECT_ANY_EXCEPT
    bool emitted;                    // firmware emitted TOUCHED for this press
    bool suppressed;                 // firmware suppressed this press (ACTIVE)
    bool decided;                    // final classification recorded
    bool deferRecorded;              // DEFER decision already logged once
    uint32_t deferStartMs;           // 0 = not deferring
    uint8_t score;                   // 0..100 deterministic evidence score
    CandidateClass cls;
    CandidateAction action;
    CandidateReason reason;
};

struct InteractionEpisode {
    bool active;
    uint16_t id;                     // monotonically increasing
    uint32_t startMs;
    uint32_t lastActivityMs;
    EpisodeContext context;
    uint32_t commandId;              // correlation id of the arming command (or COMMAND_ID_NONE)
    uint8_t candidateCount;          // accepted candidates (capped)
    bool sweepLike;
    bool chordLike;
    bool anyAmbiguous;
    bool anyEmitted;
    bool overflowed;                 // more presses than candidate slots
    TouchCandidate candidates[TOUCH_EPISODE_MAX_CANDIDATES];
};

// ============================================================================
// Telemetry records & counters (compact — formatted only on Core 1)
// ============================================================================

enum class TelemetryRecordType : uint8_t {
    EP_START = 0,
    EP_END,
    CANDIDATE,
    DECISION
};

// Candidate/episode flag bits used in TelemetryRecord::flags
constexpr uint8_t TLM_FLAG_SWEEP    = 0x01;
constexpr uint8_t TLM_FLAG_CHORD    = 0x02;
constexpr uint8_t TLM_FLAG_AMBIG    = 0x04;
constexpr uint8_t TLM_FLAG_EMITTED  = 0x08;
constexpr uint8_t TLM_FLAG_EXCLUDED = 0x10;
constexpr uint8_t TLM_FLAG_RELEASED = 0x20;
constexpr uint8_t TLM_FLAG_OVERFLOW = 0x40;

struct TelemetryRecord {
    TelemetryRecordType type;
    uint8_t inputIndex;      // 255 = none
    uint8_t context;         // EpisodeContext
    uint8_t cls;             // CandidateClass
    uint8_t act;             // CandidateAction
    uint8_t reason;          // CandidateReason
    uint8_t score;           // 0..100
    uint8_t candidateCount;
    uint8_t flags;           // TLM_FLAG_*
    uint8_t endReason;       // EpisodeEndReason
    uint8_t strongPct;       // % of delta samples above threshold
    int8_t peakDelta;
    int8_t meanDelta;
    uint16_t episodeId;
    uint16_t durationMs;     // capped at 65535
    uint16_t touchedSamples;
    uint32_t commandId;
    uint32_t timestampMs;
};

enum class TelemetryCounter : uint8_t {
    POLL_CYCLES = 0,
    MISSED_POLLS,
    I2C_STATUS_FAIL,
    I2C_DELTA_FAIL,
    CANDIDATE_OVERFLOW,
    SUPPRESSED,
    EMITTED,
    AMBIGUOUS,
    CHORDS,
    SWEEP_EPISODES,
    RECORDS_DROPPED,
    NO_EXPECT_CONTACT,     // physical contact with no active expectation
    NON_TARGET_CONTACT,    // contact on a non-armed hold during EXPECT <pos>
    COUNT
};

// Abstract sink so the classifier stays host-testable (TouchTelemetry — the
// concrete ESP32 implementation — depends on FreeRTOS; the classifier only
// depends on this interface and may hold a nullptr).
class ITouchTelemetrySink {
public:
    virtual ~ITouchTelemetrySink() {}
    virtual void record(const TelemetryRecord& rec) = 0;
    virtual void bump(TelemetryCounter counter) = 0;
    virtual bool enabledFor(uint8_t minLevel) const = 0;
};

#endif // TOUCH_INTENT_TYPES_H
