/**
 * @file TouchTelemetry.cpp
 * @brief Touch telemetry ring buffer and DIAG line formatting
 *
 * Cross-core discipline: Core 0 only calls record()/bump()/enabledFor()/
 * noteTickDuration() — all bounded, non-blocking spinlock sections with no
 * string work. Core 1 calls flush()/queueStatus()/setLevel()/clear()/
 * requestDump() and performs ALL formatting.
 */

#include "TouchTelemetry.h"
#include "EventQueue.h"
#include <string.h>

// ============================================================================
// Enum -> short token tables (Core 1 formatting only)
// ============================================================================

static const char* contextName(uint8_t ctx) {
    switch ((EpisodeContext)ctx) {
        case EpisodeContext::OPEN_ANY:    return "open_any";
        case EpisodeContext::OPEN_EXCEPT: return "open_except";
        case EpisodeContext::TARGET:      return "target";
        default:                          return "none";
    }
}

static const char* className(uint8_t cls) {
    switch ((CandidateClass)cls) {
        case CandidateClass::INTENTIONAL: return "intentional";
        case CandidateClass::INCIDENTAL:  return "incidental";
        case CandidateClass::SUPPORT:     return "support";
        case CandidateClass::AMBIGUOUS:   return "ambiguous";
        case CandidateClass::EXCLUDED:    return "excluded";
        default:                          return "unknown";
    }
}

static const char* actionName(uint8_t act) {
    switch ((CandidateAction)act) {
        case CandidateAction::EMIT:     return "emit";
        case CandidateAction::SUPPRESS: return "suppress";
        case CandidateAction::DEFER:    return "defer";
        case CandidateAction::IGNORE:   return "ignore";
        default:                        return "none";
    }
}

static const char* reasonName(uint8_t reason) {
    switch ((CandidateReason)reason) {
        case CandidateReason::STRONG_SUSTAINED:  return "strong_sustained";
        case CandidateReason::BRIEF_CONTACT:     return "brief_contact";
        case CandidateReason::BRIEF_VS_STRONG:   return "brief_vs_strong";
        case CandidateReason::WEAK_VS_STRONG:    return "weak_vs_strong";
        case CandidateReason::VALID_CHORD:       return "valid_chord";
        case CandidateReason::SWEEP_PATTERN:     return "sweep_pattern";
        case CandidateReason::NO_CLEAR_WINNER:   return "no_clear_winner";
        case CandidateReason::EXCLUDED_HOLD:     return "excluded_hold";
        case CandidateReason::QUEUE_UNAVAILABLE: return "queue_unavailable";
        case CandidateReason::EPISODE_TIMEOUT:   return "episode_timeout";
        case CandidateReason::CANDIDATE_LIMIT:   return "candidate_limit";
        case CandidateReason::MISSING_DELTA:     return "missing_delta";
        case CandidateReason::POSSIBLE_SUPPORT:  return "possible_support";
        default:                                 return "none";
    }
}

static const char* endReasonName(uint8_t reason) {
    switch ((EpisodeEndReason)reason) {
        case EpisodeEndReason::IDLE:           return "idle";
        case EpisodeEndReason::MAX_DURATION:   return "max_duration";
        case EpisodeEndReason::CLEANED:        return "cleaned";
        case EpisodeEndReason::CONTEXT_CHANGE: return "context_change";
        default:                               return "none";
    }
}

// ============================================================================
// Construction / control
// ============================================================================

TouchTelemetry::TouchTelemetry()
    : m_mux(portMUX_INITIALIZER_UNLOCKED)
    , m_head(0)
    , m_tail(0)
    , m_count(0)
    , m_level(TOUCH_LOG_DEFAULT_LEVEL)
    , m_dumpRequested(false)
    , m_lastFlushMs(0)
    , m_maxTickMicros(0)
{
    memset(m_ring, 0, sizeof(m_ring));
    memset(m_counters, 0, sizeof(m_counters));
}

