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
    bool failBus[128] = {};          // Every transaction to this address fails (NACK)
    uint8_t failCount[128] = {};     // Next N transactions to this address fail, then recover
    uint32_t deltaReads = 0;
    uint32_t endCalls = 0;           // Bus re-initializations

    void begin(uint8_t, uint8_t) {}
    void end() { endCalls++; }
    void setClock(uint32_t) {}
    void beginTransmission(uint8_t address) { m_address = address; m_written = 0; m_pending = false; }
    void write(uint8_t value) {
        if (m_written++ == 0) m_register = value;
        else { m_value = value; m_pending = true; }
    }
    uint8_t endTransmission() {
        if (failBus[m_address]) return 2;
        if (failCount[m_address] > 0) { failCount[m_address]--; return 2; }
        if (m_pending) registers[m_address][m_register] = m_value;
        return 0;
    }
    uint8_t requestFrom(uint8_t address, uint8_t) {
        if (failBus[address]) return 0;
        if (m_register >= 0x10 && m_register <= 0x17) deltaReads++;
        return (m_register == 0x03 && failStatus[address]) ? 0 : 1;
    }
    uint8_t read() { return registers[m_address][m_register]; }

private:
    uint8_t m_address = 0;
    uint8_t m_register = 0;
    uint8_t m_value = 0;
    uint8_t m_written = 0;
    bool m_pending = false;
};

extern TwoWire Wire;
#endif