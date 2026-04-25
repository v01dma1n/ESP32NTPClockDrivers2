// sony_vfd_pt6315.cpp — IDF port of the Arduino SonyVFD driver.
//
// Key differences vs the Arduino original:
//   * Uses driver/spi_master instead of the Arduino SPI class.
//   * PT6315 wants LSB-first; IDF's spi_master is MSB-first by default,
//     so we reverse each byte manually and let the bus send it MSB-first.
//   * No attached marquee helper (handled by ScrollingTextAnimation).

#include "sony_vfd_pt6315.h"
#include "sony_vfd_font.h"
#include "logging.h"

#include "driver/spi_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

namespace {
    inline uint8_t reverseBits(uint8_t b) {
        b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
        b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
        b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
        return b;
    }
}

SonyVfdPt6315::SonyVfdPt6315(int sck_gpio, int cs_gpio, int mosi_gpio,
                             spi_host_device_t host)
    : _sck(sck_gpio),
      _cs(cs_gpio),
      _mosi(mosi_gpio),
      _host(host),
      _dev(nullptr),
      _brightness(7),
      _displayOn(true),
      _autoRefresh(true),
      _initialized(false) {
    std::memset(_ram, 0, sizeof(_ram));
}

SonyVfdPt6315::~SonyVfdPt6315() {
    if (_dev) {
        spi_bus_remove_device(_dev);
        spi_bus_free(_host);
    }
}

void SonyVfdPt6315::begin() {
    if (_initialized) return;

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num    = _mosi;
    buscfg.miso_io_num    = -1;
    buscfg.sclk_io_num    = _sck;
    buscfg.quadwp_io_num  = -1;
    buscfg.quadhd_io_num  = -1;
    buscfg.max_transfer_sz = 64;

    esp_err_t err = spi_bus_initialize(_host, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        LOGERR("PT6315: spi_bus_initialize failed: %d", err);
        return;
    }

    // 1 MHz SPI_MODE0, CS active-low — matches the Arduino driver. We
    // send MSB-first on the wire but pre-reverse every byte so the
    // PT6315 sees LSB-first framing.
    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = 1'000'000;
    devcfg.mode           = 0;
    devcfg.spics_io_num   = _cs;
    devcfg.queue_size     = 1;
    devcfg.flags          = 0;

    err = spi_bus_add_device(_host, &devcfg, &_dev);
    if (err != ESP_OK) {
        LOGERR("PT6315: spi_bus_add_device failed: %d", err);
        return;
    }

    // --- PT6315 init sequence --------------------------------------------
    //   1. CMD_DATA_WRITE_AUTO: set auto-increment write mode
    //   2. clear RAM (0xC0 base address + 36 zero bytes)
    //   3. CMD_MODE_11D_17S: set mode to 11 digits x 17 segments
    //   4. CMD_DISP_ON + brightness: enable display + max brightness
    sendCommand(SonyVfdConst::CMD_DATA_WRITE_AUTO);
    writeRamToDevice();
    sendCommand(SonyVfdConst::CMD_MODE_11D_17S);
    sendDisplayControl();

    _initialized = true;
    LOGINF("PT6315 ready (sck=%d, mosi=%d, cs=%d)", _sck, _mosi, _cs);
}

void SonyVfdPt6315::spiSendByteLsbFirst(uint8_t b) {
    spi_transaction_t t = {};
    uint8_t reversed = reverseBits(b);
    t.length    = 8;
    t.tx_buffer = &reversed;
    spi_device_transmit(_dev, &t);
}

void SonyVfdPt6315::sendCommand(uint8_t cmd) {
    spiSendByteLsbFirst(cmd);
}

void SonyVfdPt6315::writeRamToDevice() {
    // The PT6315 requires the address byte and all data bytes in ONE CS
    // transaction. A second CS edge resets the chip's command decoder, so
    // splitting across two transactions makes the data bytes look like
    // commands. Build a single scratch buffer: [addr, d0..d35].
    uint8_t scratch[1 + SonyVfdConst::RAM_SIZE];
    scratch[0] = reverseBits(SonyVfdConst::CMD_ADDR_BASE);
    for (int i = 0; i < SonyVfdConst::RAM_SIZE; ++i) {
        scratch[1 + i] = reverseBits(_ram[i]);
    }
    spi_transaction_t t = {};
    t.length    = (1 + SonyVfdConst::RAM_SIZE) * 8;
    t.tx_buffer = scratch;
    spi_device_transmit(_dev, &t);
}

