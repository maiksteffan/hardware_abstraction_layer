/**
 * @file TouchIntentClassifier.cpp
 * @brief Implementation of the conservative touch intent classifier
 *
 * All evidence is deterministic integer math (explainable, no floats in the
 * real-time path). All state is fixed-size; no heap, no strings on Core 0.
 *
 * Key invariants:
 *  - Physical truth is never overwritten by classification: every candidate
 *    keeps its raw sample statistics regardless of the decision.
 *  - Only high-confidence incidental contacts may be suppressed; everything
 *    ambiguous is emitted (or left to the pre-existing pipeline in Shadow).
 *  - Deferral is bounded by INTENT_DEFER_MAX_MS — a candidate can never be
 *    silently lost by waiting.
 *  - Two-hold chords are explicitly preserved (VALID_CHORD short-circuits
 *    dominance checks).
 *  - All timestamp arithmetic is overflow-safe (now - previous >= interval).
 */

#include "TouchIntentClassifier.h"
#include "HoldGeometry.h"
#include <string.h>

// ============================================================================
// Construction / lifecycle
// ============================================================================

TouchIntentClassifier::TouchIntentClassifier()
    : m_telemetry(nullptr)
    , m_nextEpisodeId(1)
{
    memset(&m_episode, 0, sizeof(m_episode));
}

void TouchIntentClassifier::setTelemetrySink(ITouchTelemetrySink* sink) {
    m_telemetry = sink;
}

void TouchIntentClassifier::reset(uint32_t now, EpisodeEndReason reason) {
    if (m_episode.active) {
        endEpisode(now, reason);
    }
    memset(&m_episode, 0, sizeof(m_episode));
}

// ============================================================================
// Episode management
// ============================================================================

void TouchIntentClassifier::startEpisode(uint32_t now, EpisodeContext ctx,
                                         uint32_t commandId) {
    memset(&m_episode, 0, sizeof(m_episode));
    m_episode.active = true;
    m_episode.id = m_nextEpisodeId++;
    if (m_nextEpisodeId == 0) m_nextEpisodeId = 1;  // skip 0 on wrap
    m_episode.startMs = now;
    m_episode.lastActivityMs = now;
    m_episode.context = ctx;
    m_episode.commandId = commandId;
    recordEpisode(TelemetryRecordType::EP_START, now, EpisodeEndReason::NONE);
}

void TouchIntentClassifier::endEpisode(uint32_t now, EpisodeEndReason reason) {
    if (!m_episode.active) return;

    // Give every undecided candidate a final (diagnostic-only) classification.
    finalizeUndecided(now, CandidateReason::EPISODE_TIMEOUT);

    // Candidate summaries (level >= 2).
    for (uint8_t i = 0; i < TOUCH_EPISODE_MAX_CANDIDATES; i++) {
        if (m_episode.candidates[i].used) {
            recordCandidate(m_episode.candidates[i], now);
        }
    }

    recordEpisode(TelemetryRecordType::EP_END, now, reason);
    m_episode.active = false;
}

// ============================================================================
// Event feed
// ============================================================================

