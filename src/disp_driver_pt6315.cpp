// disp_driver_pt6315.cpp — thin adapter wiring SonyVfdPt6315 into IDisplayDriver.

#include "disp_driver_pt6315.h"
#include "sony_vfd_font.h"

#include <cstring>

DispDriverPT6315::DispDriverPT6315(int sck_gpio, int cs_gpio, int mosi_gpio,
                                   spi_host_device_t host)
    : _vfd(sck_gpio, cs_gpio, mosi_gpio, host),
      _buffer(SonyVfdConst::NUM_CHAR_POSITIONS, 0UL) {}

DispDriverPT6315::~DispDriverPT6315() = default;

void DispDriverPT6315::begin() {
    _vfd.begin();
    _vfd.clearAll();
}

void DispDriverPT6315::setBrightness(uint8_t level) {
    // level is 0..7 already in the IDisplayDriver contract.
    _vfd.setBrightness(level);
}

void DispDriverPT6315::clear() {
    std::fill(_buffer.begin(), _buffer.end(), 0UL);
    _vfd.clearText();
}

void DispDriverPT6315::setChar(int position, char character, bool dot) {
    if (position < 0 ||
        position >= static_cast<int>(_buffer.size())) return;

    uint16_t mask = dot ? sonyVfdFontGlyphWithDot(character)
                        : sonyVfdFontGlyph(character);
    _buffer[position] = mask;
    _vfd.writeSegmentMask(static_cast<uint8_t>(position), mask);
}

void DispDriverPT6315::setSegments(int position, uint16_t mask) {
    if (position < 0 ||
        position >= static_cast<int>(_buffer.size())) return;
    _buffer[position] = mask;
    _vfd.writeSegmentMask(static_cast<uint8_t>(position), mask);
}

void DispDriverPT6315::setDot(int position, bool on) {
    if (position < 0 ||
        position >= static_cast<int>(_buffer.size())) return;

    constexpr uint16_t DOT_BIT = 1u << 0;  // SG1
    uint16_t mask = static_cast<uint16_t>(_buffer[position]);
    if (on) mask |=  DOT_BIT;
    else    mask &= ~DOT_BIT;
    _buffer[position] = mask;
    _vfd.writeSegmentMask(static_cast<uint8_t>(position), mask);
}

unsigned long DispDriverPT6315::mapAsciiToSegment(char ascii_char, bool dot) {
    // Animations call this to build frames without touching hardware.
    return dot ? sonyVfdFontGlyphWithDot(ascii_char)
               : sonyVfdFontGlyph(ascii_char);
}

void DispDriverPT6315::setBuffer(const std::vector<unsigned long>& newBuffer) {
    size_t n = std::min(_buffer.size(), newBuffer.size());
    for (size_t i = 0; i < n; ++i) _buffer[i] = newBuffer[i];
}

void DispDriverPT6315::writeDisplay() {
    // Push all cells at once. For each cell, write the cached frame word
    // as a 16-segment mask. Annunciators (byte 2) are preserved because
    // writeSegmentMask only touches bytes 0 and 1 of each digit block.
    for (size_t i = 0; i < _buffer.size(); ++i) {
        _vfd.writeSegmentMask(static_cast<uint8_t>(i),
                              static_cast<uint16_t>(_buffer[i]));
    }
}

void DispDriverPT6315::getFrameData(unsigned long* buffer) {
    if (!buffer) return;
    for (size_t i = 0; i < _buffer.size(); ++i) buffer[i] = _buffer[i];
}

void DispDriverPT6315::setAnnunciator(SonyVfdAnnunciator ann, bool on) {
    _vfd.setAnnunciator(ann, on);
}

void DispDriverPT6315::clearAnnunciators() {
    _vfd.clearAnnunciators();
}
