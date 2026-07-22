/**
 * @file HoldGeometry.cpp
 * @brief Board coordinate table and spatial helpers
 *
 * Source coordinates (board units, origin bottom-left, X right, Y up) were
 * provided with two decimal places; they are rounded to whole units here —
 * sub-unit precision is irrelevant for adjacency checks and integer math
 * keeps this safe for the Core 0 real-time path.
 *
 * NOTE: table entry i corresponds to input "H{i+1:02}" — the SAME ordering
 * as INPUT_MAPPINGS in Config.h. If holds are ever renumbered, update both.
 */

#include "HoldGeometry.h"

static const HoldCoordinate HOLD_COORDINATES[INPUT_COUNT] = {
    { 1320, 660 },  // H01
    { 1130, 660 },  // H02
    {  940, 660 },  // H03
    {  750, 590 },  // H04
    {  560, 660 },  // H05
    {  370, 660 },  // H06
    {  180, 660 },  // H07
    { 1413, 520 },  // H08 (1412.50)
    { 1223, 520 },  // H09 (1222.50)
    { 1033, 520 },  // H10 (1032.50)
    {  904, 520 },  // H11 (903.91)
    {  596, 520 },  // H12 (596.09)
    {  463, 520 },  // H13 (462.50)
    {  273, 520 },  // H14 (272.50)
    {   83, 520 },  // H15 (82.50)
    { 1320, 380 },  // H16
    { 1130, 380 },  // H17
    {  940, 380 },  // H18
    {  750, 380 },  // H19
    {  560, 380 },  // H20
    {  370, 380 },  // H21
    {  180, 380 },  // H22
    { 1413, 240 },  // H23 (1412.50)
    { 1223, 240 },  // H24 (1222.50)
    { 1033, 240 },  // H25 (1032.50)
    {  463, 240 },  // H26 (462.50)
    {  273, 240 },  // H27 (272.50)
    {   83, 240 },  // H28 (82.50)
    { 1320, 100 },  // H29
    { 1130, 100 },  // H30
    {  940, 100 },  // H31
    {  560, 100 },  // H32
    {  370, 100 },  // H33
    {  180, 100 },  // H34
};

static_assert(sizeof(HOLD_COORDINATES) / sizeof(HOLD_COORDINATES[0]) == INPUT_COUNT,
              "HOLD_COORDINATES must have exactly INPUT_COUNT entries");

HoldCoordinate holdCoordinate(uint8_t inputIndex) {
    if (inputIndex >= INPUT_COUNT) {
        return HoldCoordinate{0, 0};
    }
    return HOLD_COORDINATES[inputIndex];
}

uint32_t squaredHoldDistance(uint8_t first, uint8_t second) {
    if (first >= INPUT_COUNT || second >= INPUT_COUNT) {
        return 0xFFFFFFFFu;
    }
    int32_t dx = (int32_t)HOLD_COORDINATES[first].x - (int32_t)HOLD_COORDINATES[second].x;
    int32_t dy = (int32_t)HOLD_COORDINATES[first].y - (int32_t)HOLD_COORDINATES[second].y;
    return (uint32_t)(dx * dx) + (uint32_t)(dy * dy);
}

bool areHoldsAdjacent(uint8_t first, uint8_t second) {
    if (first >= INPUT_COUNT || second >= INPUT_COUNT) return false;
    if (first == second) return false;
    return squaredHoldDistance(first, second) <= HOLD_ADJACENCY_DISTANCE_SQUARED;
}
