// disp_driver_lgl_vfd.h — IDisplayDriver adapter for the LGL Studio VFD.
//
// Unlike PT6315/MAX6921, this glass has no independent segment control —
// it's a font-ROM alphanumeric display that only ever takes ASCII bytes.
// So the "frame word" per cell is just the ASCII character value, and
// setSegments()/setDot() degrade to that same model (see the .cpp for
// what each one actually does). Like PT6315, the chip drives its own
// multiplexing from a single RAM push, so needsContinuousUpdate() is false
// and writeDisplay() is a real flush, not a no-op.

#pragma once

#include "i_display_driver.h"
#include "lgl_vfd_studio.h"

#include <vector>
#include <cstdint>

class DispDriverLglVfd : public IDisplayDriver {
public:
    DispDriverLglVfd(int cs_gpio, int clk_gpio, int sdi_gpio, int numDigits);
    ~DispDriverLglVfd() override;

    // --- IDisplayDriver ------------------------------------------------------
    void begin() override;
    int  getDisplaySize() override { return _vfd.getDigitCount(); }
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

private:
    LglVfdStudio _vfd;
    std::vector<unsigned long> _buffer;
};
