/**
 * @file Wire.h
 * @brief Register-backed I2C double for native touch-controller regression tests.
 *
 * Models what the firmware relies on of the ESP32 Wire library: every chip
 * keeps the register pointer of its last pointer write, a read returns the
 * register that pointer rests on, and the bus is claimed per transaction.
 * endTransmission(false) puts nothing on the bus and keeps it claimed; the
 * following requestFrom() then sends pointer and read as one transaction.
 * Whenever the bus is free between two transactions, onBusFree runs - the
 * place where, on hardware, a transaction of the other core can land.
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
    // Register each chip's pointer rests on.
    uint8_t pointer[128] = {};
    // The other core's turn on the bus; it reaches the chips through
    // `registers` and `pointer`, not through this object's transactions.
    void (*onBusFree)(TwoWire&) = nullptr;

    void begin(uint8_t, uint8_t) {}
    void setClock(uint32_t) {}
    void beginTransmission(uint8_t address) { m_address = address; m_written = 0; m_pending = false; }
    void write(uint8_t value) {
        if (m_written < sizeof(m_buffer)) m_buffer[m_written] = value;
        m_written++;
    }
    uint8_t endTransmission(bool sendStop = true) {
        if (!sendStop) {
            m_pending = true;
            return 0;
        }
        transmit();
        busFree();
        return 0;
    }
    uint8_t requestFrom(uint8_t address, uint8_t) {
        if (m_pending) {
            transmit();
            m_pending = false;
        }
        uint8_t reg = pointer[address];
        if (reg >= 0x10 && reg <= 0x17) deltaReads++;
        m_value = registers[address][reg];
        bool answered = !(reg == 0x03 && failStatus[address]);
        busFree();
        return answered ? 1 : 0;
    }
    uint8_t read() { return m_value; }

private:
    uint8_t m_address = 0;
    uint8_t m_buffer[2] = {};
    uint8_t m_written = 0;
    uint8_t m_value = 0;
    bool m_pending = false;

    // First byte: the register pointer. Second byte: the value written there.
    void transmit() {
        if (m_written >= 1) pointer[m_address] = m_buffer[0];
        if (m_written >= 2) registers[m_address][m_buffer[0]] = m_buffer[1];
    }
    void busFree() {
        if (onBusFree) onBusFree(*this);
    }
};

extern TwoWire Wire;
#endif
