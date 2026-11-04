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

#endif // ARDUINO_H_NATIVE_SHIM
