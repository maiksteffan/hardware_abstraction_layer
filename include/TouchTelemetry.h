/**
 * @file TouchTelemetry.h
 * @brief Runtime touch telemetry: ring buffer, counters, DIAG line emission
 *
 * Concrete ITouchTelemetrySink for the ESP32 target. Core 0 (touch task)
 * writes compact fixed-size TelemetryRecords into a ring buffer and bumps
 * counters inside short spinlock critical sections — it NEVER formats
 * strings or touches the serial port. Core 1 (main loop) drains the ring,
 * formats records into additive "DIAG TOUCH_*" lines and hands them to the
 * EventQueue.
 *
 * Safety rules (claude.md):
 *  - Telemetry never outranks gameplay: lines are queued only while the
 *    EventQueue has >= TOUCH_LOG_MIN_QUEUE_HEADROOM free slots; at most
 *    TOUCH_LOG_MAX_PER_FLUSH lines per loop tick, rate-limited by
 *    TOUCH_LOG_RATE_LIMIT_MS.
 *  - When the ring is full, new records are dropped and counted
 *    (RECORDS_DROPPED) — recording never blocks Core 0.
 *  - Disabled by default (TOUCH_LOG_DEFAULT_LEVEL = 0); enable with
 *    LOG_ON / LOG_LEVEL <n>.
 *
 * DIAG line formats (all begin with "DIAG " which the Pi logs and ignores):
 *   DIAG TOUCH_EP id=<n> state=start ctx=<ctx> cmd=<id> t=<ms>
 *   DIAG TOUCH_EP id=<n> state=end reason=<r> cand=<n> flags=<hex> dur=<ms> t=<ms>
 *   DIAG TOUCH_CAND ep=<n> pos=Hxx dur=<ms> peak=<d> avg=<d> strong=<pct> samples=<n> flags=<hex>
 *   DIAG TOUCH_DEC ep=<n> pos=Hxx cls=<c> act=<a> score=<s> reason=<r> t=<ms>
 *   DIAG TOUCH_PERF ... (two summary lines, emitted by LOG_STATUS)
 */

#ifndef TOUCH_TELEMETRY_H
#define TOUCH_TELEMETRY_H

#include <Arduino.h>
#include "Config.h"
#include "TouchIntentTypes.h"

class EventQueue;

class TouchTelemetry : public ITouchTelemetrySink {
public:
    TouchTelemetry();

    // ---- ITouchTelemetrySink (Core 0 safe, non-blocking) --------------------
    void record(const TelemetryRecord& rec) override;
    void bump(TelemetryCounter counter) override;
    bool enabledFor(uint8_t minLevel) const override;

    // ---- control (Core 1: LOG_* commands) ------------------------------------
    void setLevel(uint8_t level);          // clamped to TOUCH_LOG_MAX_LEVEL
    uint8_t level() const { return m_level; }
    void clear();                          // drop buffered records + reset counters
    void requestDump();                    // flush buffered records even at level 0

    // ---- gauges (Core 0) ------------------------------------------------------
    void noteTickDuration(uint32_t micros);  // touch-task tick high-water mark

    // ---- Core 1 only ----------------------------------------------------------
    // Drains up to TOUCH_LOG_MAX_PER_FLUSH records into DIAG lines. Cheap
    // no-op while disabled/empty.
    void flush(EventQueue& queue, uint32_t now);
    // Emits the two "DIAG TOUCH_PERF" summary lines (LOG_STATUS).
    void queueStatus(EventQueue& queue);

private:
    mutable portMUX_TYPE m_mux;
    TelemetryRecord m_ring[TOUCH_LOG_RING_CAPACITY];
    uint8_t m_head;
    uint8_t m_tail;
    uint8_t m_count;
    volatile uint8_t m_level;
    bool m_dumpRequested;
    uint32_t m_lastFlushMs;
    uint32_t m_counters[(uint8_t)TelemetryCounter::COUNT];
    uint32_t m_maxTickMicros;

    bool pop(TelemetryRecord& out);
    void formatRecord(const TelemetryRecord& rec, char* buffer, size_t size);
};

#endif // TOUCH_TELEMETRY_H
