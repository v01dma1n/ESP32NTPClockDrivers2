# GC9A01 round LCD driver (LVGL-based), as its own ESP-IDF component.
#
# Split out from the parent esp32_ntp_clock_drivers component (rather
# than added to its SRCS like every other driver there) because it needs
# esp_lvgl_port/lvgl/esp_lcd_gc9a01 as hard build dependencies, unlike any
# other driver in this repo. Being its own component means it's simply
# never processed unless a consumer explicitly opts in — nothing here
# affects a project that doesn't add this directory to its own
# EXTRA_COMPONENT_DIRS. See this repo's README for how to opt in.

idf_component_register(
    SRCS
        "src/disp_driver_gc9a01_round_clock.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp32_ntp_clock
        driver
        esp_lcd
        lvgl__lvgl
        espressif__esp_lvgl_port
        espressif__esp_lcd_gc9a01
)

target_compile_options(${COMPONENT_LIB} PUBLIC -std=gnu++17)
