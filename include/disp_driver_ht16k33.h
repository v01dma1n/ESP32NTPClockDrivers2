// disp_driver_ht16k33.h — IDF IDisplayDriver for HT16K33-driven LED matrix / 7-segment displays.
//
// The HT16K33 is an I2C LED controller with 16 rows × 8 columns of RAM.
// This driver uses it as a 7-segment display of up to HT16K33_MAX_DISP_SIZE digits.
// Each cell maps one buffer entry (uint16_t) to one 8-bit segment register pair.
//
// The I2C port is configured inside begin(). If the port has already been
// installed by another driver (e.g. Ds1307Driver on the same bus), begin()
// tolerates ESP_ERR_INVALID_STATE and proceeds.

#pragma once

#include "i_display_driver.h"
#include "driver/i2c.h"

#include <cstdint>
#include <vector>

#define HT16K33_I2C_DEF_ADR    0x70
#define HT16K33_DEF_DISP_SIZE  8
#define HT16K33_MAX_DISP_SIZE  16
#define HT16K33_MAX_BRIGHTNESS 15
#define HT16K33_DEF_BRIGHTNESS  7

class DispDriverHT16K33 : public IDisplayDriver {
public:
    // port: IDF I2C port number (I2C_NUM_0 or I2C_NUM_1).
    // sda / scl: GPIO pin numbers. Ignored if the port is already installed.
    DispDriverHT16K33(i2c_port_t port, int sda, int scl,
                      uint8_t i2c_addr    = HT16K33_I2C_DEF_ADR,
                      int     displaySize = HT16K33_DEF_DISP_SIZE);
    ~DispDriverHT16K33() override = default;

    // --- IDisplayDriver ------------------------------------------------------
    void begin() override;
    int  getDisplaySize() override { return _displaySize; }
    void setBrightness(uint8_t level) override;
    void clear() override;
    void setChar(int position, char character, bool dot = false) override;
    void setSegments(int position, uint16_t mask) override;
    void setDot(int position, bool on) override;

    unsigned long mapAsciiToSegment(char ascii_char, bool dot) override;
    void setBuffer(const std::vector<unsigned long>& newBuffer) override;
    void getFrameData(unsigned long* buffer) override;

    // Pushes the frame buffer to hardware. Safe to call from any task.
    void writeDisplay() override;

    // HT16K33 handles multiplexing internally; no background task needed.
    bool needsContinuousUpdate() const override { return false; }

private:
    void _writeCmd(uint8_t cmd);

    i2c_port_t _port;
    int        _sda, _scl;
    uint8_t    _i2c_addr;
    int        _displaySize;
    bool       _initialized;

    // Digits are stored right-to-left in HT16K33 RAM.
    uint16_t _displayBuffer[HT16K33_MAX_DISP_SIZE];

    // 7-segment font: indices 0..58 map to ASCII 0x20 (' ') through 0x5A ('Z').
    static const uint8_t s_font[];
};
