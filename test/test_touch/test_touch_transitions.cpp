/**
 * @file test_touch_transitions.cpp
 * @brief Exercise the real touch pipeline with simulated time, I2C and events.
 */
// Host-only harness: the default ESP32 editor configuration must not resolve
// its test doubles against the hardware Arduino/Wire APIs.
#ifdef SEQUENZBOARD_NATIVE_TEST

#include <unity.h>
#include <stdio.h>
#include "TouchController.h"
#include "CommandController.h"
#include "EventQueue.h"

#include "../../src/BoardProfiles.cpp"
#include "../../src/TouchController.cpp"

TwoWire Wire;
static uint32_t g_now;
static Event g_events[64];
static uint8_t g_count;
static bool g_rejectEvents;
static bool g_eventMutexBusy;
static void (*g_afterCapture)();
static TouchController g_touch;
static EventQueue g_queue;

uint32_t millis() { return g_now; }
void delay(uint32_t milliseconds) { g_now += milliseconds; }

// Only the sink and string formatting are doubled; debounce, expectations and
// CAP1188 register transactions run unchanged from the production controller.
EventQueue::EventQueue() : m_head(0), m_tail(0), m_count(0),
    m_queueMutex(nullptr), m_serialMutex(nullptr) {}
EventQueue::~EventQueue() {}

static bool capture(EventType type, const char* position, uint32_t id) {
    if (g_rejectEvents) return false;
    if (g_count >= 64) return false;
    Event& event = g_events[g_count++];
    event = {};
    event.type = type;
    event.commandId = id;
    if (position) snprintf(event.position, sizeof(event.position), "%s", position);
    if (g_afterCapture) {
        auto callback = g_afterCapture;
        g_afterCapture = nullptr;
        callback();
    }
    return true;
}
bool EventQueue::queueTouched(const char* position, uint32_t id, uint16_t timeoutMs) {
    if (g_eventMutexBusy) { g_now += timeoutMs; return false; }
    return capture(EventType::TOUCHED, position, id);
}
bool EventQueue::queueTouchReleased(const char* position, uint32_t id, uint16_t timeoutMs) {
    if (g_eventMutexBusy) { g_now += timeoutMs; return false; }
    return capture(EventType::TOUCH_RELEASED, position, id);
}
bool EventQueue::queueHandsOn(uint32_t id, uint16_t timeoutMs) {
    if (g_eventMutexBusy) { g_now += timeoutMs; return false; }
    return capture(EventType::HANDS_ON, nullptr, id);
}
bool EventQueue::queueHandsOff(uint32_t id, uint16_t timeoutMs) {
    if (g_eventMutexBusy) { g_now += timeoutMs; return false; }
    return capture(EventType::HANDS_OFF, nullptr, id);
}
bool EventQueue::queueTouchState(const char* position, const char* state, uint32_t id) {
    if (g_eventMutexBusy) return false;
    if (!capture(EventType::TOUCH_STATE, position, id)) return false;
    snprintf(g_events[g_count - 1].extra, sizeof(Event::extra), "%s", state);
    return true;
}
void CommandController::indexToPosition(uint8_t index, char out[POSITION_STRING_LENGTH]) {
    snprintf(out, POSITION_STRING_LENGTH, "H%02u", static_cast<unsigned>(index + 1));
}

static void advance(uint32_t duration) {
    for (uint32_t elapsed = 0; elapsed < duration; elapsed += TOUCH_POLL_INTERVAL_MS) {
        g_now += TOUCH_POLL_INTERVAL_MS;
        g_touch.tick();
    }
}

static void contact(uint8_t input, bool touched) {
    const BoardProfile& profile = activeBoardProfile();
    const InputMapping& mapping = profile.inputMappings[input];
    uint8_t address = profile.sensorAddresses[mapping.sensorIndex];
    uint8_t& status = Wire.registers[address][CAP1188_REG_SENSOR_INPUT_STATUS];
    if (touched) status |= (1u << mapping.channel);
    else status &= ~(1u << mapping.channel);
    Wire.registers[address][CAP1188_REG_SENSOR_INPUT_DELTA_1 + mapping.channel] = touched ? 100 : 0;
}

