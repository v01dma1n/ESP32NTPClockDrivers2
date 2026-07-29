# ESP32 NTP Clock drivers component.
#
# IDF-native analogue of https://github.com/v01dma1n/ESP32NTPClockDrivers.
# All drivers are compiled; each project only links the ones it calls.
#
# Drivers:
#   DispDriverMAX6921    — MAX6921 VFD driver   (SPI, GustavClock2 + future clocks)
#   DispDriverPT6315     — PT6315 VFD driver    (SPI, MoodWhisperer Sony panel)
#   DispDriverHT16K33    — HT16K33 LED driver   (I2C, 7-segment / matrix)
#   DispDriverHCS12SS59T — HCS-12SS59T VFD driver (SPI, character ROM)
#   DispDriverLglVfd     — LGL Studio VFD driver (bit-bang, MoodWhisperer's
#                          replacement glass — see components/lgl_vfd_studio)
#   Ds1307Driver         — DS1307 RTC driver    (I2C, shared)

idf_component_register(
    SRCS
        "src/disp_driver_max6921.cpp"
        "src/sony_vfd_pt6315.cpp"
        "src/sony_vfd_font.cpp"
        "src/disp_driver_pt6315.cpp"
        "src/disp_driver_ht16k33.cpp"
        "src/disp_driver_hcs12ss59t.cpp"
        "src/disp_driver_lgl_vfd.cpp"
        "src/ds1307_driver.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        driver
        esp32_ntp_clock
        esp_rom
        lgl_vfd_studio
)

target_compile_options(${COMPONENT_LIB} PUBLIC -std=gnu++17)
