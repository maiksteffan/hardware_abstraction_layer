/**
 * @file semphr.h
 * @brief Opaque mutex handle for compiling the real EventQueue API on the host.
 */
#ifndef SEMPHR_H_NATIVE_SHIM
#define SEMPHR_H_NATIVE_SHIM
using SemaphoreHandle_t = void*;
#endif