static void expectEvent(uint8_t index, EventType type, uint32_t id, const char* position = "") {
    TEST_ASSERT_GREATER_THAN(index, g_count);
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(type), static_cast<uint8_t>(g_events[index].type));
    TEST_ASSERT_EQUAL_UINT32(id, g_events[index].commandId);
    TEST_ASSERT_EQUAL_STRING(position, g_events[index].position);
}

void setUp() {
    selectBoardProfile("v1");
    Wire = TwoWire{};
    g_now = 1000;
    g_count = 0;
    g_rejectEvents = false;
    g_eventMutexBusy = false;
    g_afterCapture = nullptr;
    g_touch = TouchController{};
    g_touch.setEventQueue(&g_queue);
    for (uint8_t s = 0; s < activeBoardProfile().sensorCount; s++) {
        Wire.registers[activeBoardProfile().sensorAddresses[s]][CAP1188_REG_PRODUCT_ID] = 0x50;
    }
    TEST_ASSERT_TRUE(g_touch.begin());
}
void tearDown() {}

void test_next_hold_grabbed_before_expect_any_is_not_lost() {
    g_touch.setExpectAny(10);
    contact(0, true);
    advance(200);
    expectEvent(0, EventType::TOUCHED, 10, "H01");
    contact(0, false);
    contact(1, true);  // Pi has not rearmed yet; first hold is still release-latched.
    advance(200);
    TEST_ASSERT_EQUAL_UINT8(1, g_count);
    g_touch.setExpectAny(11);
    advance(10);
    expectEvent(1, EventType::TOUCHED, 11, "H02");
}

void test_dropout_recovers_without_waiting_for_full_release_latch() {
    g_touch.setExpectAny(20);
    contact(0, true);
    advance(50);
    contact(0, false);
    advance(EXPECT_ANY_DROPOUT_SAMPLES * TOUCH_POLL_INTERVAL_MS);
    contact(0, true);
    advance(200);
    expectEvent(0, EventType::TOUCHED, 20, "H01");
}

void test_expect_and_expect_any_cannot_consume_the_same_grab() {
    g_touch.setExpectAny(30);
    g_touch.setExpectDown(0, 31);
    contact(0, true);
    advance(200);
    TEST_ASSERT_EQUAL_UINT8(1, g_count);
    expectEvent(0, EventType::TOUCHED, 31, "H01");
    contact(1, true);
    advance(200);
    expectEvent(1, EventType::TOUCHED, 30, "H02");
}

void test_released_hold_can_be_reused_with_expect_any_still_queued() {
    g_touch.setExpectAny(40);
    g_touch.setExpectAny(41);
    contact(0, true);
    advance(200);
    contact(0, false);
    advance(TOUCH_DEBOUNCE_RELEASE_MS + 10);
    contact(0, true);
    advance(200);
    expectEvent(1, EventType::TOUCHED, 41, "H01");
}

void test_excluded_candidate_survives_until_eligible_expectation() {
    HoldMask excluded;
    excluded.clear();
    excluded.set(0);
    g_touch.setExpectAnyExcept(excluded, 50);
    g_touch.setExpectAny(51);
    contact(0, true);
    advance(200);
    TEST_ASSERT_EQUAL_UINT8(0, g_count);
    contact(1, true);
    advance(200);
    advance(10);
    expectEvent(0, EventType::TOUCHED, 50, "H02");
    expectEvent(1, EventType::TOUCHED, 51, "H01");
}

void test_release_does_not_block_another_holds_press() {
    g_touch.setExpectDown(0, 60);
    contact(0, true);
    advance(5);
    g_touch.setExpectUp(0, 61);
    contact(0, false);
    g_touch.setExpectDown(1, 62);
    contact(1, true);
    advance(5);
    expectEvent(1, EventType::TOUCHED, 62, "H02");
    advance(TOUCH_DEBOUNCE_RELEASE_MS);
    expectEvent(2, EventType::TOUCH_RELEASED, 61, "H01");
}

void test_consumed_grab_is_not_replayed_after_noise_or_rearming() {
    g_touch.setExpectAny(70);
    contact(0, true);
    advance(200);
    contact(0, false);
    advance(20);
    contact(0, true);
    g_touch.setExpectAny(71);
    g_touch.setExpectDown(0, 72);
    advance(250);
    TEST_ASSERT_EQUAL_UINT8(1, g_count);
}

