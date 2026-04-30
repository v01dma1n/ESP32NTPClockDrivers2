// disp_driver_ht16k33.cpp — IDF IDisplayDriver for HT16K33.

#include "disp_driver_ht16k33.h"
#include "logging.h"

#include <algorithm>
#include <cctype>
#include <cstring>

// HT16K33 command bytes (from datasheet)
#define HT16K33_CMD_OSCILLATOR_ON 0x21
#define HT16K33_CMD_DISPLAY_ON    0x81
#define HT16K33_CMD_BRIGHTNESS    0xE0

// 7-segment font table: ASCII 0x20 (' ') through 0x5A ('Z').
// bit0=A, bit1=B, ..., bit6=G, bit7=dot.
// Matches the Arduino Adafruit HT16K33 library font exactly.
const uint8_t DispDriverHT16K33::s_font[] = {
    0b00000000, 0b10000110, 0b00100010, 0b01111110, 0b01101101, 0b11010010,  //  !"#$%
    0b01000110, 0b00100000, 0b00101001, 0b00001011, 0b00100001, 0b01110000,  // &'()*+
    0b00010000, 0b01000000, 0b10000000, 0b01010010, 0b00111111, 0b00000110,  // ,-./01
    0b01011011, 0b01001111, 0b01100110, 0b01101101, 0b01111101, 0b00000111,  // 234567
    0b01111111, 0b01101111, 0b00001001, 0b00001101, 0b01100001, 0b01001000,  // 89:;<=
    0b01000011, 0b11010011, 0b01011111, 0b01110111, 0b01111100, 0b00111001,  // >?@ABC
    0b01011110, 0b01111001, 0b01110001, 0b00111101, 0b01110110, 0b00110000,  // DEFGHI
    0b00011110, 0b01110101, 0b00111000, 0b00010101, 0b00110111, 0b00111111,  // JKLMNO
    0b01110011, 0b01101011, 0b00110011, 0b01101101, 0b01111000, 0b00111110,  // PQRSTU
    0b00111110, 0b00101010, 0b01110110, 0b01101110, 0b01011011              //  VWXYZ
};

static uint8_t fontByte(char c) {
    char u = (char)toupper((unsigned char)c);
    if (u < ' ' || u > 'Z') return 0;
    return DispDriverHT16K33::s_font[(int)(u - ' ')];
}

DispDriverHT16K33::DispDriverHT16K33(i2c_port_t port, int sda, int scl,
                                     uint8_t i2c_addr, int displaySize)
    : _port(port), _sda(sda), _scl(scl),
      _i2c_addr(i2c_addr), _initialized(false) {
    _displaySize = std::max(1, std::min(displaySize, HT16K33_MAX_DISP_SIZE));
    memset(_displayBuffer, 0, sizeof(_displayBuffer));
}

void DispDriverHT16K33::begin() {
    if (_initialized) return;

    i2c_config_t conf = {};
    conf.mode             = I2C_MODE_MASTER;
    conf.sda_io_num       = _sda;
    conf.scl_io_num       = _scl;
    conf.sda_pullup_en    = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en    = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 400000;
    i2c_param_config(_port, &conf);

    esp_err_t err = i2c_driver_install(_port, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_FAIL) {
        LOGERR("HT16K33: i2c_driver_install failed: %d", (int)err);
        return;
    }

    _writeCmd(HT16K33_CMD_OSCILLATOR_ON);
    _writeCmd(HT16K33_CMD_DISPLAY_ON);
    setBrightness(HT16K33_DEF_BRIGHTNESS);

    _initialized = true;
    LOGINF("HT16K33 ready (port=%d sda=%d scl=%d addr=0x%02X digits=%d)",
           (int)_port, _sda, _scl, _i2c_addr, _displaySize);
}

void DispDriverHT16K33::_writeCmd(uint8_t cmd) {
    esp_err_t err = i2c_master_write_to_device(
        _port, _i2c_addr, &cmd, 1, pdMS_TO_TICKS(10));
    if (err != ESP_OK)
        LOGERR("HT16K33: I2C cmd 0x%02X failed: %d", cmd, (int)err);
}

void DispDriverHT16K33::setBrightness(uint8_t level) {
    if (level > HT16K33_MAX_BRIGHTNESS) level = HT16K33_MAX_BRIGHTNESS;
    _writeCmd(HT16K33_CMD_BRIGHTNESS | level);
}

void DispDriverHT16K33::clear() {
    memset(_displayBuffer, 0, sizeof(uint16_t) * _displaySize);
}

void DispDriverHT16K33::setChar(int position, char character, bool dot) {
    if (position < 0 || position >= _displaySize) return;
    uint8_t seg = fontByte(character);
    if (dot) seg |= 0x80;
    _displayBuffer[_displaySize - 1 - position] = seg;
}

void DispDriverHT16K33::setSegments(int position, uint16_t mask) {
    if (position < 0 || position >= _displaySize) return;
    _displayBuffer[_displaySize - 1 - position] = mask;
}

void DispDriverHT16K33::setDot(int position, bool on) {
    if (position < 0 || position >= _displaySize) return;
    int idx = _displaySize - 1 - position;
    if (on) _displayBuffer[idx] |=  0x80;
    else    _displayBuffer[idx] &= ~0x80;
}

void DispDriverHT16K33::writeDisplay() {
    // 1 address byte + 2 bytes per digit (HT16K33 uses 16-bit rows)
    uint8_t buf[1 + HT16K33_MAX_DISP_SIZE * 2];
    buf[0] = 0x00;  // start at RAM address 0
    for (int i = 0; i < _displaySize; i++) {
        buf[1 + i * 2]     = _displayBuffer[i] & 0xFF;
        buf[1 + i * 2 + 1] = _displayBuffer[i] >> 8;
    }
    esp_err_t err = i2c_master_write_to_device(
        _port, _i2c_addr, buf, 1 + _displaySize * 2, pdMS_TO_TICKS(50));
    if (err != ESP_OK)
        LOGERR("HT16K33: writeDisplay I2C error: %d", (int)err);
}

unsigned long DispDriverHT16K33::mapAsciiToSegment(char ascii_char, bool dot) {
    uint8_t seg = fontByte(ascii_char);
    if (dot) seg |= 0x80;
    return seg;
}

void DispDriverHT16K33::setBuffer(const std::vector<unsigned long>& newBuffer) {
    int n = std::min((int)newBuffer.size(), _displaySize);
    for (int i = 0; i < n; i++)
        _displayBuffer[_displaySize - 1 - i] = (uint16_t)newBuffer[i];
    for (int i = n; i < _displaySize; i++)
        _displayBuffer[_displaySize - 1 - i] = 0;
}

void DispDriverHT16K33::getFrameData(unsigned long* buffer) {
    if (!buffer) return;
    for (int i = 0; i < _displaySize; i++)
        buffer[i] = _displayBuffer[_displaySize - 1 - i];
}
