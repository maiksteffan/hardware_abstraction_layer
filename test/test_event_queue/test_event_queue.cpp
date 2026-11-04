/**
 * @file test_event_queue.cpp
 * @brief Exercise production enqueue timeouts and serial formatting, not a fake event sink.
 */
#ifdef SEQUENZBOARD_NATIVE_TEST
#define SEQUENZBOARD_EVENT_QUEUE_TEST
#include <stdio.h>
#include <unity.h>
#include "EventQueue.h"
#include "../../src/BoardProfiles.cpp"
#include "../../src/EventQueue.cpp"

NativeSerial Serial;
bool g_mutexAvailable = true;
TickType_t g_lastMutexTimeout = 0;
uint32_t g_waitedMs = 0;

void setUp() {
    Serial = NativeSerial{};
    g_mutexAvailable = true;
    g_lastMutexTimeout = 0;
    g_waitedMs = 0;
}
void tearDown() {}

void test_actual_touch_enqueue_never_waits_when_requested_nonblocking() {
    EventQueue queue;
    queue.begin();
    g_mutexAvailable = false;
    TEST_ASSERT_FALSE(queue.queueTouched("H01", 1, MUTEX_TIMEOUT_TOUCH_QUEUE_MS));
    TEST_ASSERT_FALSE(queue.queueTouchReleased("H01", 2, MUTEX_TIMEOUT_TOUCH_QUEUE_MS));
    TEST_ASSERT_FALSE(queue.queueHandsOn(COMMAND_ID_NONE, MUTEX_TIMEOUT_TOUCH_QUEUE_MS));
    TEST_ASSERT_FALSE(queue.queueHandsOff(COMMAND_ID_NONE, MUTEX_TIMEOUT_TOUCH_QUEUE_MS));
    TEST_ASSERT_FALSE(queue.queueTouchState("H01", "raw=1", 3));
    TEST_ASSERT_EQUAL_UINT32(0, g_waitedMs);
    TEST_ASSERT_TRUE(queue.isEmpty());
}

void test_command_replies_keep_the_existing_queue_timeout() {
    EventQueue queue;
    queue.begin();
    g_mutexAvailable = false;
    TEST_ASSERT_FALSE(queue.queueAck("EXPECT", "H01", 1));
    TEST_ASSERT_EQUAL_UINT32(MUTEX_TIMEOUT_QUEUE_MS, g_lastMutexTimeout);
    TEST_ASSERT_EQUAL_UINT32(MUTEX_TIMEOUT_QUEUE_MS, g_waitedMs);
}

void test_ack_touch_and_diagnostic_serialize_in_order_with_ids() {
    EventQueue queue;
    queue.begin();
    TEST_ASSERT_TRUE(queue.queueAck("EXPECT", "H01", 42));
    TEST_ASSERT_TRUE(queue.queueTouched("H01", 42, MUTEX_TIMEOUT_TOUCH_QUEUE_MS));
    TEST_ASSERT_TRUE(queue.queueAck("TOUCH_STATE", "H01", 43));
    TEST_ASSERT_TRUE(queue.queueTouchState("H01", "active=1 valid=1 raw=1", 43));
    queue.flush(QUEUE_SIZE_EVENTS);
    TEST_ASSERT_EQUAL_STRING(
        "ACK EXPECT H01 #42\nTOUCHED H01 #42\n"
        "ACK TOUCH_STATE H01 #43\nTOUCH_STATE H01 active=1 valid=1 raw=1 #43\n",
        Serial.output);
}

void test_queue_full_can_be_retried_without_changing_event_grammar() {
    EventQueue queue;
    queue.begin();
    for (uint8_t i = 0; i < QUEUE_SIZE_EVENTS; i++) TEST_ASSERT_TRUE(queue.queueAck("PING"));
    TEST_ASSERT_FALSE(queue.queueTouched("H02", 44, MUTEX_TIMEOUT_TOUCH_QUEUE_MS));
    queue.clear();
    TEST_ASSERT_TRUE(queue.queueTouched("H02", 44, MUTEX_TIMEOUT_TOUCH_QUEUE_MS));
    TEST_ASSERT_TRUE(queue.queueTouchReleased("H02", COMMAND_ID_NONE, MUTEX_TIMEOUT_TOUCH_QUEUE_MS));
    queue.flush(QUEUE_SIZE_EVENTS);
    TEST_ASSERT_EQUAL_STRING("TOUCHED H02 #44\nTOUCH_RELEASED H02\n", Serial.output);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_actual_touch_enqueue_never_waits_when_requested_nonblocking);
    RUN_TEST(test_command_replies_keep_the_existing_queue_timeout);
    RUN_TEST(test_ack_touch_and_diagnostic_serialize_in_order_with_ids);
    RUN_TEST(test_queue_full_can_be_retried_without_changing_event_grammar);
    return UNITY_END();
}
#endif