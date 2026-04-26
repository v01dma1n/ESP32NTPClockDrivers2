# ESP32 NTP Clock drivers component.
#
# IDF-native analogue of https://github.com/v01dma1n/ESP32NTPClockDrivers.
# For MoodWhisperer the relevant driver is DispDriverPT6315 (Sony HT-CT550W
# front panel: PT6315 controller + 10x 16-segment Union Jack glass +
# 10 per-digit annunciators + 8 top-row annunciators on DIG1).

idf_component_register(
    SRCS
        "src/sony_vfd_pt6315.cpp"
        "src/sony_vfd_font.cpp"
        "src/disp_driver_pt6315.cpp"
        "src/ds1307_driver.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        driver
        esp32_ntp_clock
)

target_compile_options(${COMPONENT_LIB} PUBLIC -std=gnu++17)
