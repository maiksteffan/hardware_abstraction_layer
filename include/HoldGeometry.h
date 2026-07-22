/**
 * @file HoldGeometry.h
 * @brief Compile-time board coordinates and spatial helpers for H01..H34
 *
 * Coordinates use integer board units, origin bottom-left, X right, Y up.
 * Used as supporting evidence by the touch intent classifier (a brief
 * contact directly adjacent to a strong grab is more plausibly incidental
 * than one on the far side of the wall).
 *
 * Arduino-free: compiled in native unit tests as well.
 */

#ifndef HOLD_GEOMETRY_H
#define HOLD_GEOMETRY_H

#include "Config.h"

struct HoldCoordinate {
    int16_t x;   // board units (source values rounded to integers)
    int16_t y;
};

// Squared Euclidean distance between two holds (input indices 0..33).
// Returns UINT32_MAX for out-of-range indices.
uint32_t squaredHoldDistance(uint8_t first, uint8_t second);

// True when the two holds are within HOLD_ADJACENCY_DISTANCE_SQUARED
// (direct or diagonal board neighbours). False for invalid indices.
bool areHoldsAdjacent(uint8_t first, uint8_t second);

// Access to the raw coordinate (for diagnostics/tests). Returns {0,0} for
// invalid indices.
HoldCoordinate holdCoordinate(uint8_t inputIndex);

#endif // HOLD_GEOMETRY_H