void TouchIntentClassifier::onPressEdge(uint8_t input, uint32_t now,
                                        EpisodeContext ctx, uint32_t commandId,
                                        bool excluded) {
    if (input >= INPUT_COUNT || ctx == EpisodeContext::NONE) return;

    // A context switch (e.g. open selection -> target replay) means the
    // previous reaching action is over; stale episodes must never influence
    // a later unrelated expectation.
    if (m_episode.active && m_episode.context != ctx) {
        endEpisode(now, EpisodeEndReason::CONTEXT_CHANGE);
    }
    if (!m_episode.active) {
        startEpisode(now, ctx, commandId);
    }
    m_episode.lastActivityMs = now;

    // Re-grab on a hold that already has a RELEASED candidate opens a new
    // candidate (a fresh press edge is fresh evidence). A still-live
    // candidate for the same input is just refreshed (shouldn't normally
    // happen — press edges require a debounced release first).
    TouchCandidate* existing = findLiveCandidate(input);
    if (existing) {
        existing->lastObservedMs = now;
        return;
    }

    // Find a free slot.
    TouchCandidate* slot = nullptr;
    for (uint8_t i = 0; i < TOUCH_EPISODE_MAX_CANDIDATES; i++) {
        if (!m_episode.candidates[i].used) {
            slot = &m_episode.candidates[i];
            break;
        }
    }
    if (!slot) {
        // Deterministic degradation: count the overflow, keep gameplay
        // untouched (the pre-existing pipeline still handles the press).
        m_episode.overflowed = true;
        if (m_telemetry) m_telemetry->bump(TelemetryCounter::CANDIDATE_OVERFLOW);
        return;
    }

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->inputIndex = input;
    slot->pressStartMs = now;
    slot->lastObservedMs = now;
    slot->currentlyTouched = true;
    slot->excluded = excluded;
    slot->cls = CandidateClass::UNKNOWN;
    slot->action = CandidateAction::NONE;
    slot->reason = CandidateReason::NONE;
    if (m_episode.candidateCount < 255) m_episode.candidateCount++;
}

void TouchIntentClassifier::onSample(uint8_t input, bool rawTouched,
                                     bool deltaValid, int8_t delta,
                                     uint32_t now) {
    TouchCandidate* c = findLiveCandidate(input);
    if (!c) return;

    c->lastObservedMs = now;
    if (c->sampleCount < 0xFFFF) c->sampleCount++;
    c->currentlyTouched = rawTouched;

    if (rawTouched) {
        c->untouchedConsecutive = 0;
        if (c->touchedSampleCount < 0xFFFF) c->touchedSampleCount++;
        m_episode.lastActivityMs = now;
    } else if (!c->released) {
        if (c->untouchedConsecutive < 0xFF) c->untouchedConsecutive++;
        if (c->untouchedConsecutive >= EXPECT_ANY_DROPOUT_SAMPLES) {
            // Raw contact ended (same dropout criterion the qualification
            // pipeline uses). Debounced release follows much later (800 ms
            // latch) — for evidence purposes the contact is over now.
            c->released = true;
            c->releaseObservedMs = now;
        }
    }

    if (deltaValid) {
        if (c->deltaSampleCount == 0) c->firstDelta = delta;
        if (c->deltaSampleCount < 0xFFFF) {
            c->deltaSampleCount++;
            c->deltaSum += delta;
        }
        if (delta > c->peakDelta) c->peakDelta = delta;
        if (delta >= INTENT_STRONG_DELTA_THRESHOLD &&
            c->strongDeltaSampleCount < 0xFFFF) {
            c->strongDeltaSampleCount++;
        }
    } else if (c->missingDeltaSampleCount < 0xFFFF) {
        c->missingDeltaSampleCount++;
    }
}

void TouchIntentClassifier::onReleaseEdge(uint8_t input, uint32_t now) {
    TouchCandidate* c = findLiveCandidate(input);
    if (!c) return;
    if (!c->released) {
        c->released = true;
        c->releaseObservedMs = now;
    }
    c->currentlyTouched = false;
    m_episode.lastActivityMs = now;
}

void TouchIntentClassifier::noteSweepDetected(uint32_t now) {
    if (!m_episode.active) return;
    if (!m_episode.sweepLike) {
        m_episode.sweepLike = true;
        if (m_telemetry) m_telemetry->bump(TelemetryCounter::SWEEP_EPISODES);
    }
    m_episode.lastActivityMs = now;
}

// ============================================================================
// Evidence math
// ============================================================================

uint32_t TouchIntentClassifier::candidateDuration(const TouchCandidate& c,
                                                  uint32_t now) {
    uint32_t end = c.released ? c.releaseObservedMs : now;
    return end - c.pressStartMs;  // overflow-safe
}

bool TouchIntentClassifier::isBrief(const TouchCandidate& c, uint32_t now) {
    return c.released &&
           candidateDuration(c, now) < INCIDENTAL_MAX_DURATION_MS &&
           c.touchedSampleCount < INCIDENTAL_MAX_SAMPLE_COUNT;
}

