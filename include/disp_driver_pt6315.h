// disp_driver_pt6315.h — IDisplayDriver adapter for the Sony/PT6315 board.
//
// The engine talks to displays through IDisplayDriver. This class wraps
// the low-level SonyVfdPt6315 and exposes the wide-frame abstraction
// (one unsigned-long frame word per cell) that animations build against.
//
// Unlike the MAX6921 driver, the PT6315 is *not* externally multiplexed:
// the chip handles multiplexing internally from a single RAM push. So
// needsContinuousUpdate() returns false and writeDisplay() is a real
// flush, not a no-op.

#pragma once

#include "i_display_driver.h"
#include "sony_vfd_pt6315.h"

#include <vector>
#include <cstdint>

class DispDriverPT6315 : public IDisplayDriver {
public:
    DispDriverPT6315(int sck_gpio, int cs_gpio, int mosi_gpio,
                     spi_host_device_t host = SPI3_HOST);
    ~DispDriverPT6315() override;

    // --- IDisplayDriver ----------------------------------------------------
    void begin() override;
    int  getDisplaySize() override { return SonyVfdConst::NUM_CHAR_POSITIONS; }
    void setBrightness(uint8_t level) override;
    void clear() override;
    void setChar(int position, char character, bool dot = false) override;
    void setSegments(int position, uint16_t mask) override;
    void setDot(int position, bool on) override;

    unsigned long mapAsciiToSegment(char ascii_char, bool dot) override;
    void setBuffer(const std::vector<unsigned long>& newBuffer) override;

    void writeDisplay() override;
    bool needsContinuousUpdate() const override { return false; }

    void getFrameData(unsigned long* buffer) override;

    // --- Passthroughs for the application ----------------------------------
    // Annunciator control — not part of IDisplayDriver because other
    // glass doesn't have named annunciators. Apps that specifically use
    // the Sony board cast their IDisplayDriver& back to this type to
    // call these. See WhispererApp::setupHardware() for usage.
    void setAnnunciator(SonyVfdAnnunciator ann, bool on);
    void clearAnnunciators();

private:
    SonyVfdPt6315 _vfd;
    std::vector<unsigned long> _buffer;
};