void TouchTelemetry::setLevel(uint8_t level) {
    if (level > TOUCH_LOG_MAX_LEVEL) level = TOUCH_LOG_MAX_LEVEL;
    m_level = level;
}

bool TouchTelemetry::enabledFor(uint8_t minLevel) const {
    return m_level >= minLevel;
}

void TouchTelemetry::clear() {
    portENTER_CRITICAL(&m_mux);
    m_head = 0;
    m_tail = 0;
    m_count = 0;
    memset(m_counters, 0, sizeof(m_counters));
    m_maxTickMicros = 0;
    portEXIT_CRITICAL(&m_mux);
}

void TouchTelemetry::requestDump() {
    m_dumpRequested = true;
}

// ============================================================================
// Core 0 API — bounded, never blocks, never formats
// ============================================================================

void TouchTelemetry::record(const TelemetryRecord& rec) {
    portENTER_CRITICAL(&m_mux);
    if (m_count < TOUCH_LOG_RING_CAPACITY) {
        m_ring[m_head] = rec;
        m_head = (m_head + 1) % TOUCH_LOG_RING_CAPACITY;
        m_count++;
    } else {
        m_counters[(uint8_t)TelemetryCounter::RECORDS_DROPPED]++;
    }
    portEXIT_CRITICAL(&m_mux);
}

void TouchTelemetry::bump(TelemetryCounter counter) {
    if ((uint8_t)counter >= (uint8_t)TelemetryCounter::COUNT) return;
    portENTER_CRITICAL(&m_mux);
    m_counters[(uint8_t)counter]++;
    portEXIT_CRITICAL(&m_mux);
}

void TouchTelemetry::noteTickDuration(uint32_t micros) {
    portENTER_CRITICAL(&m_mux);
    if (micros > m_maxTickMicros) m_maxTickMicros = micros;
    portEXIT_CRITICAL(&m_mux);
}

// ============================================================================
// Core 1 API — formatting & emission
// ============================================================================

bool TouchTelemetry::pop(TelemetryRecord& out) {
    bool ok = false;
    portENTER_CRITICAL(&m_mux);
    if (m_count > 0) {
        out = m_ring[m_tail];
        m_tail = (m_tail + 1) % TOUCH_LOG_RING_CAPACITY;
        m_count--;
        ok = true;
    }
    portEXIT_CRITICAL(&m_mux);
    return ok;
}

void TouchTelemetry::formatRecord(const TelemetryRecord& rec, char* buffer,
                                  size_t size) {
    switch (rec.type) {
        case TelemetryRecordType::EP_START:
            snprintf(buffer, size,
                     "TOUCH_EP id=%u state=start ctx=%s cmd=%lu t=%lu",
                     (unsigned)rec.episodeId, contextName(rec.context),
                     (unsigned long)rec.commandId,
                     (unsigned long)rec.timestampMs);
            break;
        case TelemetryRecordType::EP_END:
            snprintf(buffer, size,
                     "TOUCH_EP id=%u state=end reason=%s cand=%u flags=0x%02X "
                     "dur=%u t=%lu",
                     (unsigned)rec.episodeId, endReasonName(rec.endReason),
                     (unsigned)rec.candidateCount, (unsigned)rec.flags,
                     (unsigned)rec.durationMs, (unsigned long)rec.timestampMs);
            break;
        case TelemetryRecordType::CANDIDATE:
            snprintf(buffer, size,
                     "TOUCH_CAND ep=%u pos=H%02u dur=%u peak=%d avg=%d "
                     "strong=%u samples=%u flags=0x%02X",
                     (unsigned)rec.episodeId, (unsigned)(rec.inputIndex + 1),
                     (unsigned)rec.durationMs, (int)rec.peakDelta,
                     (int)rec.meanDelta, (unsigned)rec.strongPct,
                     (unsigned)rec.touchedSamples, (unsigned)rec.flags);
            break;
        case TelemetryRecordType::DECISION:
            snprintf(buffer, size,
                     "TOUCH_DEC ep=%u pos=H%02u cls=%s act=%s score=%u "
                     "reason=%s t=%lu",
                     (unsigned)rec.episodeId, (unsigned)(rec.inputIndex + 1),
                     className(rec.cls), actionName(rec.act),
                     (unsigned)rec.score, reasonName(rec.reason),
                     (unsigned long)rec.timestampMs);
            break;
        default:
            snprintf(buffer, size, "TOUCH_UNKNOWN");
            break;
    }
}