bool TouchIntentClassifier::startGapWithin(const TouchCandidate& a,
                                           const TouchCandidate& b,
                                           uint16_t windowMs) {
    uint32_t gap = (a.pressStartMs >= b.pressStartMs)
                       ? (a.pressStartMs - b.pressStartMs)
                       : (b.pressStartMs - a.pressStartMs);
    return gap <= windowMs;
}

uint8_t TouchIntentClassifier::computeScore(const TouchCandidate& c,
                                            uint32_t now) {
    uint32_t dur = candidateDuration(c, now);
    uint32_t score = 0;

    // Persistence: up to 30 points, saturating at INTENT_MIN_PERSISTENCE_MS.
    if (dur >= INTENT_MIN_PERSISTENCE_MS) {
        score += 30;
    } else {
        score += (30 * dur) / INTENT_MIN_PERSISTENCE_MS;
    }
    // Sample-count corroboration: 10 points.
    if (c.touchedSampleCount >= INTENT_MIN_TOUCHED_SAMPLES) score += 10;

    // Delta strength: up to 45 points. Missing delta data scores a neutral
    // midpoint (22) instead of zero — I2C degradation must not turn every
    // touch into a "weak" candidate.
    if (c.deltaSampleCount > 0) {
        if (c.peakDelta >= INTENT_STRONG_DELTA_THRESHOLD) score += 20;
        uint32_t strongPct =
            (100u * c.strongDeltaSampleCount) / c.deltaSampleCount;
        if (strongPct >= INTENT_MIN_STRONG_SAMPLE_PERCENT) {
            score += 25;
        } else {
            score += (25 * strongPct) / INTENT_MIN_STRONG_SAMPLE_PERCENT;
        }
    } else {
        score += 22;
    }

    // Ongoing contact: 15 points (a grip that is STILL held is strong
    // evidence of intent).
    if (!c.released && c.currentlyTouched) score += 15;

    return (score > 100) ? 100 : (uint8_t)score;
}

TouchCandidate* TouchIntentClassifier::strongestCompetitor(
    const TouchCandidate& c, uint32_t now) {
    TouchCandidate* best = nullptr;
    uint8_t bestScore = 0;
    for (uint8_t i = 0; i < TOUCH_EPISODE_MAX_CANDIDATES; i++) {
        TouchCandidate& other = m_episode.candidates[i];
        if (!other.used || &other == &c) continue;
        if (other.inputIndex == c.inputIndex) continue;
        if (other.excluded || other.suppressed) continue;
        if (!startGapWithin(c, other, INCIDENTAL_COMPETITOR_WINDOW_MS)) continue;
        uint8_t s = computeScore(other, now);
        if (!best || s > bestScore) {
            best = &other;
            bestScore = s;
        }
    }
    return best;
}

bool TouchIntentClassifier::isChordPair(const TouchCandidate& a,
                                        const TouchCandidate& b,
                                        uint32_t now) const {
    // Both must still be in contact (a chord is two simultaneous grips).
    if (a.released || b.released) return false;
    if (!a.currentlyTouched || !b.currentlyTouched) return false;
    if (!startGapWithin(a, b, CHORD_MAX_START_GAP_MS)) return false;

    // Meaningful temporal overlap since the LATER press.
    uint32_t laterStart = (a.pressStartMs >= b.pressStartMs) ? a.pressStartMs
                                                             : b.pressStartMs;
    if (now - laterStart < CHORD_MIN_OVERLAP_MS) return false;

    // Similar strength/persistence: neither is dramatically weaker.
    uint8_t sa = computeScore(a, now);
    uint8_t sb = computeScore(b, now);
    uint8_t diff = (sa >= sb) ? (sa - sb) : (sb - sa);
    if (diff > CHORD_MAX_SCORE_DIFFERENCE) return false;

    // Delta plausibility: if both have delta data, both peaks should look
    // grab-like. Missing delta data does not veto a chord.
    if (a.deltaSampleCount > 0 && a.peakDelta < INTENT_STRONG_DELTA_THRESHOLD / 2)
        return false;
    if (b.deltaSampleCount > 0 && b.peakDelta < INTENT_STRONG_DELTA_THRESHOLD / 2)
        return false;

    return true;
}

