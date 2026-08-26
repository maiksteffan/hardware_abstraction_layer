/**
 * @file Arduino.h
 * @brief Host-build stand-in, used only by `pio test -e native`.
 *
 * Config.h includes <Arduino.h> for its fixed-width integer types and nothing
 * else — it is a header of constants. This shim satisfies that include so the
 * board profile tables can be tested on a laptop instead of only on hardware.
 * It is never part of a firmware build.
 */

#ifndef ARDUINO_H_NATIVE_SHIM
#define ARDUINO_H_NATIVE_SHIM

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#endif // ARDUINO_H_NATIVE_SHIM
