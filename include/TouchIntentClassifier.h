/**
 * @file TouchIntentClassifier.h
 * @brief Conservative intent-aware qualification for open-selection touches
 *
 * Groups physical contacts into fixed-size InteractionEpisodes and classifies
 * each candidate as intentional / incidental / support / ambiguous using
 * deterministic, explainable integer evidence (no ML, no heap, no strings).
 *
 * Division of responsibility (unchanged): the Pi owns ALL game logic. This
 * classifier only refines WHICH physical contact answers an open-selection
 * expectation (EXPECT_ANY / EXPECT_ANY_EXCEPT). Expected-target commands
 * (EXPECT <pos>) keep their instant, unfiltered behavior; the classifier at
 * most records diagnostics about non-target contacts there.
 *
 * Modes (Config.h OPEN_SELECTION_INTENT_FILTER_MODE):
 *   Disabled — classifier not consulted.
 *   Shadow   — classifier runs and logs decisions; emission is entirely
 *              decided by the pre-existing qualification pipeline.
 *   Active   — the classifier may (a) DEFER a weak candidate for at most
 *              INTENT_DEFER_MAX_MS while a clearly stronger competitor is
 *              present, and (b) SUPPRESS a candidate that is high-confidence
 *              incidental (brief + released + dominated + spatially
 *              plausible). Everything ambiguous is still emitted.
 *
 * Design principle: a false suppression of an intentional grip is more
 * damaging than emitting an ambiguous contact.
 *
 * Threading: ALL methods are called from Core 0 (touch task) except none.
 * The class performs no I/O; telemetry goes through ITouchTelemetrySink.
 * Arduino-free: unit-tested natively (pio test -e native).
 */

#ifndef TOUCH_INTENT_CLASSIFIER_H
#define TOUCH_INTENT_CLASSIFIER_H

#include "Config.h"
#include "TouchIntentTypes.h"

class TouchIntentClassifier {
public:
    TouchIntentClassifier();

    void setTelemetrySink(ITouchTelemetrySink* sink);

    // ---- lifecycle ---------------------------------------------------------
    // Ends any active episode (finalizing candidates for telemetry) and
    // clears state. Called on CLEAN_QUEUE and internally on context change.
    void reset(uint32_t now, EpisodeEndReason reason);

    // ---- event feed (Core 0) -----------------------------------------------
    // A debounced press edge occurred on `input` under the given expectation
    // context. `commandId` is the arming command's correlation id (or
    // COMMAND_ID_NONE). `excluded` marks EXPECT_ANY_EXCEPT-excluded holds.
    void onPressEdge(uint8_t input, uint32_t now, EpisodeContext ctx,
                     uint32_t commandId, bool excluded);

    // Per-poll sample for a tracked input. `deltaValid=false` records a
    // missing delta sample (I2C failure or read budget) — classification
    // degrades gracefully, it never blocks on delta data.
    void onSample(uint8_t input, bool rawTouched, bool deltaValid,
                  int8_t delta, uint32_t now);

    // Debounced release edge (the 800 ms latch elapsed).
    void onReleaseEdge(uint8_t input, uint32_t now);

    // Sweep detected by the existing press-edge-ring heuristic.
    void noteSweepDetected(uint32_t now);

    // ---- decisions ---------------------------------------------------------
    // Called by TouchController when the pre-existing qualification pipeline
    // is about to emit TOUCHED for `input` through EXPECT_ANY(_EXCEPT).
    // Records the (shadow) decision and returns what ACTIVE mode would do.
    // The caller ignores the result unless the mode is ACTIVE.
    EmissionDecision decideEmission(uint8_t input, uint32_t now);

    // Firmware actually emitted TOUCHED for this candidate.
    void onEmitted(uint8_t input, uint32_t now);

    // Firmware suppressed this candidate (ACTIVE mode only).
    void onSuppressed(uint8_t input, uint32_t now);

    // ---- periodic maintenance (once per poll tick) --------------------------
    // Finalizes released candidates, ends the episode on idle/max-duration,
    // emits telemetry records.
    void evaluate(uint32_t now);

    // ---- queries -----------------------------------------------------------
    bool hasActiveEpisode() const { return m_episode.active; }
    bool isTracking(uint8_t input) const;

    // Introspection (tests / diagnostics)
    const InteractionEpisode& episode() const { return m_episode; }
    const TouchCandidate* findCandidate(uint8_t input) const;

private:
    ITouchTelemetrySink* m_telemetry;
    InteractionEpisode m_episode;
    uint16_t m_nextEpisodeId;

    TouchCandidate* findLiveCandidate(uint8_t input);
    void startEpisode(uint32_t now, EpisodeContext ctx, uint32_t commandId);
    void endEpisode(uint32_t now, EpisodeEndReason reason);

    // Deterministic 0..100 evidence score (persistence + delta strength +
    // ongoing contact). Missing delta data scores neutrally, not negatively.
    static uint8_t computeScore(const TouchCandidate& c, uint32_t now);
    static uint32_t candidateDuration(const TouchCandidate& c, uint32_t now);
    static bool isBrief(const TouchCandidate& c, uint32_t now);
    static bool startGapWithin(const TouchCandidate& a, const TouchCandidate& b,
                               uint16_t windowMs);

    // Strongest other live candidate whose press start is within
    // INCIDENTAL_COMPETITOR_WINDOW_MS of `c`. nullptr when none.
    TouchCandidate* strongestCompetitor(const TouchCandidate& c, uint32_t now);

    bool isChordPair(const TouchCandidate& a, const TouchCandidate& b,
                     uint32_t now) const;
    // Competitor clearly dominates c (score margin + persistence + delta
    // margin + spatial plausibility).
    bool dominates(const TouchCandidate& comp, const TouchCandidate& c,
                   uint32_t now) const;

    void classify(TouchCandidate& c, uint32_t now, CandidateClass cls,
                  CandidateAction act, CandidateReason reason);
    void recordDecision(const TouchCandidate& c, uint32_t now);
    void recordCandidate(const TouchCandidate& c, uint32_t now);
    void recordEpisode(TelemetryRecordType type, uint32_t now,
                       EpisodeEndReason reason);
    void finalizeUndecided(uint32_t now, CandidateReason fallbackReason);
};

#endif // TOUCH_INTENT_CLASSIFIER_H