void test_brush_by_and_stale_grabs_are_not_reported() {
    contact(0, true);
    advance(20);
    contact(0, false);
    advance(TOUCH_DEBOUNCE_RELEASE_MS + 10);
    contact(1, true);
    advance(EXPECT_HELD_FRESH_MS + 10);
    g_touch.setExpectAny(80);
    advance(200);
    TEST_ASSERT_EQUAL_UINT8(0, g_count);
}

void test_clean_queue_does_not_replay_an_already_held_candidate() {
    contact(0, true);
    advance(50);
    g_touch.clearAllExpectations();
    g_touch.setExpectAny(90);
    advance(200);
    TEST_ASSERT_EQUAL_UINT8(0, g_count);
}

void test_failed_status_reads_cannot_confirm_a_release() {
    contact(0, true);
    advance(5);
    g_touch.setExpectUp(0, 100);
    contact(0, false);
    advance(5);
    uint8_t address = activeBoardProfile().sensorAddresses[activeBoardProfile().inputMappings[0].sensorIndex];
    Wire.failStatus[address] = true;
    advance(TOUCH_DEBOUNCE_RELEASE_MS + 10);
    TEST_ASSERT_TRUE(g_touch.isTouched(0));
    TEST_ASSERT_EQUAL_UINT8(0, g_count);
    Wire.failStatus[address] = false;
    advance(TOUCH_DEBOUNCE_RELEASE_MS + 10);
    expectEvent(0, EventType::TOUCH_RELEASED, 100, "H01");
}

void test_v2_eighth_sensor_channel_is_enabled_and_detected() {
    selectBoardProfile("v2");
    TEST_ASSERT_TRUE(g_touch.begin());
    const InputMapping& mapping = activeBoardProfile().inputMappings[31];
    uint8_t address = activeBoardProfile().sensorAddresses[mapping.sensorIndex];
    TEST_ASSERT_EQUAL_UINT8(7, mapping.channel);
    TEST_ASSERT_BITS_HIGH(1u << mapping.channel, Wire.registers[address][CAP1188_REG_SENSOR_INPUT_ENABLE]);
    g_touch.setExpectDown(31, 110);
    contact(31, true);
    advance(5);
    expectEvent(0, EventType::TOUCHED, 110, "H32");
}

void test_handoff_preserves_occupancy_until_last_hold_releases() {
    contact(0, true);
    advance(5);
    g_touch.setHandsOffDetection(true, 120);
    expectEvent(0, EventType::HANDS_ON, 120);
    contact(0, false);
    contact(1, true);
    advance(TOUCH_DEBOUNCE_RELEASE_MS + 10);
    TEST_ASSERT_EQUAL_UINT8(1, g_count);
    contact(1, false);
    advance(TOUCH_DEBOUNCE_RELEASE_MS);
    TEST_ASSERT_EQUAL_UINT8(1, g_count);
    advance(10);
    expectEvent(1, EventType::HANDS_OFF, COMMAND_ID_NONE);
}

void test_two_simultaneous_grabs_are_delivered_across_rearming() {
    g_touch.setExpectAny(130);
    contact(0, true);
    contact(1, true);
    advance(200);
    TEST_ASSERT_EQUAL_UINT8(1, g_count);
    g_touch.setExpectAny(131);
    advance(10);
    expectEvent(0, EventType::TOUCHED, 130, "H01");
    expectEvent(1, EventType::TOUCHED, 131, "H02");
}

void test_release_and_next_press_work_across_millis_wrap() {
    g_now = UINT32_MAX - 100;
    g_touch.setExpectDown(0, 140);
    contact(0, true);
    advance(5);
    g_touch.setExpectUp(0, 141);
    contact(0, false);
    advance(TOUCH_DEBOUNCE_RELEASE_MS + 10);
    g_touch.setExpectDown(1, 142);
    contact(1, true);
    advance(5);
    expectEvent(1, EventType::TOUCH_RELEASED, 141, "H01");
    expectEvent(2, EventType::TOUCHED, 142, "H02");
}