bool TouchIntentClassifier::dominates(const TouchCandidate& comp,
                                      const TouchCandidate& c,
                                      uint32_t now) const {
    uint8_t compScore = computeScore(comp, now);
    uint8_t cScore = computeScore(c, now);

    // Competitor must clearly out-score the candidate...
    if (compScore < (uint16_t)cScore + INCIDENTAL_SCORE_MARGIN) return false;
    // ...and demonstrate real persistence itself.
    if (candidateDuration(comp, now) < INTENT_MIN_PERSISTENCE_MS &&
        !(comp.currentlyTouched && !comp.released)) {
        return false;
    }
    // Delta dominance (only checkable when both have delta data).
    if (comp.deltaSampleCount > 0 && c.deltaSampleCount > 0) {
        if ((int16_t)comp.peakDelta <
            (int16_t)c.peakDelta + INCIDENTAL_PEAK_DELTA_MARGIN) {
            return false;
        }
    }
    // Spatial plausibility: incidental contacts along a reach path are
    // adjacent to the real target. A distant contact is only dominated when
    // it was very brief.
    if (!areHoldsAdjacent(comp.inputIndex, c.inputIndex) && !isBrief(c, now)) {
        return false;
    }
    return true;
}

// ============================================================================
// Decisions
// ============================================================================

void TouchIntentClassifier::classify(TouchCandidate& c, uint32_t now,
                                     CandidateClass cls, CandidateAction act,
                                     CandidateReason reason) {
    c.score = computeScore(c, now);
    c.cls = cls;
    c.action = act;
    c.reason = reason;
    c.decided = true;
    if (cls == CandidateClass::AMBIGUOUS) {
        m_episode.anyAmbiguous = true;
        if (m_telemetry) m_telemetry->bump(TelemetryCounter::AMBIGUOUS);
    }
    recordDecision(c, now);
}

