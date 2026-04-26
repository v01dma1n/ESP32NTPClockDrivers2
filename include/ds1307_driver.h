#pragma once
#include "driver/i2c.h"
#include <cstdint>
#include <ctime>

// Minimal DS1307 RTC driver for ESP-IDF.
// Stores and retrieves UTC time. Uses the clock-halt bit (reg 0x00 bit 7)
// to detect "oscillator never started" (fresh battery, time never set).

class Ds1307Driver {
public:
    explicit Ds1307Driver(i2c_port_t port, int sda, int scl, uint8_t addr = 0x68);

    // Install I2C and probe the device. Returns false only if the chip
    // does not respond on I2C (not present or wrong wiring).
    bool init();

    // True if the oscillator is running (CH bit clear). False means the
    // battery was installed but writeTime() has never been called.
    bool isRunning();

    // Read UTC time. Returns (time_t)-1 if the oscillator is halted or
    // on I2C error.
    time_t readTime();

    // Write UTC time and start the oscillator (clears CH bit).
    bool writeTime(time_t t);

private:
    i2c_port_t _port;
    int        _sda, _scl;
    uint8_t    _addr;

    void    writeBurst(uint8_t reg, const uint8_t* src, uint8_t len);
    void    readBurst(uint8_t reg, uint8_t* dst, uint8_t len);

    static uint8_t fromBCD(uint8_t b) { return ((b >> 4) * 10) + (b & 0x0F); }
    static uint8_t toBCD(uint8_t n)   { return ((n / 10) << 4) | (n % 10); }
};
