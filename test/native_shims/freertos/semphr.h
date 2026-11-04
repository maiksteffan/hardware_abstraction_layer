/**
 * @file semphr.h
 * @brief Opaque mutex handle for compiling the real EventQueue API on the host.
 */
#ifndef SEMPHR_H_NATIVE_SHIM
#define SEMPHR_H_NATIVE_SHIM
using SemaphoreHandle_t = void*;

#ifdef SEQUENZBOARD_EVENT_QUEUE_TEST
extern bool g_mutexAvailable;
extern TickType_t g_lastMutexTimeout;
extern uint32_t g_waitedMs;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return &g_mutexAvailable; }
inline void vSemaphoreDelete(SemaphoreHandle_t) {}
inline int xSemaphoreTake(SemaphoreHandle_t, TickType_t timeout) {
	g_lastMutexTimeout = timeout;
	if (g_mutexAvailable) return pdTRUE;
	g_waitedMs += timeout;
	return pdFALSE;
}
inline void xSemaphoreGive(SemaphoreHandle_t) {}
#endif
#endif