void TouchTelemetry::flush(EventQueue& queue, uint32_t now) {
    // Records may still be buffered after LOG_OFF; drain them only when a
    // dump was requested. While enabled, drain continuously.
    if (m_level == 0 && !m_dumpRequested) return;
    if (m_count == 0) {
        m_dumpRequested = false;
        return;
    }
    // Rate limit bursts.
    if (now - m_lastFlushMs < TOUCH_LOG_RATE_LIMIT_MS) return;

    uint8_t sent = 0;
    char text[152];  // fits Event::extra (160) with "DIAG " prefix headroom
    TelemetryRecord rec;
    while (sent < TOUCH_LOG_MAX_PER_FLUSH) {
        // Gameplay first: only use the queue while there is real headroom.
        if (queue.freeSlots() < TOUCH_LOG_MIN_QUEUE_HEADROOM) break;
        if (!pop(rec)) break;
        formatRecord(rec, text, sizeof(text));
        queue.queueDiag(text);
        sent++;
    }
    if (sent > 0) m_lastFlushMs = now;
    if (m_count == 0) m_dumpRequested = false;
}

void TouchTelemetry::queueStatus(EventQueue& queue) {
    // Snapshot under the lock, format outside it.
    uint32_t counters[(uint8_t)TelemetryCounter::COUNT];
    uint32_t maxTick;
    uint8_t buffered;
    portENTER_CRITICAL(&m_mux);
    memcpy(counters, m_counters, sizeof(counters));
    maxTick = m_maxTickMicros;
    buffered = m_count;
    portEXIT_CRITICAL(&m_mux);

    char text[152];
    snprintf(text, sizeof(text),
             "TOUCH_PERF lvl=%u polls=%lu missed=%lu i2c_sf=%lu i2c_df=%lu "
             "tick_max_us=%lu buf=%u",
             (unsigned)m_level,
             (unsigned long)counters[(uint8_t)TelemetryCounter::POLL_CYCLES],
             (unsigned long)counters[(uint8_t)TelemetryCounter::MISSED_POLLS],
             (unsigned long)counters[(uint8_t)TelemetryCounter::I2C_STATUS_FAIL],
             (unsigned long)counters[(uint8_t)TelemetryCounter::I2C_DELTA_FAIL],
             (unsigned long)maxTick, (unsigned)buffered);
    queue.queueDiag(text);

    snprintf(text, sizeof(text),
             "TOUCH_PERF emitted=%lu suppressed=%lu ambig=%lu chords=%lu "
             "sweeps=%lu cand_ovf=%lu rec_drop=%lu noexp=%lu nontgt=%lu "
             "eq_fail=%lu eq_max=%u",
             (unsigned long)counters[(uint8_t)TelemetryCounter::EMITTED],
             (unsigned long)counters[(uint8_t)TelemetryCounter::SUPPRESSED],
             (unsigned long)counters[(uint8_t)TelemetryCounter::AMBIGUOUS],
             (unsigned long)counters[(uint8_t)TelemetryCounter::CHORDS],
             (unsigned long)counters[(uint8_t)TelemetryCounter::SWEEP_EPISODES],
             (unsigned long)counters[(uint8_t)TelemetryCounter::CANDIDATE_OVERFLOW],
             (unsigned long)counters[(uint8_t)TelemetryCounter::RECORDS_DROPPED],
             (unsigned long)counters[(uint8_t)TelemetryCounter::NO_EXPECT_CONTACT],
             (unsigned long)counters[(uint8_t)TelemetryCounter::NON_TARGET_CONTACT],
             (unsigned long)queue.pushFailureCount(),
             (unsigned)queue.maxDepthSeen());
    queue.queueDiag(text);
}