void SonyVfdPt6315::sendDisplayControl() {
    uint8_t cmd = _displayOn
        ? (SonyVfdConst::CMD_DISP_ON_BASE | (_brightness & 0x07))
        :  SonyVfdConst::CMD_DISP_OFF;
    sendCommand(cmd);
}

uint8_t SonyVfdPt6315::digitBaseAddress(uint8_t digit) const {
    // digit: 1..12 -> 0..33, 3 bytes per digit
    if (digit < 1)  digit = 1;
    if (digit > 12) digit = 12;
    return static_cast<uint8_t>((digit - 1) * 3);
}

void SonyVfdPt6315::setBit(uint8_t addr, uint8_t bit, bool on) {
    if (addr >= SonyVfdConst::RAM_SIZE) return;
    uint8_t mask = static_cast<uint8_t>(1u << (bit & 0x07));
    if (on) _ram[addr] |=  mask;
    else    _ram[addr] &= ~mask;
}

void SonyVfdPt6315::renderChar(uint8_t position, char c, bool showDot) {
    // position is 0..9 (mapping onto DIG2..DIG11).
    if (position >= SonyVfdConst::NUM_CHAR_POSITIONS) return;

    uint16_t mask = showDot ? sonyVfdFontGlyphWithDot(c) : sonyVfdFontGlyph(c);

    uint8_t digit = SonyVfdConst::FIRST_CHAR_DIGIT + position;
    uint8_t base  = digitBaseAddress(digit);

    // Byte 0 = SG1..SG8 (lower half + decimal), Byte 1 = SG9..SG16.
    _ram[base + 0] = static_cast<uint8_t>(mask & 0x00FF);
    _ram[base + 1] = static_cast<uint8_t>((mask >> 8) & 0x00FF);
    // Byte 2 (annunciator for this digit) is left untouched.
}

// --- Public API -------------------------------------------------------------

void SonyVfdPt6315::clearText() {
    for (uint8_t p = 0; p < SonyVfdConst::NUM_CHAR_POSITIONS; ++p) {
        uint8_t digit = SonyVfdConst::FIRST_CHAR_DIGIT + p;
        uint8_t base  = digitBaseAddress(digit);
        _ram[base + 0] = 0;
        _ram[base + 1] = 0;
    }
    if (_autoRefresh) refresh();
}

void SonyVfdPt6315::clearAll() {
    std::memset(_ram, 0, sizeof(_ram));
    if (_autoRefresh) refresh();
}

void SonyVfdPt6315::setChar(uint8_t position, char c, bool showDot) {
    renderChar(position, c, showDot);
    if (_autoRefresh) refresh();
}

void SonyVfdPt6315::writeText(const char* text) {
    clearText();
    writeTextAt(0, text);
}

void SonyVfdPt6315::writeTextAt(uint8_t position, const char* text) {
    if (!text) return;
    uint8_t p = position;
    for (; *text && p < SonyVfdConst::NUM_CHAR_POSITIONS; ++text, ++p) {
        renderChar(p, *text, false);
    }
    if (_autoRefresh) refresh();
}

void SonyVfdPt6315::writeSegmentMask(uint8_t position, uint16_t mask) {
    if (position >= SonyVfdConst::NUM_CHAR_POSITIONS) return;
    uint8_t digit = SonyVfdConst::FIRST_CHAR_DIGIT + position;
    uint8_t base  = digitBaseAddress(digit);
    _ram[base + 0] = static_cast<uint8_t>(mask & 0x00FF);
    _ram[base + 1] = static_cast<uint8_t>((mask >> 8) & 0x00FF);
    if (_autoRefresh) refresh();
}

// --- Annunciators -----------------------------------------------------------

