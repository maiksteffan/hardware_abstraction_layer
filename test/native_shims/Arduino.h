/**
 * @file Arduino.h
 * @brief Host-build stand-in, used only by `pio test -e native`.
 *
 * Supplies the types needed by Config.h and clock declarations implemented by
 * the touch test harness. Never part of a firmware build.
 */

#ifndef ARDUINO_H_NATIVE_SHIM
#define ARDUINO_H_NATIVE_SHIM

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Controlled by the touch-controller host tests; never linked on the MCU.
uint32_t millis();
void delay(uint32_t milliseconds);
inline void delayMicroseconds(uint32_t) {}

#ifdef SEQUENZBOARD_EVENT_QUEUE_TEST
// Only the real EventQueue test suite needs a serial sink. Other suites use
// their existing captured-event doubles and never format serial output.
struct NativeSerial {
	char output[2048] = {};
	size_t length = 0;
	void write(const char* data, size_t count) {
		size_t available = sizeof(output) - length - 1;
		if (count > available) count = available;
		memcpy(output + length, data, count);
		length += count;
		output[length] = '\0';
	}
	void flush() {}
};
extern NativeSerial Serial;
#endif

#endif // ARDUINO_H_NATIVE_SHIM