EmissionDecision TouchIntentClassifier::decideEmission(uint8_t input,
                                                       uint32_t now) {
    TouchCandidate* c = findLiveCandidate(input);
    if (!c) return EmissionDecision::ALLOW;  // untracked -> never interfere

    m_episode.lastActivityMs = now;

    // Excluded holds cannot satisfy EXPECT_ANY_EXCEPT anyway (fireExpectAny
    // checks the mask); record the classification once for telemetry.
    if (c->excluded) {
        if (!c->decided) {
            classify(*c, now, CandidateClass::EXCLUDED, CandidateAction::IGNORE,
                     CandidateReason::EXCLUDED_HOLD);
        }
        return EmissionDecision::ALLOW;
    }

    if (c->decided) {
        // Already classified (e.g. chord partner emitted earlier, or deferral
        // timed out). Suppression decisions stay final for this press.
        return (c->action == CandidateAction::SUPPRESS)
                   ? EmissionDecision::SUPPRESS
                   : EmissionDecision::ALLOW;
    }

    TouchCandidate* comp = strongestCompetitor(*c, now);

    // ---- 1. Chord preservation (checked BEFORE any dominance logic) -------
    if (comp && isChordPair(*c, *comp, now)) {
        m_episode.chordLike = true;
        if (m_telemetry) m_telemetry->bump(TelemetryCounter::CHORDS);
        classify(*c, now, CandidateClass::INTENTIONAL, CandidateAction::EMIT,
                 CandidateReason::VALID_CHORD);
        return EmissionDecision::ALLOW;
    }

    // ---- 2. Clearly dominated by a competitor ------------------------------
    if (comp && dominates(*comp, *c, now)) {
        if (c->released) {
            if (isBrief(*c, now)) {
                // High-confidence incidental: brief, already gone, dominated,
                // spatially plausible. The ONLY suppression case.
                classify(*c, now, CandidateClass::INCIDENTAL,
                         CandidateAction::SUPPRESS,
                         CandidateReason::BRIEF_VS_STRONG);
                return EmissionDecision::SUPPRESS;
            }
            // Released but not brief -> ambiguous; emit conservatively.
            classify(*c, now, CandidateClass::AMBIGUOUS, CandidateAction::EMIT,
                     CandidateReason::NO_CLEAR_WINNER);
            return EmissionDecision::ALLOW;
        }

        // Still in contact but currently much weaker than the competitor:
        // DEFER (bounded). If it keeps holding it will either become a chord,
        // out-wait the window, or release quickly (incidental).
        if (c->deferStartMs == 0) {
            c->deferStartMs = now;
        }
        if (now - c->deferStartMs < INTENT_DEFER_MAX_MS) {
            if (!c->deferRecorded) {
                c->deferRecorded = true;
                // Log the defer decision once (not final -> decided stays false).
                TouchCandidate snapshot = *c;
                snapshot.score = computeScore(*c, now);
                snapshot.cls = CandidateClass::UNKNOWN;
                snapshot.action = CandidateAction::DEFER;
                snapshot.reason = CandidateReason::WEAK_VS_STRONG;
                recordDecision(snapshot, now);
            }
            return EmissionDecision::DEFER;
        }
        // Deferral exhausted with the contact still held: ambiguous — emit.
        classify(*c, now, CandidateClass::AMBIGUOUS, CandidateAction::EMIT,
                 CandidateReason::NO_CLEAR_WINNER);
        return EmissionDecision::ALLOW;
    }

    // ---- 3. Standalone assessment ------------------------------------------
    uint8_t score = computeScore(*c, now);
    if (score >= INTENT_EMIT_SCORE) {
        classify(*c, now, CandidateClass::INTENTIONAL, CandidateAction::EMIT,
                 CandidateReason::STRONG_SUSTAINED);
    } else {
        classify(*c, now, CandidateClass::AMBIGUOUS, CandidateAction::EMIT,
                 (c->deltaSampleCount == 0) ? CandidateReason::MISSING_DELTA
                                            : CandidateReason::NO_CLEAR_WINNER);
    }
    return EmissionDecision::ALLOW;
}

void TouchIntentClassifier::onEmitted(uint8_t input, uint32_t now) {
    TouchCandidate* c = findLiveCandidate(input);
    if (!c) return;
    c->emitted = true;
    m_episode.anyEmitted = true;
    m_episode.lastActivityMs = now;
    if (m_telemetry) m_telemetry->bump(TelemetryCounter::EMITTED);
}

void TouchIntentClassifier::onSuppressed(uint8_t input, uint32_t now) {
    TouchCandidate* c = findLiveCandidate(input);
    if (!c) return;
    c->suppressed = true;
    m_episode.lastActivityMs = now;
    if (m_telemetry) m_telemetry->bump(TelemetryCounter::SUPPRESSED);
}

// ============================================================================
// Periodic maintenance
// ============================================================================

