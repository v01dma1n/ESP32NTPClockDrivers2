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
#
# DispDriverGc9a01RoundClock (GC9A01 round LCD, LVGL-based) lives in its
# own nested component, gc9a01/, instead of here — it needs esp_lvgl_port/
# lvgl/esp_lcd_gc9a01 as build dependencies, and ESP-IDF's component
# system doesn't support conditionally adding to REQUIRES based on what's
# present elsewhere in the build (only SRCS can be conditional; REQUIRES
# has to be known before the dependency graph — which BUILD_COMPONENTS
# reflects — is resolved, so a REQUIRES that depends on BUILD_COMPONENTS
# doesn't reliably take effect). A separate component means it's simply
# never processed at all unless a consumer explicitly adds it to
# EXTRA_COMPONENT_DIRS — see gc9a01/CMakeLists.txt and this repo's
# README.

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
