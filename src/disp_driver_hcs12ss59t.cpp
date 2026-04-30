// disp_driver_hcs12ss59t.cpp — IDF IDisplayDriver for the HCS-12SS59T VFD controller.

#include "disp_driver_hcs12ss59t.h"
#include "logging.h"

#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

// VFD controller command nibbles (ORed with a 4-bit argument)
#define VFD_DCRAM_WR      0x10
#define VFD_DUTY          0x50
#define VFD_NUMDIGIT      0x60
#define VFD_LIGHTS        0x70
#define VFD_LIGHTS_NORMAL 0x00

DispDriverHCS12SS59T::DispDriverHCS12SS59T(int sclk, int mosi, int cs,
                                           int reset, int vdon,
                                           int displaySize,
                                           spi_host_device_t host)
    : _sclk(sclk), _mosi(mosi), _cs(cs), _reset(reset), _vdon(vdon),
      _displaySize(displaySize > 12 ? 12 : displaySize),
      _spi_host(host), _spi_handle(nullptr), _initialized(false) {
    _displayBuffer = new uint8_t[_displaySize]();
}

void DispDriverHCS12SS59T::begin() {
    if (_initialized) return;

    // GPIO: CS, RESET, VDON (all outputs).
    gpio_config_t io = {};
    io.pin_bit_mask  = (1ULL << _cs) | (1ULL << _reset) | (1ULL << _vdon);
    io.mode          = GPIO_MODE_OUTPUT;
    io.pull_up_en    = GPIO_PULLUP_DISABLE;
    io.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    io.intr_type     = GPIO_INTR_DISABLE;
    gpio_config(&io);

    gpio_set_level(static_cast<gpio_num_t>(_reset), 1);
    gpio_set_level(static_cast<gpio_num_t>(_cs),    1);
    gpio_set_level(static_cast<gpio_num_t>(_vdon),  1);  // supply off initially

    // SPI bus (MISO unused — VFD is write-only).
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num    = _mosi;
    buscfg.miso_io_num    = -1;
    buscfg.sclk_io_num    = _sclk;
    buscfg.quadwp_io_num  = -1;
    buscfg.quadhd_io_num  = -1;
    buscfg.max_transfer_sz = 16;

    esp_err_t err = spi_bus_initialize(_spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        LOGERR("HCS12SS59T: spi_bus_initialize failed: %d", (int)err);
        return;
    }

    // HCS-12SS59T: SPI mode 3 (CPOL=1, CPHA=1), 2 MHz, LSB-first.
    // CS is manual (GPIO) so spics_io_num = -1.
    spi_device_interface_config_t devcfg = {};
    devcfg.mode           = 3;
    devcfg.clock_speed_hz = 2'000'000;
    devcfg.spics_io_num   = -1;
    devcfg.flags          = SPI_DEVICE_TXBIT_LSBFIRST;
    devcfg.queue_size     = 1;

    err = spi_bus_add_device(_spi_host, &devcfg, &_spi_handle);
    if (err != ESP_OK) {
        LOGERR("HCS12SS59T: spi_bus_add_device failed: %d", (int)err);
        return;
    }

    _supplyOn();

    // Hardware reset pulse
    gpio_set_level(static_cast<gpio_num_t>(_reset), 0);
    esp_rom_delay_us(2);
    gpio_set_level(static_cast<gpio_num_t>(_reset), 1);
    esp_rom_delay_us(2);

    _sendCmd(VFD_NUMDIGIT, _displaySize);
    setBrightness(8);
    _sendCmd(VFD_LIGHTS, VFD_LIGHTS_NORMAL);
    clear();
    writeDisplay();

    _initialized = true;
    LOGINF("HCS12SS59T ready (sclk=%d mosi=%d cs=%d digits=%d)",
           _sclk, _mosi, _cs, _displaySize);
}

void DispDriverHCS12SS59T::_supplyOn() {
    gpio_set_level(static_cast<gpio_num_t>(_vdon), 0);  // active-low enable
    vTaskDelay(pdMS_TO_TICKS(1));
}

void DispDriverHCS12SS59T::_writeByte(uint8_t byte) {
    spi_transaction_t t = {};
    t.length     = 8;
    t.flags      = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = byte;
    spi_device_transmit(_spi_handle, &t);
}

void DispDriverHCS12SS59T::_sendCmd(uint8_t cmd, uint8_t arg) {
    gpio_set_level(static_cast<gpio_num_t>(_cs), 0);
    esp_rom_delay_us(1);
    _writeByte(cmd | arg);
    esp_rom_delay_us(8);
    gpio_set_level(static_cast<gpio_num_t>(_cs), 1);
}

void DispDriverHCS12SS59T::setBrightness(uint8_t level) {
    if (level > 15) level = 15;
    _sendCmd(VFD_DUTY, level);
}

void DispDriverHCS12SS59T::clear() {
    for (int i = 0; i < _displaySize; i++)
        _displayBuffer[i] = _getCode(' ');
}

uint8_t DispDriverHCS12SS59T::_getCode(char c) {
    if      (c >= '@' && c <= '_') c -= 48;
    else if (c >= ' ' && c <= '?') c += 16;
    else if (c >= 'a' && c <= 'z') c -= 80;
    else                            c  = 79;  // '?' for anything unmapped
    return (uint8_t)c;
}

void DispDriverHCS12SS59T::setChar(int position, char character, bool /*dot*/) {
    if (position < 0 || position >= _displaySize) return;
    _displayBuffer[position] = _getCode(character);
}

void DispDriverHCS12SS59T::setSegments(int position, uint16_t /*mask*/) {
    // Character ROM VFD — segment bitmasks have no direct meaning here.
    setChar(position, ' ');
}

void DispDriverHCS12SS59T::writeDisplay() {
    // Send DCRAM_WR command followed by all cells.
    // Physical display is right-to-left, so buffer is sent in reverse.
    gpio_set_level(static_cast<gpio_num_t>(_cs), 0);
    esp_rom_delay_us(1);
    _writeByte(VFD_DCRAM_WR | 0);      // write starting at address 0
    esp_rom_delay_us(8);
    for (int i = _displaySize - 1; i >= 0; i--) {
        _writeByte(_displayBuffer[i]);
        esp_rom_delay_us(8);
    }
    gpio_set_level(static_cast<gpio_num_t>(_cs), 1);
}

unsigned long DispDriverHCS12SS59T::mapAsciiToSegment(char ascii_char, bool /*dot*/) {
    return _getCode(ascii_char);
}

void DispDriverHCS12SS59T::setBuffer(const std::vector<unsigned long>& newBuffer) {
    int n = std::min((int)newBuffer.size(), _displaySize);
    for (int i = 0; i < n; i++)
        _displayBuffer[i] = (uint8_t)newBuffer[i];
    for (int i = n; i < _displaySize; i++)
        _displayBuffer[i] = _getCode(' ');
}

void DispDriverHCS12SS59T::getFrameData(unsigned long* buffer) {
    if (!buffer) return;
    for (int i = 0; i < _displaySize; i++)
        buffer[i] = _displayBuffer[i];
}
