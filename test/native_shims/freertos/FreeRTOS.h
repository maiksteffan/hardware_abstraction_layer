/**
 * @file FreeRTOS.h
 * @brief Native placeholder; touch tests replace the event sink, not its mutexes.
 */
#ifndef FREERTOS_H_NATIVE_SHIM
#define FREERTOS_H_NATIVE_SHIM
// The native harness is single-threaded; callbacks exercise revision changes
// at queue delivery boundaries without pretending to test hardware spinlocks.
struct portMUX_TYPE {};
#define portMUX_INITIALIZER_UNLOCKED {}
inline void portENTER_CRITICAL(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL(portMUX_TYPE*) {}
#endif