SonyVfdPt6315::AnnMap
SonyVfdPt6315::annunciatorMap(SonyVfdAnnunciator ann) const {
    // Per-digit annunciators: byte 2 of the digit's 3-byte block.
    // Bit position is assumed to be 0 (SG17) pending bit-walk — see the
    // "Outstanding Work" section of the Sony VFD integration README.
    auto perDigit = [](uint8_t digit) -> AnnMap {
        uint8_t base = static_cast<uint8_t>((digit - 1) * 3);
        return { static_cast<uint8_t>(base + 2), 0 };  // bit 0 = SG17
    };

    switch (ann) {
        // Top-row annunciators (DIG1 byte 0/1/2). Exact bits TBD.
        case ANN_AAC_LPCM:     return { 0, 0 };
        case ANN_TRUEHD:       return { 0, 1 };
        case ANN_DD_EX:        return { 0, 2 };
        case ANN_DPLIIx:       return { 0, 3 };
        case ANN_DTS_96_24:    return { 1, 0 };
        case ANN_DTS_ES_NEO6:  return { 1, 1 };
        case ANN_DTS_HD_LBR:   return { 1, 2 };
        case ANN_MSTR_HI_RES:  return { 1, 3 };

        case ANN_NIGHT:   return perDigit(2);
        case ANN_SLEEP:   return perDigit(3);
        case ANN_HDMI:    return perDigit(4);
        case ANN_COAX:    return perDigit(5);
        case ANN_OPT:     return perDigit(6);
        case ANN_TUNED:   return perDigit(7);
        case ANN_ST:      return perDigit(8);
        case ANN_S_AIR:   return perDigit(9);
        case ANN_ECO:     return perDigit(10);
        case ANN_MUTING:  return perDigit(11);
        default:          return { 0, 0 };
    }
}

void SonyVfdPt6315::setAnnunciator(SonyVfdAnnunciator ann, bool on) {
    AnnMap m = annunciatorMap(ann);
    setBit(m.addr, m.bit, on);
    if (_autoRefresh) refresh();
}

void SonyVfdPt6315::clearAnnunciators() {
    // Clear DIG1 entirely (top row) and byte 2 of every per-digit block.
    _ram[0] = _ram[1] = _ram[2] = 0;
    for (int d = 2; d <= 12; ++d) {
        _ram[digitBaseAddress(d) + 2] = 0;
    }
    if (_autoRefresh) refresh();
}

// --- Display control --------------------------------------------------------

void SonyVfdPt6315::setBrightness(uint8_t brightness) {
    _brightness = brightness & 0x07;
    sendDisplayControl();
}

void SonyVfdPt6315::displayOn()  { _displayOn = true;  sendDisplayControl(); }
void SonyVfdPt6315::displayOff() { _displayOn = false; sendDisplayControl(); }

void SonyVfdPt6315::refresh() {
    sendCommand(SonyVfdConst::CMD_DATA_WRITE_AUTO);
    writeRamToDevice();
}

// --- Low-level --------------------------------------------------------------

void SonyVfdPt6315::setSegment(uint8_t digit, uint8_t segment, bool on) {
    if (digit < 1 || digit > 12) return;
    if (segment < 1 || segment > 24) return;
    uint8_t base = digitBaseAddress(digit);
    uint8_t s0   = segment - 1;  // 0..23
    uint8_t byte = static_cast<uint8_t>(s0 >> 3);
    uint8_t bit  = static_cast<uint8_t>(s0 & 0x07);
    setBit(base + byte, bit, on);
    if (_autoRefresh) refresh();
}

void SonyVfdPt6315::setDigitRaw(uint8_t digit, uint32_t segmentMask) {
    if (digit < 1 || digit > 12) return;
    uint8_t base = digitBaseAddress(digit);
    _ram[base + 0] = static_cast<uint8_t>( segmentMask        & 0xFF);
    _ram[base + 1] = static_cast<uint8_t>((segmentMask >>  8) & 0xFF);
    _ram[base + 2] = static_cast<uint8_t>((segmentMask >> 16) & 0xFF);
    if (_autoRefresh) refresh();
}