void test_already_held_expect_retries_rejected_delivery() {
    contact(0, true);
    advance(20);
    g_rejectEvents = true;
    g_touch.setExpectDown(0, 150);
    advance(20);
    TEST_ASSERT_EQUAL_UINT8(0, g_count);
    g_rejectEvents = false;
    advance(10);
    expectEvent(0, EventType::TOUCHED, 150, "H01");
    advance(100);
    TEST_ASSERT_EQUAL_UINT8(1, g_count);
}

void test_press_report_survives_queue_pressure_and_release() {
    g_touch.setExpectDown(0, 160);
    g_rejectEvents = true;
    contact(0, true);
    advance(10);
    contact(0, false);
    advance(EXPECT_HELD_FRESH_MS + TOUCH_DEBOUNCE_RELEASE_MS);
    g_rejectEvents = false;
    advance(10);
    expectEvent(0, EventType::TOUCHED, 160, "H01");
}

void test_release_report_survives_queue_pressure_and_regrab() {
    contact(0, true);
    advance(10);
    g_touch.setExpectUp(0, 170);
    g_rejectEvents = true;
    contact(0, false);
    advance(TOUCH_DEBOUNCE_RELEASE_MS + 10);
    contact(0, true);
    advance(10);
    g_rejectEvents = false;
    advance(10);
    expectEvent(0, EventType::TOUCH_RELEASED, 170, "H01");
}

void test_expect_does_not_count_a_release_latched_hold() {
    contact(0, true);
    advance(10);
    contact(0, false);
    advance(10);
    g_touch.setExpectDown(0, 180);
    advance(10);
    TEST_ASSERT_EQUAL_UINT8(0, g_count);
    advance(TOUCH_DEBOUNCE_RELEASE_MS);
    contact(0, true);
    advance(10);
    expectEvent(0, EventType::TOUCHED, 180, "H01");
}

void test_expect_any_retries_qualified_touch_after_queue_pressure() {
    g_touch.setExpectAny(190);
    g_rejectEvents = true;
    contact(0, true);
    advance(200);
    contact(0, false);
    advance(EXPECT_HELD_FRESH_MS + TOUCH_DEBOUNCE_RELEASE_MS);
    g_rejectEvents = false;
    advance(10);
    expectEvent(0, EventType::TOUCHED, 190, "H01");
}

void test_clean_queue_cancels_reports_waiting_for_delivery() {
    g_touch.setExpectDown(0, 200);
    g_touch.setExpectAny(201);
    contact(0, true);
    contact(1, true);
    g_rejectEvents = true;
    advance(200);
    g_touch.clearAllExpectations();
    g_rejectEvents = false;
    advance(10);
    TEST_ASSERT_EQUAL_UINT8(0, g_count);
}

static void rearmDuringDelivery() { g_touch.setExpectDown(0, 211); }

void test_rearming_during_delivery_does_not_erase_new_expectation() {
    g_touch.setExpectDown(0, 210);
    g_afterCapture = rearmDuringDelivery;
    contact(0, true);
    advance(10);
    contact(0, false);
    advance(TOUCH_DEBOUNCE_RELEASE_MS + 10);
    contact(0, true);
    advance(10);
    expectEvent(0, EventType::TOUCHED, 210, "H01");
    expectEvent(1, EventType::TOUCHED, 211, "H01");
}

void test_already_released_expect_retries_rejected_delivery() {
    g_rejectEvents = true;
    g_touch.setExpectUp(0, 220);
    advance(10);
    g_rejectEvents = false;
    advance(10);
    expectEvent(0, EventType::TOUCH_RELEASED, 220, "H01");
}

void test_new_expect_waits_for_a_valid_sensor_status() {
    contact(0, true);
    advance(10);
    uint8_t address = activeBoardProfile().sensorAddresses[activeBoardProfile().inputMappings[0].sensorIndex];
    Wire.failStatus[address] = true;
    advance(10);
    g_touch.setExpectDown(0, 230);
    advance(10);
    TEST_ASSERT_EQUAL_UINT8(0, g_count);
    Wire.failStatus[address] = false;
    advance(10);
    expectEvent(0, EventType::TOUCHED, 230, "H01");
}

