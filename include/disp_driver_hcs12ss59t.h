// disp_driver_hcs12ss59t.h — IDF IDisplayDriver for the HCS-12SS59T VFD controller.
//
// The HCS-12SS59T is a 12-digit VFD controller with an internal character ROM.
// It is driven over SPI (mode 3, LSB-first, 2 MHz). Display RAM is written
// one byte per cell via the DCRAM_WR command.
//
// Because the chip uses a character ROM rather than direct segment control,
// setSegments() maps to a space, setDot() is a no-op, and mapAsciiToSegment() /
// setBuffer() / getFrameData() behave as character-code pass-throughs.
//
// CS is driven manually (GPIO) to accommodate the 1 µs / 8 µs setup/hold
// timing requirements; spi_host must be initialized with spi_bus_initialize()
// before calling begin() — or begin() will initialize it if not already done.

#pragma once

#include "i_display_driver.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#include <cstdint>
#include <vector>

class DispDriverHCS12SS59T : public IDisplayDriver {
public:
    // sclk/mosi: SPI bus pins.  cs/reset/vdon: GPIO control pins.
    // host: which IDF SPI peripheral to use (defaults to SPI2_HOST).
    DispDriverHCS12SS59T(int sclk, int mosi, int cs, int reset, int vdon,
                         int displaySize,
                         spi_host_device_t host = SPI2_HOST);

    void begin() override;
    int  getDisplaySize() override { return _displaySize; }
    void setBrightness(uint8_t level) override;
    void clear() override;
    void setChar(int position, char character, bool dot = false) override;
    void setSegments(int position, uint16_t mask) override;
    void setDot(int /*position*/, bool /*on*/) override {}  // character-ROM VFD has no dot bit

    unsigned long mapAsciiToSegment(char ascii_char, bool dot) override;
    void setBuffer(const std::vector<unsigned long>& newBuffer) override;
    void getFrameData(unsigned long* buffer) override;

    void writeDisplay() override;
    bool needsContinuousUpdate() const override { return false; }

private:
    void    _supplyOn();
    void    _writeByte(uint8_t byte);
    void    _sendCmd(uint8_t cmd, uint8_t arg);
    uint8_t _getCode(char c);

    int               _sclk, _mosi, _cs, _reset, _vdon;
    int               _displaySize;
    spi_host_device_t _spi_host;
    spi_device_handle_t _spi_handle;
    bool              _initialized;
    uint8_t*          _displayBuffer;
};
