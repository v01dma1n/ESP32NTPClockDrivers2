# ESP32NTPClockDrivers2

Display driver components (VFD/LED/HV/LCD) for the ESP32NTPClock2
framework.

Most drivers live directly in `include/`/`src/` and are all compiled by
the top-level `CMakeLists.txt`; each consuming project only links the
ones it actually calls. That's how MoodWhisperer, GustavClock2, and
BubbleLEDClock2 all use this repo today, via
`components/esp32_ntp_clock_drivers` as a git submodule.

## `gc9a01/` — an exception, opt-in only

`DispDriverGc9a01RoundClock` (GC9A01 round SPI LCD, LVGL-based) is *not*
part of the top-level component — it lives in its own nested component,
`gc9a01/`, with its own `CMakeLists.txt` and `idf_component.yml`. Unlike
every other driver here, it needs `esp_lvgl_port`/`lvgl`/`esp_lcd_gc9a01`
as hard build dependencies, and ESP-IDF's component system doesn't
support conditionally adding to a component's `REQUIRES` based on what
else is present in the build (only `SRCS` can be conditional that way) —
so making it part of the shared top-level component would force LVGL
onto every consumer, whether they use a round LCD or not.

To use it, a consuming project adds an `EXTRA_COMPONENT_DIRS` entry
pointing directly *at* `gc9a01/`, not at this repo's root — an
`EXTRA_COMPONENT_DIRS` entry that itself has a `CMakeLists.txt` (as this
repo's root does) is registered as that one component and never scanned
for nested component subdirectories, so pointing at the root only gets
you `esp32_ntp_clock_drivers` again, not `gc9a01`:

```cmake
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/components"
    "${CMAKE_SOURCE_DIR}/components/esp32_ntp_clock_drivers/gc9a01"
)
```

(assuming this repo is checked out at
`components/esp32_ntp_clock_drivers`, matching the other consumers'
convention) and lists `gc9a01` in its own `REQUIRES`. `gc9a01/`'s own
`idf_component.yml` pulls in the LVGL-related managed dependencies
automatically — no need to redeclare them. See `temporal_anomaly_clock`
(the first consumer) for a working example.