void test_stale_or_consumed_holds_do_not_complete_new_expect() {
    contact(0, true);
    advance(EXPECT_HELD_FRESH_MS + 10);
    g_touch.setExpectDown(0, 240);
    advance(10);
    TEST_ASSERT_EQUAL_UINT8(0, g_count);
    contact(0, false);
    advance(TOUCH_DEBOUNCE_RELEASE_MS + 10);
    contact(0, true);
    advance(10);
    expectEvent(0, EventType::TOUCHED, 240, "H01");
    g_touch.setExpectDown(0, 241);
    advance(10);
    TEST_ASSERT_EQUAL_UINT8(1, g_count);
}

void test_intermittent_read_errors_do_not_pin_a_consumed_hold() {
    g_touch.setExpectDown(0, 250);
    contact(0, true);
    advance(10);
    expectEvent(0, EventType::TOUCHED, 250, "H01");
    g_touch.setExpectUp(0, 251);
    contact(0, false);
    uint8_t address = activeBoardProfile().sensorAddresses[activeBoardProfile().inputMappings[0].sensorIndex];
    // One failed poll every 200ms: the old full-timer reset never releases,
    // however many seconds the hold has actually been untouched.
    for (uint8_t cycle = 0; cycle < 6; cycle++) {
        Wire.failStatus[address] = false;
        advance(195);
        Wire.failStatus[address] = true;
        advance(5);
    }
    Wire.failStatus[address] = false;
    advance(10);
    TEST_ASSERT_FALSE(g_touch.isTouched(0));
    expectEvent(1, EventType::TOUCH_RELEASED, 251, "H01");
    g_touch.setExpectDown(0, 252);
    contact(0, true);
    advance(10);
    expectEvent(2, EventType::TOUCHED, 252, "H01");
}

void test_event_mutex_contention_does_not_block_touch_polling() {
    g_touch.setExpectDown(0, 260);
    g_touch.setExpectDown(1, 261);
    contact(0, true);
    contact(1, true);
    g_eventMutexBusy = true;
    uint32_t before = g_now;
    advance(5);
    TEST_ASSERT_EQUAL_UINT32(TOUCH_POLL_INTERVAL_MS, g_now - before);
    g_eventMutexBusy = false;
    advance(10);
    expectEvent(0, EventType::TOUCHED, 260, "H01");
    expectEvent(1, EventType::TOUCHED, 261, "H02");
}

void test_replay_does_not_poll_recording_deltas() {
    g_touch.setExpectDown(0, 270);
    contact(1, true);  // Resting on another hold while the lit one is armed.
    advance(300);
    contact(0, true);
    advance(10);
    expectEvent(0, EventType::TOUCHED, 270, "H01");
    TEST_ASSERT_EQUAL_UINT32(0, Wire.deltaReads);
}

void test_observed_recontact_restarts_release_evidence() {
    contact(0, true);
    advance(10);
    g_touch.setExpectUp(0, 280);
    contact(0, false);
    advance(300);
    contact(0, true);
    advance(5);
    contact(0, false);
    advance(300);
    TEST_ASSERT_TRUE(g_touch.isTouched(0));
    TEST_ASSERT_EQUAL_UINT8(0, g_count);
    advance(TOUCH_DEBOUNCE_RELEASE_MS);
    expectEvent(0, EventType::TOUCH_RELEASED, 280, "H01");
}

void test_bus_stall_is_not_credited_as_release_evidence() {
    contact(0, true);
    advance(10);
    contact(0, false);
    advance(10);
    g_now += 5000;
    advance(5);
    TEST_ASSERT_TRUE(g_touch.isTouched(0));
}

void test_normal_scan_overruns_do_not_lengthen_release_latch() {
    contact(0, true);
    advance(10);
    g_touch.setExpectUp(0, 281);
    contact(0, false);
    advance(5); // First untouched observation starts the latch.
    for (uint16_t elapsed = 0; elapsed < TOUCH_DEBOUNCE_RELEASE_MS; elapsed += 10) {
        g_now += 10; // A realistic scan can exceed the nominal 5ms schedule.
        g_touch.tick();
    }
    TEST_ASSERT_FALSE(g_touch.isTouched(0));
    expectEvent(0, EventType::TOUCH_RELEASED, 281, "H01");
}