void TouchIntentClassifier::finalizeUndecided(uint32_t now,
                                              CandidateReason fallbackReason) {
    for (uint8_t i = 0; i < TOUCH_EPISODE_MAX_CANDIDATES; i++) {
        TouchCandidate& c = m_episode.candidates[i];
        if (!c.used || c.decided) continue;

        if (c.excluded) {
            classify(c, now, CandidateClass::EXCLUDED, CandidateAction::IGNORE,
                     CandidateReason::EXCLUDED_HOLD);
            continue;
        }
        if (c.emitted) {
            classify(c, now, CandidateClass::INTENTIONAL, CandidateAction::EMIT,
                     CandidateReason::STRONG_SUSTAINED);
            continue;
        }

        TouchCandidate* comp = strongestCompetitor(c, now);
        bool compEmittedOrStrong =
            comp && (comp->emitted || computeScore(*comp, now) >= INTENT_EMIT_SCORE);

        if (c.released && isBrief(c, now)) {
            if (m_episode.sweepLike) {
                // Brief member of a sweep episode: incidental by pattern.
                classify(c, now, CandidateClass::INCIDENTAL,
                         CandidateAction::IGNORE, CandidateReason::SWEEP_PATTERN);
            } else if (compEmittedOrStrong && dominates(*comp, c, now)) {
                classify(c, now, CandidateClass::INCIDENTAL,
                         CandidateAction::IGNORE, CandidateReason::BRIEF_VS_STRONG);
            } else {
                // Brief ISOLATED contact: never confidently incidental —
                // classification stays ambiguous (physical truth preserved).
                classify(c, now, CandidateClass::AMBIGUOUS,
                         CandidateAction::IGNORE, CandidateReason::BRIEF_CONTACT);
            }
            continue;
        }

        // Sustained but never emitted. A stable, weaker contact overlapping a
        // stronger emitted grip fits the support-contact pattern; the data
        // cannot PROVE body-part identity, so this stays a soft class.
        uint32_t dur = candidateDuration(c, now);
        if (dur >= INTENT_MIN_PERSISTENCE_MS && compEmittedOrStrong) {
            classify(c, now, CandidateClass::SUPPORT, CandidateAction::IGNORE,
                     CandidateReason::POSSIBLE_SUPPORT);
        } else {
            classify(c, now, CandidateClass::AMBIGUOUS, CandidateAction::IGNORE,
                     fallbackReason);
        }
    }
}

void TouchIntentClassifier::evaluate(uint32_t now) {
    if (!m_episode.active) return;

    // Hard duration cap.
    if (now - m_episode.startMs >= TOUCH_EPISODE_MAX_MS) {
        endEpisode(now, EpisodeEndReason::MAX_DURATION);
        return;
    }

    // Idle end: no press/sample/decision activity for the idle window AND no
    // candidate still in contact.
    bool anyLive = false;
    for (uint8_t i = 0; i < TOUCH_EPISODE_MAX_CANDIDATES; i++) {
        const TouchCandidate& c = m_episode.candidates[i];
        if (c.used && !c.released && c.currentlyTouched) {
            anyLive = true;
            break;
        }
    }
    if (!anyLive && now - m_episode.lastActivityMs >= TOUCH_EPISODE_IDLE_END_MS) {
        endEpisode(now, EpisodeEndReason::IDLE);
    }
}

// ============================================================================
// Lookup helpers
// ============================================================================

TouchCandidate* TouchIntentClassifier::findLiveCandidate(uint8_t input) {
    if (!m_episode.active) return nullptr;
    // Prefer the most recent (last) matching slot — a re-grab creates a new
    // candidate while the released one is kept for episode telemetry.
    TouchCandidate* found = nullptr;
    for (uint8_t i = 0; i < TOUCH_EPISODE_MAX_CANDIDATES; i++) {
        TouchCandidate& c = m_episode.candidates[i];
        if (c.used && c.inputIndex == input) found = &c;
    }
    return found;
}

const TouchCandidate* TouchIntentClassifier::findCandidate(uint8_t input) const {
    return const_cast<TouchIntentClassifier*>(this)->findLiveCandidate(input);
}

bool TouchIntentClassifier::isTracking(uint8_t input) const {
    const TouchCandidate* c = findCandidate(input);
    return c != nullptr && !c->released;
}

// ============================================================================
// Telemetry record emission
// ============================================================================

static uint8_t candidateFlags(const TouchCandidate& c,
                              const InteractionEpisode& ep) {
    uint8_t flags = 0;
    if (ep.sweepLike) flags |= TLM_FLAG_SWEEP;
    if (ep.chordLike) flags |= TLM_FLAG_CHORD;
    if (c.cls == CandidateClass::AMBIGUOUS) flags |= TLM_FLAG_AMBIG;
    if (c.emitted) flags |= TLM_FLAG_EMITTED;
    if (c.excluded) flags |= TLM_FLAG_EXCLUDED;
    if (c.released) flags |= TLM_FLAG_RELEASED;
    return flags;
}

