// disp_driver_max6921.h — IDF IDisplayDriver for a MAX6921-driven VFD.
//
// The MAX6921 is a 20-output, serially-loaded VFD driver that requires
// external multiplexing: the host must cycle through each grid (digit)
// at display refresh rate (~100 Hz for a 10-digit display at 10ms/digit).
//
// This driver combines the logical display buffer (char → segment mapping)
// with the hardware SPI + GPIO control, following the IDF pattern used by
// DispDriverPT6315 in MoodWhisperer.
//
// Since it needs external multiplexing, needsContinuousUpdate() returns
// true. The main application spawns a high-priority RefreshTask on Core 1
// that calls writeNextDigit() every 1 ms, cycling through all digits at
// ~100 Hz total.

#pragma once

#include "i_display_driver.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"

#include <cstdint>
#include <vector>

// Hardware-specific bitmask layout for one VFD tube position.
// Each field is a 20-bit mask (MAX6921 has OUT0..OUT19).
struct Max6921SegmentMap {
    unsigned long segA;
    unsigned long segB;
    unsigned long segC;
    unsigned long segD;
    unsigned long segE;
    unsigned long segF;
    unsigned long segG;
    unsigned long segDot;
};

class DispDriverMAX6921 : public IDisplayDriver {
public:
    // sclk/miso/mosi/ss: SPI bus pins (MISO is unused but declared for bus init).
    // blank: MAX6921 BLANK pin — drive HIGH to suppress output, LOW to show.
    // gridMap: array of 20-bit grid-enable masks, one per digit position.
    // digitCount: length of gridMap (and the display's character count).
    // segMap: per-display wiring of segment lines to OUT0..OUT19.
    // host: which IDF SPI peripheral to use (defaults to SPI3_HOST = VSPI).
    DispDriverMAX6921(int sclk, int miso, int mosi, int ss, int blank,
                      const unsigned long* gridMap, int digitCount,
                      const Max6921SegmentMap& segMap,
                      spi_host_device_t host = SPI3_HOST);
    ~DispDriverMAX6921() override;

    // --- IDisplayDriver ------------------------------------------------------
    void begin() override;
    int  getDisplaySize() override { return _digitCount; }
    void setBrightness(uint8_t level) override {}  // MAX6921 has no brightness control
    void clear() override;
    void setChar(int position, char character, bool dot = false) override;
    void setSegments(int position, uint16_t mask) override;
    void setDot(int position, bool on) override;

    unsigned long mapAsciiToSegment(char ascii_char, bool dot) override;
    void setBuffer(const std::vector<unsigned long>& newBuffer) override;

    // Flush is a no-op — the RefreshTask drives the glass via writeNextDigit().
    void writeDisplay() override {}

    // Called from the high-priority RefreshTask every 1 ms.
    // Sends the next digit's data over SPI and advances the digit counter.
    void writeNextDigit() override;

    bool needsContinuousUpdate() const override { return true; }

    // Copy frame buffer into caller-supplied array (getDisplaySize() elements).
    void getFrameData(unsigned long* buffer) override;

private:
    // Send 3 bytes (24 bits, MSB-first) over SPI.
    // The MAX6921 has a 20-bit shift register; bits [19:0] of `data` map
    // directly to OUT19..OUT0 after the chip latches on the LOAD edge.
    void spiSend(unsigned long data);

    // Translate a generic 8-bit segment mask (GENERIC_SEG_* flags)
    // and optional dot into a hardware 20-bit mask using _segMap.
    unsigned long mapGenericToHw(uint8_t mask, bool dot) const;

    // Retrieve the generic 8-bit mask for an ASCII character from the
    // built-in default 7-segment font table.
    static uint8_t charToGenericMask(char c);

    int _sclk, _miso, _mosi, _ss, _blank;
    spi_host_device_t   _host;
    spi_device_handle_t _dev;

    const unsigned long*    _gridMap;
    int                     _digitCount;
    const Max6921SegmentMap& _segMap;

    unsigned long* _buffer;     // frame buffer, one word per digit
    int            _currentDigit;
    bool           _initialized;
};