void test_touch_state_explains_consumed_and_release_latched_input() {
    g_touch.setExpectDown(0, 290);
    contact(0, true);
    advance(10);
    g_touch.setExpectDown(0, 291);
    contact(0, false);
    advance(10);
    g_touch.requestTouchState(0, 292);
    advance(5);
    expectEvent(1, EventType::TOUCH_STATE, 292, "H01");
    TEST_ASSERT_NOT_NULL(strstr(g_events[1].extra, "active=1 valid=1 raw=0 held=1 used=1"));
    TEST_ASSERT_NOT_NULL(strstr(g_events[1].extra, "expect=1 pending=0"));
    advance(10);
    TEST_ASSERT_EQUAL_UINT8(2, g_count);
}

void test_touch_state_retries_without_reading_deltas_or_consuming_touch() {
    contact(0, true);
    g_touch.requestTouchState(0, 300);
    g_eventMutexBusy = true;
    advance(10);
    TEST_ASSERT_EQUAL_UINT8(0, g_count);
    g_eventMutexBusy = false;
    advance(5);
    expectEvent(0, EventType::TOUCH_STATE, 300, "H01");
    TEST_ASSERT_NOT_NULL(strstr(g_events[0].extra, "raw=1 held=1 used=0"));
    TEST_ASSERT_EQUAL_UINT32(0, Wire.deltaReads);
    g_touch.setExpectDown(0, 301);
    advance(5);
    expectEvent(1, EventType::TOUCHED, 301, "H01");
}

void test_occupancy_delivery_retries_without_blocking_poll() {
    contact(0, true);
    advance(10);
    g_touch.setHandsOffDetection(true, 310);
    contact(0, false);
    g_eventMutexBusy = true;
    advance(TOUCH_DEBOUNCE_RELEASE_MS + 10);
    TEST_ASSERT_EQUAL_UINT8(1, g_count);
    uint32_t before = g_now;
    advance(5);
    TEST_ASSERT_EQUAL_UINT32(TOUCH_POLL_INTERVAL_MS, g_now - before);
    g_eventMutexBusy = false;
    advance(5);
    expectEvent(1, EventType::HANDS_OFF, COMMAND_ID_NONE);
}

void test_hold_time_ignores_repeated_short_sensor_dropouts() {
    g_touch.setExpectDown(0, 320);
    contact(0, true);
    advance(10);
    expectEvent(0, EventType::TOUCHED, 320, "H01");
    g_touch.setExpectUp(0, 321);

    // More than 500ms of negatives in TOTAL must not reset a hold-time timer.
    // Each confirmed contact breaks the release streak, even for one poll.
    const uint16_t dropouts[] = {5, 20, 100, 300, 495};
    for (uint8_t cycle = 0; cycle < 4; cycle++) {
        for (uint16_t dropout : dropouts) {
            contact(0, false);
            advance(dropout);
            TEST_ASSERT_TRUE(g_touch.isTouched(0));
            TEST_ASSERT_EQUAL_UINT8(1, g_count);
            contact(0, true);
            advance(TOUCH_POLL_INTERVAL_MS);
        }
    }

    // Only a sustained release may notify the Pi to restart its countdown.
    contact(0, false);
    advance(TOUCH_DEBOUNCE_RELEASE_MS + TOUCH_POLL_INTERVAL_MS);
    TEST_ASSERT_FALSE(g_touch.isTouched(0));
    expectEvent(1, EventType::TOUCH_RELEASED, 321, "H01");
    advance(500);
    TEST_ASSERT_EQUAL_UINT8(2, g_count);
}

void test_hold_time_release_armed_during_dropout_does_not_fire_early() {
    g_touch.setExpectDown(0, 330);
    contact(0, true);
    advance(10);
    contact(0, false);
    advance(10);
    // SUCCESS may finish and the Pi arm EXPECT_RELEASE during a noisy sample.
    g_touch.setExpectUp(0, 331);
    advance(100);
    TEST_ASSERT_TRUE(g_touch.isTouched(0));
    TEST_ASSERT_EQUAL_UINT8(1, g_count);
    contact(0, true);
    advance(TOUCH_POLL_INTERVAL_MS);
    contact(0, false);
    advance(400);
    TEST_ASSERT_EQUAL_UINT8(1, g_count);
    advance(TOUCH_DEBOUNCE_RELEASE_MS);
    expectEvent(1, EventType::TOUCH_RELEASED, 331, "H01");
}

