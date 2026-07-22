/**
 * @file test_intent_classifier.cpp
 * @brief Native (host) unit tests for TouchIntentClassifier + HoldGeometry
 *
 * Run with:  pio test -e native
 *
 * The classifier is Arduino-free; the tests drive it with synthetic press /
 * sample / release feeds at the real 5 ms poll cadence and verify the
 * scenario matrix from claude.md:
 *   - single intentional touch is emitted
 *   - brief isolated contact stays AMBIGUOUS (never confidently suppressed)
 *   - brush-by directly before a strong grab is INCIDENTAL (suppressible)
 *   - two-hold chords are preserved (both ALLOW)
 *   - sweep episodes classify brief members as incidental, final grip allowed
 *   - multiple sustained contacts are never suppressed
 *   - weak-vs-strong deferral is bounded and ends in conservative emission
 *   - millis() wraparound safety
 *   - candidate overflow does not crash and is counted
 *   - reset (CLEAN_QUEUE) drops all tracking
 */

#include <unity.h>

// Compile the pure-logic modules directly into the test binary.
#include "../../src/HoldGeometry.cpp"
#include "../../src/TouchIntentClassifier.cpp"

// ============================================================================
// Test telemetry sink
// ============================================================================

struct TestSink : public ITouchTelemetrySink {
    uint32_t counters[(uint8_t)TelemetryCounter::COUNT];
    uint32_t recordCount;

    TestSink() { resetAll(); }
    void resetAll() {
        for (uint8_t i = 0; i < (uint8_t)TelemetryCounter::COUNT; i++) counters[i] = 0;
        recordCount = 0;
    }
    void record(const TelemetryRecord&) override { recordCount++; }
    void bump(TelemetryCounter c) override { counters[(uint8_t)c]++; }
    bool enabledFor(uint8_t) const override { return true; }
    uint32_t get(TelemetryCounter c) const { return counters[(uint8_t)c]; }
};

static TouchIntentClassifier clf;
static TestSink sink;

// Input indices used in tests (H17=16, H18=17 are direct neighbours: dx=190)
static const uint8_t H15_IDX = 14;
static const uint8_t H16_IDX = 15;
static const uint8_t H17_IDX = 16;
static const uint8_t H18_IDX = 17;
static const uint8_t H29_IDX = 28;  // far corner, NOT adjacent to H17/H18

static const uint32_t CMD_ID = 42;

void setUp(void) {
    clf.reset(0, EpisodeEndReason::NONE);
    sink.resetAll();
    clf.setTelemetrySink(&sink);
}

void tearDown(void) {}

// Feed poll samples every 5 ms in [from, to).
static void feed(uint8_t input, uint32_t from, uint32_t to, bool touched,
                 bool deltaValid, int8_t delta) {
    for (uint32_t t = from; t < to; t += 5) {
        clf.onSample(input, touched, deltaValid, delta, t);
    }
}

static void press(uint8_t input, uint32_t at,
                  EpisodeContext ctx = EpisodeContext::OPEN_ANY,
                  bool excluded = false) {
    clf.onPressEdge(input, at, ctx, CMD_ID, excluded);
}

// ============================================================================
// Geometry
// ============================================================================

void test_geometry_basics(void) {
    // H17 (1130,380) vs H18 (940,380): dx=190 -> 36100
    TEST_ASSERT_EQUAL_UINT32(36100u, squaredHoldDistance(H17_IDX, H18_IDX));
    TEST_ASSERT_TRUE(areHoldsAdjacent(H17_IDX, H18_IDX));
    // H17 vs far corner H29 (1320,100): clearly not adjacent
    TEST_ASSERT_FALSE(areHoldsAdjacent(H17_IDX, H29_IDX));
    // Bounds safety
    TEST_ASSERT_FALSE(areHoldsAdjacent(200, H17_IDX));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, squaredHoldDistance(200, 0));
    // Self is not "adjacent"
    TEST_ASSERT_FALSE(areHoldsAdjacent(H17_IDX, H17_IDX));
}

// ============================================================================
// Scenario: single intentional touch
// ============================================================================

void test_single_intentional_touch_allowed(void) {
    press(H18_IDX, 0);
    feed(H18_IDX, 0, 150, true, true, 80);  // strong sustained grip

    EmissionDecision d = clf.decideEmission(H18_IDX, 150);
    TEST_ASSERT_EQUAL(EmissionDecision::ALLOW, d);

    const TouchCandidate* c = clf.findCandidate(H18_IDX);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL(CandidateClass::INTENTIONAL, c->cls);
    TEST_ASSERT_EQUAL(CandidateReason::STRONG_SUSTAINED, c->reason);

    clf.onEmitted(H18_IDX, 150);
    TEST_ASSERT_EQUAL_UINT32(1, sink.get(TelemetryCounter::EMITTED));
}

