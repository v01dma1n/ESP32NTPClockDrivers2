#include "ds1307_driver.h"
#include "logging.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

// DS1307 register map (values are BCD unless noted).
namespace Reg1307 {
    static constexpr uint8_t SECONDS = 0x00;  // bit 7 = CH (clock halt)
    static constexpr uint8_t MINUTES = 0x01;
    static constexpr uint8_t HOURS   = 0x02;  // bit 6 = 12hr, bit 5 = PM
    static constexpr uint8_t DOW     = 0x03;  // day-of-week, 1-7
    static constexpr uint8_t DATE    = 0x04;
    static constexpr uint8_t MONTH   = 0x05;
    static constexpr uint8_t YEAR    = 0x06;  // 0-99 (2000-2099)
}

static constexpr uint8_t CH_BIT = 0x80;

Ds1307Driver::Ds1307Driver(i2c_port_t port, int sda, int scl, uint8_t addr)
    : _port(port), _sda(sda), _scl(scl), _addr(addr) {}

// --- I2C helpers -------------------------------------------------------------

void Ds1307Driver::writeBurst(uint8_t reg, const uint8_t* src, uint8_t len) {
    uint8_t buf[16];
    buf[0] = reg;
    memcpy(buf + 1, src, len < 15 ? len : 15);
    i2c_master_write_to_device(_port, _addr, buf, len + 1, pdMS_TO_TICKS(10));
}

void Ds1307Driver::readBurst(uint8_t reg, uint8_t* dst, uint8_t len) {
    i2c_master_write_read_device(_port, _addr, &reg, 1, dst, len, pdMS_TO_TICKS(10));
}

// --- Public API --------------------------------------------------------------

bool Ds1307Driver::init() {
    i2c_config_t conf = {};
    conf.mode             = I2C_MODE_MASTER;
    conf.sda_io_num       = _sda;
    conf.scl_io_num       = _scl;
    conf.sda_pullup_en    = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en    = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 100000;  // DS1307 max is 100 kHz
    i2c_param_config(_port, &conf);

    esp_err_t err = i2c_driver_install(_port, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_FAIL) {
        LOGERR("DS1307 I2C install failed: %d", (int)err);
        return false;
    }

    // Probe: check that the chip ACKs on the bus.
    uint8_t reg = Reg1307::SECONDS;
    uint8_t secs = 0;
    err = i2c_master_write_read_device(_port, _addr, &reg, 1, &secs, 1,
                                       pdMS_TO_TICKS(20));
    if (err != ESP_OK) {
        LOGERR("DS1307 not found (I2C err %d)", (int)err);
        return false;
    }

    LOGINF("DS1307 found (CH=%d)", (secs & CH_BIT) ? 1 : 0);
    return true;
}

bool Ds1307Driver::isRunning() {
    uint8_t reg = Reg1307::SECONDS;
    uint8_t secs = 0;
    esp_err_t err = i2c_master_write_read_device(_port, _addr, &reg, 1, &secs, 1,
                                                  pdMS_TO_TICKS(20));
    return (err == ESP_OK) && !(secs & CH_BIT);
}

time_t Ds1307Driver::readTime() {
    uint8_t regs[7];
    readBurst(Reg1307::SECONDS, regs, 7);

    if (regs[0] & CH_BIT) return (time_t)-1;

    struct tm t = {};
    t.tm_sec  = fromBCD(regs[0] & 0x7F);
    t.tm_min  = fromBCD(regs[1] & 0x7F);

    uint8_t hr = regs[2];
    if (hr & 0x40) {
        // 12-hour mode — convert to 24-hour.
        int h = fromBCD(hr & 0x1F);
        t.tm_hour = (h % 12) + ((hr & 0x20) ? 12 : 0);
    } else {
        t.tm_hour = fromBCD(hr & 0x3F);
    }
    // regs[3] = day-of-week; ignored — mktime() derives it.
    t.tm_mday  = fromBCD(regs[4]);
    t.tm_mon   = fromBCD(regs[5] & 0x1F) - 1;  // tm_mon is 0-based
    t.tm_year  = fromBCD(regs[6]) + 100;        // 0-99 → 2000-2099
    t.tm_isdst = 0;

    // RTC stores UTC. mktime() interprets in local time. This is called from
    // setupHardware() before setupSntp() sets TZ; newlib's default is UTC,
    // so the two are equivalent at this point.
    return mktime(&t);
}

bool Ds1307Driver::writeTime(time_t t) {
    struct tm utc;
    gmtime_r(&t, &utc);

    uint8_t regs[7];
    regs[0] = toBCD((uint8_t)utc.tm_sec);          // CH=0 → starts oscillator
    regs[1] = toBCD((uint8_t)utc.tm_min);
    regs[2] = toBCD((uint8_t)utc.tm_hour);         // 24-hr mode (bit6=0)
    regs[3] = (uint8_t)(utc.tm_wday + 1);         // DS1307 DOW: 1=Sunday
    regs[4] = toBCD((uint8_t)utc.tm_mday);
    regs[5] = toBCD((uint8_t)(utc.tm_mon + 1));
    regs[6] = toBCD((uint8_t)(utc.tm_year % 100));

    writeBurst(Reg1307::SECONDS, regs, 7);
    LOGINF("DS1307 time written (UTC)");
    return true;
}