void test_double_hold_time_filters_each_holds_dropouts_independently() {
    g_touch.setExpectDown(0, 340);
    g_touch.setExpectDown(1, 341);
    contact(0, true);
    contact(1, true);
    advance(10);
    g_touch.setExpectUp(0, 342);
    g_touch.setExpectUp(1, 343);
    contact(0, false); // Noise on H01.
    contact(1, false); // Real release on H02.
    advance(200);
    contact(0, true);
    advance(TOUCH_DEBOUNCE_RELEASE_MS);
    TEST_ASSERT_TRUE(g_touch.isTouched(0));
    TEST_ASSERT_FALSE(g_touch.isTouched(1));
    TEST_ASSERT_EQUAL_UINT8(3, g_count);
    expectEvent(2, EventType::TOUCH_RELEASED, 343, "H02");
    contact(0, false);
    advance(TOUCH_DEBOUNCE_RELEASE_MS + TOUCH_POLL_INTERVAL_MS);
    expectEvent(3, EventType::TOUCH_RELEASED, 342, "H01");
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_next_hold_grabbed_before_expect_any_is_not_lost);
    RUN_TEST(test_dropout_recovers_without_waiting_for_full_release_latch);
    RUN_TEST(test_expect_and_expect_any_cannot_consume_the_same_grab);
    RUN_TEST(test_released_hold_can_be_reused_with_expect_any_still_queued);
    RUN_TEST(test_excluded_candidate_survives_until_eligible_expectation);
    RUN_TEST(test_release_does_not_block_another_holds_press);
    RUN_TEST(test_consumed_grab_is_not_replayed_after_noise_or_rearming);
    RUN_TEST(test_brush_by_and_stale_grabs_are_not_reported);
    RUN_TEST(test_clean_queue_does_not_replay_an_already_held_candidate);
    RUN_TEST(test_failed_status_reads_cannot_confirm_a_release);
    RUN_TEST(test_v2_eighth_sensor_channel_is_enabled_and_detected);
    RUN_TEST(test_handoff_preserves_occupancy_until_last_hold_releases);
    RUN_TEST(test_two_simultaneous_grabs_are_delivered_across_rearming);
    RUN_TEST(test_release_and_next_press_work_across_millis_wrap);
    RUN_TEST(test_already_held_expect_retries_rejected_delivery);
    RUN_TEST(test_press_report_survives_queue_pressure_and_release);
    RUN_TEST(test_release_report_survives_queue_pressure_and_regrab);
    RUN_TEST(test_expect_does_not_count_a_release_latched_hold);
    RUN_TEST(test_expect_any_retries_qualified_touch_after_queue_pressure);
    RUN_TEST(test_clean_queue_cancels_reports_waiting_for_delivery);
    RUN_TEST(test_rearming_during_delivery_does_not_erase_new_expectation);
    RUN_TEST(test_already_released_expect_retries_rejected_delivery);
    RUN_TEST(test_new_expect_waits_for_a_valid_sensor_status);
    RUN_TEST(test_stale_or_consumed_holds_do_not_complete_new_expect);
    RUN_TEST(test_intermittent_read_errors_do_not_pin_a_consumed_hold);
    RUN_TEST(test_event_mutex_contention_does_not_block_touch_polling);
    RUN_TEST(test_replay_does_not_poll_recording_deltas);
    RUN_TEST(test_observed_recontact_restarts_release_evidence);
    RUN_TEST(test_bus_stall_is_not_credited_as_release_evidence);
    RUN_TEST(test_normal_scan_overruns_do_not_lengthen_release_latch);
    RUN_TEST(test_touch_state_explains_consumed_and_release_latched_input);
    RUN_TEST(test_touch_state_retries_without_reading_deltas_or_consuming_touch);
    RUN_TEST(test_occupancy_delivery_retries_without_blocking_poll);
    RUN_TEST(test_hold_time_ignores_repeated_short_sensor_dropouts);
    RUN_TEST(test_hold_time_release_armed_during_dropout_does_not_fire_early);
    RUN_TEST(test_double_hold_time_filters_each_holds_dropouts_independently);
    return UNITY_END();
}

#endif // SEQUENZBOARD_NATIVE_TEST