// ============================================================================
// Scenario: brief isolated contact -> AMBIGUOUS, never suppressed
// ============================================================================

void test_brief_isolated_contact_stays_ambiguous(void) {
    press(H17_IDX, 0);
    feed(H17_IDX, 0, 60, true, true, 20);    // 60 ms weak contact
    feed(H17_IDX, 60, 90, false, false, 0);  // raw contact gone -> released

    // Let the episode idle out (evaluate runs each poll tick).
    for (uint32_t t = 90; t < 2000; t += 5) clf.evaluate(t);

    TEST_ASSERT_FALSE(clf.hasActiveEpisode());
    // Physical truth preserved, classification cautious: with NO competitor
    // a brief contact must never be confidently incidental.
    // (Candidate storage is gone with the episode; verify via counters:
    // no suppression happened, one ambiguous classification recorded.)
    TEST_ASSERT_EQUAL_UINT32(0, sink.get(TelemetryCounter::SUPPRESSED));
    TEST_ASSERT_EQUAL_UINT32(1, sink.get(TelemetryCounter::AMBIGUOUS));
}

// ============================================================================
// Scenario: shoulder brush directly before a strong grab
// ============================================================================

void test_brush_before_strong_grab_is_incidental(void) {
    // H17: brief weak brush 0..80 ms, then raw release
    press(H17_IDX, 0);
    feed(H17_IDX, 0, 80, true, true, 20);
    feed(H17_IDX, 80, 110, false, false, 0);   // dropout -> released

    // H18 (adjacent): strong grab starting at 70 ms
    press(H18_IDX, 70);
    feed(H18_IDX, 70, 300, true, true, 80);

    // Strong grab sails through.
    TEST_ASSERT_EQUAL(EmissionDecision::ALLOW, clf.decideEmission(H18_IDX, 300));
    clf.onEmitted(H18_IDX, 300);

    // The brush is confidently incidental (released + brief + dominated +
    // adjacent) -> ACTIVE mode may suppress it.
    TEST_ASSERT_EQUAL(EmissionDecision::SUPPRESS, clf.decideEmission(H17_IDX, 300));
    const TouchCandidate* c = clf.findCandidate(H17_IDX);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL(CandidateClass::INCIDENTAL, c->cls);
    TEST_ASSERT_EQUAL(CandidateReason::BRIEF_VS_STRONG, c->reason);
}

// ============================================================================
// Scenario: deliberate two-hold chord is preserved
// ============================================================================

void test_chord_both_allowed(void) {
    press(H17_IDX, 0);
    press(H18_IDX, 40);
    feed(H17_IDX, 0, 160, true, true, 80);
    feed(H18_IDX, 40, 160, true, true, 75);

    TEST_ASSERT_EQUAL(EmissionDecision::ALLOW, clf.decideEmission(H17_IDX, 160));
    clf.onEmitted(H17_IDX, 160);

    feed(H17_IDX, 160, 200, true, true, 80);
    feed(H18_IDX, 160, 200, true, true, 75);
    TEST_ASSERT_EQUAL(EmissionDecision::ALLOW, clf.decideEmission(H18_IDX, 200));

    const TouchCandidate* a = clf.findCandidate(H17_IDX);
    const TouchCandidate* b = clf.findCandidate(H18_IDX);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL(CandidateReason::VALID_CHORD, a->reason);
    TEST_ASSERT_EQUAL(CandidateReason::VALID_CHORD, b->reason);
    TEST_ASSERT_TRUE(clf.episode().chordLike);
    TEST_ASSERT_EQUAL_UINT32(0, sink.get(TelemetryCounter::SUPPRESSED));
}

// ============================================================================
// Scenario: sweep ending in a real grip
// ============================================================================

