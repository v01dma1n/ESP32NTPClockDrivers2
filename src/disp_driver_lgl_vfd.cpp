// disp_driver_lgl_vfd.cpp — thin adapter wiring LglVfdStudio into IDisplayDriver.

#include "disp_driver_lgl_vfd.h"

#include <algorithm>

DispDriverLglVfd::DispDriverLglVfd(int cs_gpio, int clk_gpio, int sdi_gpio, int numDigits)
    : _vfd(cs_gpio, clk_gpio, sdi_gpio, numDigits),
      _buffer(static_cast<size_t>(_vfd.getDigitCount()),
              static_cast<unsigned long>(' ')) {}

DispDriverLglVfd::~DispDriverLglVfd() = default;

void DispDriverLglVfd::begin() {
    _vfd.begin();
    _vfd.clear();
}

void DispDriverLglVfd::setBrightness(uint8_t level) {
    // level is 0..7 in the IDisplayDriver contract; the LGL chip's native
    // scale is a full byte (0..255).
    uint8_t native = (level >= 7) ? 0xFF
                                  : static_cast<uint8_t>((level * 255) / 7);
    _vfd.setBrightness(native);
}

void DispDriverLglVfd::clear() {
    std::fill(_buffer.begin(), _buffer.end(), static_cast<unsigned long>(' '));
    _vfd.clear();
}

void DispDriverLglVfd::setChar(int position, char character, bool /*dot*/) {
    // No separate decimal-point bit on this glass — unlike a segment
    // display, a '.' can only be shown by putting it in its own cell, so
    // `dot` has nothing to attach to here.
    if (position < 0 || position >= static_cast<int>(_buffer.size())) return;
    _buffer[position] = static_cast<unsigned long>(static_cast<uint8_t>(character));
    _vfd.writeChar(static_cast<uint8_t>(position), character);
}

void DispDriverLglVfd::setSegments(int position, uint16_t mask) {
    // No independent segment control on a font-ROM glass — the low byte
    // of the mask is treated as the ASCII character to render.
    setChar(position, static_cast<char>(mask & 0xFF));
}

void DispDriverLglVfd::setDot(int position, bool /*on*/) {
    (void)position;  // no-op: see setChar().
}

unsigned long DispDriverLglVfd::mapAsciiToSegment(char ascii_char, bool /*dot*/) {
    return static_cast<unsigned long>(static_cast<uint8_t>(ascii_char));
}

void DispDriverLglVfd::setBuffer(const std::vector<unsigned long>& newBuffer) {
    size_t n = std::min(_buffer.size(), newBuffer.size());
    for (size_t i = 0; i < n; ++i) _buffer[i] = newBuffer[i];
}

void DispDriverLglVfd::writeDisplay() {
    // Push the whole row in one burst rather than one CS pulse per cell.
    char text[LglVfdConst::MAX_DIGITS + 1];
    int n = std::min(_buffer.size(), static_cast<size_t>(LglVfdConst::MAX_DIGITS));
    for (int i = 0; i < n; ++i) text[i] = static_cast<char>(_buffer[i]);
    text[n] = '\0';
    _vfd.writeString(0, text);
}

void DispDriverLglVfd::getFrameData(unsigned long* buffer) {
    if (!buffer) return;
    for (size_t i = 0; i < _buffer.size(); ++i) buffer[i] = _buffer[i];
}
