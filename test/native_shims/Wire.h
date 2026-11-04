/**
 * @file Wire.h
 * @brief Register-backed I2C double for native touch-controller regression tests.
 */
#ifndef WIRE_H_NATIVE_SHIM
#define WIRE_H_NATIVE_SHIM

#include <stdint.h>
#include <string.h>

class TwoWire {
public:
    uint8_t registers[128][256] = {};
    bool failStatus[128] = {};
    uint32_t deltaReads = 0;

    void begin(uint8_t, uint8_t) {}
    void setClock(uint32_t) {}
    void beginTransmission(uint8_t address) { m_address = address; m_written = 0; }
    void write(uint8_t value) {
        if (m_written++ == 0) m_register = value;
        else registers[m_address][m_register] = value;
    }
    uint8_t endTransmission() { return 0; }
    uint8_t requestFrom(uint8_t address, uint8_t) {
        if (m_register >= 0x10 && m_register <= 0x17) deltaReads++;
        return (m_register == 0x03 && failStatus[address]) ? 0 : 1;
    }
    uint8_t read() { return registers[m_address][m_register]; }

private:
    uint8_t m_address = 0;
    uint8_t m_register = 0;
    uint8_t m_written = 0;
};

extern TwoWire Wire;
#endif