void test_sweep_then_grip(void) {
    // Three brief contacts marching across the wall...
    press(H15_IDX, 0);
    feed(H15_IDX, 0, 50, true, true, 25);
    feed(H15_IDX, 50, 80, false, false, 0);

    press(H16_IDX, 100);
    feed(H16_IDX, 100, 150, true, true, 25);
    feed(H16_IDX, 150, 180, false, false, 0);

    press(H17_IDX, 200);
    feed(H17_IDX, 200, 250, true, true, 25);
    feed(H17_IDX, 250, 280, false, false, 0);

    clf.noteSweepDetected(200);
    TEST_ASSERT_EQUAL_UINT32(1, sink.get(TelemetryCounter::SWEEP_EPISODES));

    // ...ending in a sustained grab.
    press(H18_IDX, 300);
    feed(H18_IDX, 300, 500, true, true, 80);

    TEST_ASSERT_EQUAL(EmissionDecision::ALLOW, clf.decideEmission(H18_IDX, 500));
    clf.onEmitted(H18_IDX, 500);

    // Idle out the episode; brief sweep members classify as incidental.
    feed(H18_IDX, 500, 520, false, false, 0);
    clf.onReleaseEdge(H18_IDX, 520);
    for (uint32_t t = 520; t < 2200; t += 5) clf.evaluate(t);
    TEST_ASSERT_FALSE(clf.hasActiveEpisode());
    TEST_ASSERT_EQUAL_UINT32(0, sink.get(TelemetryCounter::SUPPRESSED));
}

// ============================================================================
// Scenario: several sustained contacts -> nothing is suppressed
// ============================================================================

void test_multiple_sustained_contacts_never_suppressed(void) {
    press(H16_IDX, 0);
    press(H17_IDX, 50);
    press(H18_IDX, 100);
    feed(H16_IDX, 0, 400, true, true, 70);
    feed(H17_IDX, 50, 400, true, true, 60);
    feed(H18_IDX, 100, 400, true, true, 65);

    TEST_ASSERT_NOT_EQUAL(EmissionDecision::SUPPRESS, clf.decideEmission(H16_IDX, 400));
    TEST_ASSERT_NOT_EQUAL(EmissionDecision::SUPPRESS, clf.decideEmission(H17_IDX, 400));
    TEST_ASSERT_NOT_EQUAL(EmissionDecision::SUPPRESS, clf.decideEmission(H18_IDX, 400));
    TEST_ASSERT_EQUAL_UINT32(0, sink.get(TelemetryCounter::SUPPRESSED));
}

// ============================================================================
// Scenario: weak-vs-strong deferral is bounded
// ============================================================================

void test_weak_candidate_defer_is_bounded(void) {
    press(H17_IDX, 0);                      // weak but latched contact
    press(H18_IDX, 50);                     // strong competitor
    feed(H17_IDX, 0, 200, true, true, 20);
    feed(H18_IDX, 50, 200, true, true, 80);

    // Dominated + still touched -> DEFER
    TEST_ASSERT_EQUAL(EmissionDecision::DEFER, clf.decideEmission(H17_IDX, 200));

    feed(H17_IDX, 200, 350, true, true, 20);
    feed(H18_IDX, 200, 350, true, true, 80);
    TEST_ASSERT_EQUAL(EmissionDecision::DEFER, clf.decideEmission(H17_IDX, 350));

    // Defer window (INTENT_DEFER_MAX_MS after first defer at t=200) elapsed:
    // conservative emission as AMBIGUOUS. The candidate is never lost.
    feed(H17_IDX, 350, 650, true, true, 20);
    feed(H18_IDX, 350, 650, true, true, 80);
    TEST_ASSERT_EQUAL(EmissionDecision::ALLOW, clf.decideEmission(H17_IDX, 650));

    const TouchCandidate* c = clf.findCandidate(H17_IDX);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL(CandidateClass::AMBIGUOUS, c->cls);
    TEST_ASSERT_EQUAL(CandidateReason::NO_CLEAR_WINNER, c->reason);
}

// ============================================================================
// Scenario: millis() wraparound
// ============================================================================

void test_millis_wraparound_safe(void) {
    // Start 100 ms before the uint32 wrap; timestamps cross 0.
    uint32_t base = 0xFFFFFFFFu - 100u;
    press(H18_IDX, base);
    for (uint32_t i = 0; i < 40; i++) {  // 200 ms of samples across the wrap
        clf.onSample(H18_IDX, true, true, 80, base + i * 5u);
    }
    uint32_t now = base + 200u;  // wrapped: == 99
    TEST_ASSERT_EQUAL(EmissionDecision::ALLOW, clf.decideEmission(H18_IDX, now));
    const TouchCandidate* c = clf.findCandidate(H18_IDX);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL(CandidateClass::INTENTIONAL, c->cls);
}

// ============================================================================
// Scenario: candidate overflow degrades deterministically
// ============================================================================

