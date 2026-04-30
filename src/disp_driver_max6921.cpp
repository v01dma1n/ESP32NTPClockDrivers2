// disp_driver_max6921.cpp — IDF IDisplayDriver for MAX6921-driven VFD.

#include "disp_driver_max6921.h"
#include "logging.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

#include <cctype>
#include <cstring>
#include <algorithm>

// --- Generic 7-segment bit flags (bit 0 = segA, bit 6 = segG, bit 7 = dot) -

#define GENERIC_SEG_A   0x01u
#define GENERIC_SEG_B   0x02u
#define GENERIC_SEG_C   0x04u
#define GENERIC_SEG_D   0x08u
#define GENERIC_SEG_E   0x10u
#define GENERIC_SEG_F   0x20u
#define GENERIC_SEG_G   0x40u
#define GENERIC_SEG_DOT 0x80u

// --- Built-in default 7-segment font table ----------------------------------
// Indexed by (char - ' '). Range: space (0x20) through 'Z' (0x5A).
// Matches Default7SegFont in the Arduino ESP32NTPClock library.

static const uint8_t s_fontMap[] = {
    0,                                                          // ' '
    0,                                                          // !
    GENERIC_SEG_F | GENERIC_SEG_B,                             // "
    0, 0, 0, 0,                                                 // # $ % &
    GENERIC_SEG_A,                                              // '
    0, 0, 0, 0, 0,                                              // ( ) * + ,
    GENERIC_SEG_G,                                              // -
    0,                                                          // . (handled by dot boolean)
    0,                                                          // /
    GENERIC_SEG_A|GENERIC_SEG_B|GENERIC_SEG_C|GENERIC_SEG_D|GENERIC_SEG_E|GENERIC_SEG_F, // 0
    GENERIC_SEG_B|GENERIC_SEG_C,                               // 1
    GENERIC_SEG_A|GENERIC_SEG_B|GENERIC_SEG_G|GENERIC_SEG_E|GENERIC_SEG_D, // 2
    GENERIC_SEG_A|GENERIC_SEG_B|GENERIC_SEG_G|GENERIC_SEG_C|GENERIC_SEG_D, // 3
    GENERIC_SEG_F|GENERIC_SEG_G|GENERIC_SEG_B|GENERIC_SEG_C,  // 4
    GENERIC_SEG_A|GENERIC_SEG_F|GENERIC_SEG_G|GENERIC_SEG_C|GENERIC_SEG_D, // 5
    GENERIC_SEG_A|GENERIC_SEG_F|GENERIC_SEG_E|GENERIC_SEG_D|GENERIC_SEG_C|GENERIC_SEG_G, // 6
    GENERIC_SEG_A|GENERIC_SEG_B|GENERIC_SEG_C,                // 7
    GENERIC_SEG_A|GENERIC_SEG_B|GENERIC_SEG_C|GENERIC_SEG_D|GENERIC_SEG_E|GENERIC_SEG_F|GENERIC_SEG_G, // 8
    GENERIC_SEG_A|GENERIC_SEG_F|GENERIC_SEG_G|GENERIC_SEG_B|GENERIC_SEG_C|GENERIC_SEG_D, // 9
    0, 0, 0, 0, 0,                                             // : ; < = >
    GENERIC_SEG_A|GENERIC_SEG_B|GENERIC_SEG_G|GENERIC_SEG_E,  // ?
    0,                                                         // @
    GENERIC_SEG_A|GENERIC_SEG_B|GENERIC_SEG_C|GENERIC_SEG_E|GENERIC_SEG_F|GENERIC_SEG_G, // A
    GENERIC_SEG_C|GENERIC_SEG_D|GENERIC_SEG_E|GENERIC_SEG_F|GENERIC_SEG_G,               // B
    GENERIC_SEG_A|GENERIC_SEG_D|GENERIC_SEG_E|GENERIC_SEG_F,                             // C
    GENERIC_SEG_B|GENERIC_SEG_C|GENERIC_SEG_D|GENERIC_SEG_E|GENERIC_SEG_G,               // D
    GENERIC_SEG_A|GENERIC_SEG_D|GENERIC_SEG_E|GENERIC_SEG_F|GENERIC_SEG_G,               // E
    GENERIC_SEG_A|GENERIC_SEG_E|GENERIC_SEG_F|GENERIC_SEG_G,                             // F
    GENERIC_SEG_A|GENERIC_SEG_C|GENERIC_SEG_D|GENERIC_SEG_E|GENERIC_SEG_F,               // G
    GENERIC_SEG_B|GENERIC_SEG_C|GENERIC_SEG_E|GENERIC_SEG_F|GENERIC_SEG_G,               // H
    GENERIC_SEG_E|GENERIC_SEG_F,                                                          // I
    GENERIC_SEG_B|GENERIC_SEG_C|GENERIC_SEG_D|GENERIC_SEG_E,                             // J
    GENERIC_SEG_B|GENERIC_SEG_E|GENERIC_SEG_F|GENERIC_SEG_G,                             // K
    GENERIC_SEG_D|GENERIC_SEG_E|GENERIC_SEG_F,                                           // L
    GENERIC_SEG_A|GENERIC_SEG_B|GENERIC_SEG_C|GENERIC_SEG_E|GENERIC_SEG_F,               // M
    GENERIC_SEG_A|GENERIC_SEG_B|GENERIC_SEG_C|GENERIC_SEG_E|GENERIC_SEG_F,               // N
    GENERIC_SEG_A|GENERIC_SEG_B|GENERIC_SEG_C|GENERIC_SEG_D|GENERIC_SEG_E|GENERIC_SEG_F, // O
    GENERIC_SEG_A|GENERIC_SEG_B|GENERIC_SEG_E|GENERIC_SEG_F|GENERIC_SEG_G,               // P
    GENERIC_SEG_A|GENERIC_SEG_B|GENERIC_SEG_C|GENERIC_SEG_F|GENERIC_SEG_G,               // Q
    GENERIC_SEG_E|GENERIC_SEG_G,                                                          // R
    GENERIC_SEG_A|GENERIC_SEG_F|GENERIC_SEG_G|GENERIC_SEG_C|GENERIC_SEG_D,               // S
    GENERIC_SEG_D|GENERIC_SEG_E|GENERIC_SEG_F|GENERIC_SEG_G,                             // T
    GENERIC_SEG_B|GENERIC_SEG_C|GENERIC_SEG_D|GENERIC_SEG_E|GENERIC_SEG_F,               // U
    GENERIC_SEG_C|GENERIC_SEG_D|GENERIC_SEG_E,                                           // V
    GENERIC_SEG_A|GENERIC_SEG_C|GENERIC_SEG_D|GENERIC_SEG_E,                             // W
    GENERIC_SEG_B|GENERIC_SEG_C|GENERIC_SEG_E|GENERIC_SEG_F,                             // X
    GENERIC_SEG_B|GENERIC_SEG_C|GENERIC_SEG_D|GENERIC_SEG_F|GENERIC_SEG_G,               // Y
    0,                                                                                    // Z
};

