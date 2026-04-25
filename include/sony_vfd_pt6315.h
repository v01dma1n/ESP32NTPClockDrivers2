// sony_vfd_pt6315.h — low-level PT6315 driver for the Sony HT-CT550W glass.
//
// IDF-native reimplementation of the Arduino SonyVFD library. Only the
// methods VFDWhisperer needs are kept — text writing, per-segment writes,
// annunciator control, brightness, refresh. The marquee logic is omitted
// because ScrollingTextAnimation already handles scrolling above this layer.
//
// This class is not an IDisplayDriver subclass — that wrapper lives in
// disp_driver_pt6315.h and uses this class as the bit-level backend.

#pragma once

#include "driver/spi_master.h"

#include <cstdint>

namespace SonyVfdConst {
    constexpr uint8_t NUM_CHAR_POSITIONS = 10;   // DIG2..DIG11
    constexpr uint8_t FIRST_CHAR_DIGIT   = 2;    // DIG2 = leftmost cell
    constexpr uint8_t TOP_ANN_DIGIT      = 1;    // DIG1 = top-row annunciators
    constexpr uint8_t RAM_SIZE           = 36;   // 12 digits * 3 bytes

    constexpr uint8_t CMD_MODE_11D_17S    = 0x07;
    constexpr uint8_t CMD_DATA_WRITE_AUTO = 0x40;
    constexpr uint8_t CMD_ADDR_BASE       = 0xC0;
    constexpr uint8_t CMD_DISP_OFF        = 0x80;
    constexpr uint8_t CMD_DISP_ON_BASE    = 0x88;   // | brightness (0..7)
}

// Per-digit annunciator identifiers (on byte 2 of each digit's 3-byte block).
// Top-row identifiers are placeholders pending a bit-walk; see
// SonyVFD.cpp in the original Arduino library.
enum SonyVfdAnnunciator : uint8_t {
    ANN_AAC_LPCM,
    ANN_TRUEHD,
    ANN_DD_EX,
    ANN_DPLIIx,
    ANN_DTS_96_24,
    ANN_DTS_ES_NEO6,
    ANN_DTS_HD_LBR,
    ANN_MSTR_HI_RES,

    ANN_NIGHT,    // DIG2
    ANN_SLEEP,    // DIG3
    ANN_HDMI,     // DIG4
    ANN_COAX,     // DIG5
    ANN_OPT,      // DIG6
    ANN_TUNED,    // DIG7
    ANN_ST,       // DIG8
    ANN_S_AIR,    // DIG9
    ANN_ECO,      // DIG10
    ANN_MUTING,   // DIG11

    ANN_COUNT
};

class SonyVfdPt6315 {
public:
    // sck/mosi/cs are GPIO numbers. hostNum lets the caller pick between
    // HSPI/VSPI; defaults to VSPI to match the Sony board's wiring.
    SonyVfdPt6315(int sck_gpio, int cs_gpio, int mosi_gpio,
                  spi_host_device_t host = SPI3_HOST);
    ~SonyVfdPt6315();

    // Bring up SPI, send the PT6315 init sequence, clear RAM, enable display.
    void begin();

    // --- Text ---------------------------------------------------------------
    void clearText();
    void clearAll();
    void setChar(uint8_t position, char c, bool showDot = false);
    void writeText(const char* text);
    void writeTextAt(uint8_t position, const char* text);

    // Write a raw 16-segment mask directly to a cell (no font lookup).
    // `mask` bit N-1 => segment SG(N).
    void writeSegmentMask(uint8_t position, uint16_t mask);

    // --- Annunciators -------------------------------------------------------
    void setAnnunciator(SonyVfdAnnunciator ann, bool on);
    void clearAnnunciators();

    // --- Display control ----------------------------------------------------
    void setBrightness(uint8_t brightness);   // 0..7
    void displayOn();
    void displayOff();

    // Push the shadow RAM buffer to the PT6315 in one burst.
    void refresh();

    // --- Low-level (for debugging / bit walking) ----------------------------
    void setSegment(uint8_t digit, uint8_t segment, bool on);
    void setDigitRaw(uint8_t digit, uint32_t segmentMask);
    const uint8_t* ramBuffer() const { return _ram; }

private:
    void sendCommand(uint8_t cmd);
    void writeRamToDevice();
    void sendDisplayControl();
    uint8_t digitBaseAddress(uint8_t digit) const;
    void setBit(uint8_t addr, uint8_t bit, bool on);
    void renderChar(uint8_t position, char c, bool showDot);

    struct AnnMap { uint8_t addr; uint8_t bit; };
    AnnMap annunciatorMap(SonyVfdAnnunciator ann) const;

    // Send one raw byte, LSB-first. The PT6315 uses LSB-first framing.
    void spiSendByteLsbFirst(uint8_t b);

    // --- Members -----------------------------------------------------------
    int _sck;
    int _cs;
    int _mosi;
    spi_host_device_t _host;
    spi_device_handle_t _dev;

    uint8_t _ram[SonyVfdConst::RAM_SIZE];
    uint8_t _brightness;
    bool    _displayOn;
    bool    _autoRefresh;
    bool    _initialized;
};