void test_candidate_overflow_counted(void) {
    for (uint8_t i = 0; i < 12; i++) {
        press(i, 10u * i);
    }
    TEST_ASSERT_EQUAL_UINT8(TOUCH_EPISODE_MAX_CANDIDATES,
                            clf.episode().candidateCount);
    TEST_ASSERT_EQUAL_UINT32(12 - TOUCH_EPISODE_MAX_CANDIDATES,
                             sink.get(TelemetryCounter::CANDIDATE_OVERFLOW));
    // Untracked presses never interfere with emission.
    TEST_ASSERT_EQUAL(EmissionDecision::ALLOW, clf.decideEmission(11, 200));
}

// ============================================================================
// Scenario: reset (CLEAN_QUEUE) drops all tracking
// ============================================================================

void test_reset_clears_tracking(void) {
    press(H17_IDX, 0);
    feed(H17_IDX, 0, 100, true, true, 60);
    TEST_ASSERT_TRUE(clf.hasActiveEpisode());
    TEST_ASSERT_TRUE(clf.isTracking(H17_IDX));

    clf.reset(100, EpisodeEndReason::CLEANED);
    TEST_ASSERT_FALSE(clf.hasActiveEpisode());
    TEST_ASSERT_FALSE(clf.isTracking(H17_IDX));
    TEST_ASSERT_EQUAL(EmissionDecision::ALLOW, clf.decideEmission(H17_IDX, 110));
}

// ============================================================================
// Scenario: excluded hold (EXPECT_ANY_EXCEPT) classified, not emitted-blocked
// ============================================================================

void test_excluded_hold_ignored(void) {
    press(H17_IDX, 0, EpisodeContext::OPEN_EXCEPT, true /* excluded */);
    feed(H17_IDX, 0, 200, true, true, 80);
    // Classifier never blocks: fireExpectAny() itself skips excluded holds.
    TEST_ASSERT_EQUAL(EmissionDecision::ALLOW, clf.decideEmission(H17_IDX, 200));
    const TouchCandidate* c = clf.findCandidate(H17_IDX);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL(CandidateClass::EXCLUDED, c->cls);
    TEST_ASSERT_EQUAL(CandidateReason::EXCLUDED_HOLD, c->reason);
}

// ============================================================================
// Scenario: sustained secondary contact -> possible support, not suppressed
// ============================================================================

void test_sustained_secondary_contact_is_support(void) {
    // Strong grab, emitted.
    press(H18_IDX, 0);
    feed(H18_IDX, 0, 200, true, true, 80);
    TEST_ASSERT_EQUAL(EmissionDecision::ALLOW, clf.decideEmission(H18_IDX, 200));
    clf.onEmitted(H18_IDX, 200);

    // Weaker but SUSTAINED contact on the adjacent hold (e.g. steadying hand).
    press(H17_IDX, 100);
    feed(H17_IDX, 100, 600, true, true, 25);
    feed(H18_IDX, 200, 600, true, true, 80);

    // Both release; episode idles out.
    feed(H17_IDX, 600, 630, false, false, 0);
    feed(H18_IDX, 600, 630, false, false, 0);
    clf.onReleaseEdge(H17_IDX, 630);
    clf.onReleaseEdge(H18_IDX, 630);
    for (uint32_t t = 630; t < 2400; t += 5) clf.evaluate(t);

    TEST_ASSERT_FALSE(clf.hasActiveEpisode());
    // Sustained contacts are NEVER suppressed - only classified.
    TEST_ASSERT_EQUAL_UINT32(0, sink.get(TelemetryCounter::SUPPRESSED));
}

// ============================================================================
// Runner
// ============================================================================

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_geometry_basics);
    RUN_TEST(test_single_intentional_touch_allowed);
    RUN_TEST(test_brief_isolated_contact_stays_ambiguous);
    RUN_TEST(test_brush_before_strong_grab_is_incidental);
    RUN_TEST(test_chord_both_allowed);
    RUN_TEST(test_sweep_then_grip);
    RUN_TEST(test_multiple_sustained_contacts_never_suppressed);
    RUN_TEST(test_weak_candidate_defer_is_bounded);
    RUN_TEST(test_millis_wraparound_safe);
    RUN_TEST(test_candidate_overflow_counted);
    RUN_TEST(test_reset_clears_tracking);
    RUN_TEST(test_excluded_hold_ignored);
    RUN_TEST(test_sustained_secondary_contact_is_support);
    return UNITY_END();
}