// ---------------------------------------------------------------------------

DispDriverMAX6921::DispDriverMAX6921(int sclk, int miso, int mosi, int ss, int blank,
                                     const unsigned long* gridMap, int digitCount,
                                     const Max6921SegmentMap& segMap,
                                     spi_host_device_t host)
    : _sclk(sclk), _miso(miso), _mosi(mosi), _ss(ss), _blank(blank),
      _host(host), _dev(nullptr),
      _gridMap(gridMap), _digitCount(digitCount), _segMap(segMap),
      _buffer(nullptr), _currentDigit(0), _initialized(false)
{
    _buffer = new unsigned long[_digitCount]();
}

DispDriverMAX6921::~DispDriverMAX6921() {
    if (_dev) {
        spi_bus_remove_device(_dev);
        spi_bus_free(_host);
    }
    delete[] _buffer;
}

void DispDriverMAX6921::begin() {
    if (_initialized) return;

    // GPIO: BLANK (output, start HIGH = blanked) and SS (managed by SPI driver).
    gpio_config_t io = {};
    io.pin_bit_mask  = (1ULL << _blank);
    io.mode          = GPIO_MODE_OUTPUT;
    io.pull_up_en    = GPIO_PULLUP_DISABLE;
    io.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    io.intr_type     = GPIO_INTR_DISABLE;
    gpio_config(&io);
    gpio_set_level(static_cast<gpio_num_t>(_blank), 1);  // blank until first write

    // SPI bus — MISO unused (MAX6921 is write-only), set to -1.
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num    = _mosi;
    buscfg.miso_io_num    = (_miso >= 0) ? _miso : -1;
    buscfg.sclk_io_num    = _sclk;
    buscfg.quadwp_io_num  = -1;
    buscfg.quadhd_io_num  = -1;
    buscfg.max_transfer_sz = 4;

    esp_err_t err = spi_bus_initialize(_host, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        LOGERR("MAX6921: spi_bus_initialize failed: %d", err);
        return;
    }

    // MAX6921: 8 MHz, SPI mode 0, CS active-low, MSB-first.
    // MAX6921 supports up to 26 MHz; 8 MHz keeps the 24-bit transfer under 3µs
    // so blanking time per digit is negligible.
    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = 8'000'000;
    devcfg.mode           = 0;
    devcfg.spics_io_num   = _ss;
    devcfg.queue_size     = 1;
    devcfg.flags          = 0;

    err = spi_bus_add_device(_host, &devcfg, &_dev);
    if (err != ESP_OK) {
        LOGERR("MAX6921: spi_bus_add_device failed: %d", err);
        return;
    }

    clear();
    _initialized = true;
    LOGINF("MAX6921 ready (sclk=%d mosi=%d ss=%d blank=%d digits=%d)",
           _sclk, _mosi, _ss, _blank, _digitCount);
}