void TouchIntentClassifier::recordDecision(const TouchCandidate& c,
                                           uint32_t now) {
    if (!m_telemetry || !m_telemetry->enabledFor(1)) return;
    TelemetryRecord r;
    memset(&r, 0, sizeof(r));
    r.type = TelemetryRecordType::DECISION;
    r.inputIndex = c.inputIndex;
    r.context = (uint8_t)m_episode.context;
    r.cls = (uint8_t)c.cls;
    r.act = (uint8_t)c.action;
    r.reason = (uint8_t)c.reason;
    r.score = c.score;
    r.flags = candidateFlags(c, m_episode);
    r.episodeId = m_episode.id;
    uint32_t dur = candidateDuration(c, now);
    r.durationMs = (dur > 0xFFFF) ? 0xFFFF : (uint16_t)dur;
    r.touchedSamples = c.touchedSampleCount;
    r.peakDelta = c.peakDelta;
    r.meanDelta = (c.deltaSampleCount > 0)
                      ? (int8_t)(c.deltaSum / (int32_t)c.deltaSampleCount)
                      : 0;
    r.strongPct = (c.deltaSampleCount > 0)
                      ? (uint8_t)((100u * c.strongDeltaSampleCount) /
                                  c.deltaSampleCount)
                      : 0;
    r.commandId = m_episode.commandId;
    r.timestampMs = now;
    m_telemetry->record(r);
}

void TouchIntentClassifier::recordCandidate(const TouchCandidate& c,
                                            uint32_t now) {
    if (!m_telemetry || !m_telemetry->enabledFor(2)) return;
    TelemetryRecord r;
    memset(&r, 0, sizeof(r));
    r.type = TelemetryRecordType::CANDIDATE;
    r.inputIndex = c.inputIndex;
    r.context = (uint8_t)m_episode.context;
    r.cls = (uint8_t)c.cls;
    r.act = (uint8_t)c.action;
    r.reason = (uint8_t)c.reason;
    r.score = c.score;
    r.flags = candidateFlags(c, m_episode);
    r.episodeId = m_episode.id;
    uint32_t dur = candidateDuration(c, now);
    r.durationMs = (dur > 0xFFFF) ? 0xFFFF : (uint16_t)dur;
    r.touchedSamples = c.touchedSampleCount;
    r.peakDelta = c.peakDelta;
    r.meanDelta = (c.deltaSampleCount > 0)
                      ? (int8_t)(c.deltaSum / (int32_t)c.deltaSampleCount)
                      : 0;
    r.strongPct = (c.deltaSampleCount > 0)
                      ? (uint8_t)((100u * c.strongDeltaSampleCount) /
                                  c.deltaSampleCount)
                      : 0;
    r.commandId = m_episode.commandId;
    r.timestampMs = now;
    m_telemetry->record(r);
}

void TouchIntentClassifier::recordEpisode(TelemetryRecordType type,
                                          uint32_t now,
                                          EpisodeEndReason reason) {
    if (!m_telemetry || !m_telemetry->enabledFor(1)) return;
    TelemetryRecord r;
    memset(&r, 0, sizeof(r));
    r.type = type;
    r.inputIndex = 255;
    r.context = (uint8_t)m_episode.context;
    r.episodeId = m_episode.id;
    r.candidateCount = m_episode.candidateCount;
    r.endReason = (uint8_t)reason;
    uint8_t flags = 0;
    if (m_episode.sweepLike) flags |= TLM_FLAG_SWEEP;
    if (m_episode.chordLike) flags |= TLM_FLAG_CHORD;
    if (m_episode.anyAmbiguous) flags |= TLM_FLAG_AMBIG;
    if (m_episode.anyEmitted) flags |= TLM_FLAG_EMITTED;
    if (m_episode.overflowed) flags |= TLM_FLAG_OVERFLOW;
    r.flags = flags;
    uint32_t dur = now - m_episode.startMs;
    r.durationMs = (dur > 0xFFFF) ? 0xFFFF : (uint16_t)dur;
    r.commandId = m_episode.commandId;
    r.timestampMs = now;
    m_telemetry->record(r);
}
