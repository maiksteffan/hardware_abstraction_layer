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
    if (g_count >= 64) return false;
    Event& event = g_events[g_count++];
    event = {};
    event.type = type;
    event.commandId = id;
    if (position) snprintf(event.position, sizeof(event.position), "%s", position);
    return true;
}
bool EventQueue::queueTouched(const char* position, uint32_t id) {
    return capture(EventType::TOUCHED, position, id);
}
bool EventQueue::queueTouchReleased(const char* position, uint32_t id) {
    return capture(EventType::TOUCH_RELEASED, position, id);
}
bool EventQueue::queueHandsOn(uint32_t id) { return capture(EventType::HANDS_ON, nullptr, id); }
bool EventQueue::queueHandsOff(uint32_t id) { return capture(EventType::HANDS_OFF, nullptr, id); }
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
    return UNITY_END();
}

#endif // SEQUENZBOARD_NATIVE_TEST