void DispDriverMAX6921::clear() {
    if (_buffer) {
        std::fill(_buffer, _buffer + _digitCount, 0UL);
    }
}

// Map a generic 8-bit segment mask + dot to the hardware 20-bit word.
unsigned long DispDriverMAX6921::mapGenericToHw(uint8_t mask, bool dot) const {
    unsigned long hw = 0;
    if (mask & GENERIC_SEG_A)   hw |= _segMap.segA;
    if (mask & GENERIC_SEG_B)   hw |= _segMap.segB;
    if (mask & GENERIC_SEG_C)   hw |= _segMap.segC;
    if (mask & GENERIC_SEG_D)   hw |= _segMap.segD;
    if (mask & GENERIC_SEG_E)   hw |= _segMap.segE;
    if (mask & GENERIC_SEG_F)   hw |= _segMap.segF;
    if (mask & GENERIC_SEG_G)   hw |= _segMap.segG;
    if (dot || (mask & GENERIC_SEG_DOT)) hw |= _segMap.segDot;
    return hw;
}

uint8_t DispDriverMAX6921::charToGenericMask(char c) {
    char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (upper >= ' ' && upper <= 'Z') {
        return s_fontMap[static_cast<unsigned char>(upper) - ' '];
    }
    return 0;
}

unsigned long DispDriverMAX6921::mapAsciiToSegment(char ascii_char, bool dot) {
    return mapGenericToHw(charToGenericMask(ascii_char), dot);
}

void DispDriverMAX6921::setChar(int position, char character, bool dot) {
    if (position < 0 || position >= _digitCount) return;
    _buffer[position] = mapAsciiToSegment(character, dot);
}

void DispDriverMAX6921::setSegments(int position, uint16_t mask) {
    if (position < 0 || position >= _digitCount) return;
    _buffer[position] = mapGenericToHw(static_cast<uint8_t>(mask), false);
}

void DispDriverMAX6921::setDot(int position, bool on) {
    if (position < 0 || position >= _digitCount) return;
    if (on) _buffer[position] |=  _segMap.segDot;
    else    _buffer[position] &= ~_segMap.segDot;
}

void DispDriverMAX6921::setBuffer(const std::vector<unsigned long>& newBuffer) {
    int n = static_cast<int>(std::min(static_cast<int>(newBuffer.size()), _digitCount));
    for (int i = 0; i < n; ++i) _buffer[i] = newBuffer[i];
}

void DispDriverMAX6921::getFrameData(unsigned long* buffer) {
    if (buffer && _buffer) {
        std::memcpy(buffer, _buffer, _digitCount * sizeof(unsigned long));
    }
}

// Send a 20-bit value as 3 bytes (24 bits, MSB-first).
// The MAX6921's 20-bit shift register is loaded from the last 20 clocks, so
// the 4 leading zero bits (bits [23:20]) are harmless.
void DispDriverMAX6921::spiSend(unsigned long data) {
    uint8_t bytes[3];
    bytes[0] = static_cast<uint8_t>((data >> 16) & 0xFF);
    bytes[1] = static_cast<uint8_t>((data >>  8) & 0xFF);
    bytes[2] = static_cast<uint8_t>( data        & 0xFF);

    spi_transaction_t t = {};
    t.length    = 24;
    t.tx_buffer = bytes;
    spi_device_polling_transmit(_dev, &t);
}

// Called from RefreshTask in a tight loop (one call per digit, all digits per
// frame). Loads the digit data while still blanked from the previous call,
// then unblanks, holds for US_PER_DIGIT using a hardware-accurate busy-wait,
// and re-blanks. The display is therefore always blanked when the task yields
// between frames, so every digit gets exactly the same on-time independent of
// FreeRTOS scheduling.
static constexpr uint32_t US_PER_DIGIT = 900;

void DispDriverMAX6921::writeNextDigit() {
    if (!_initialized || !_dev) return;

    // SPI while still blanked (from previous call or begin()).
    spiSend(_gridMap[_currentDigit] | _buffer[_currentDigit]);

    gpio_set_level(static_cast<gpio_num_t>(_blank), 0);  // unblank
    esp_rom_delay_us(US_PER_DIGIT);                       // hold — hardware-accurate
    gpio_set_level(static_cast<gpio_num_t>(_blank), 1);  // blank

    _currentDigit = (_currentDigit + 1) % _digitCount;
}
