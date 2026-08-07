// disp_driver_gc9a01_round_clock.h — GC9A01 round LCD driver.
//
// Deliberately generic/reusable, unlike the other drivers in this
// component: those speak to segment glass directly, but this one drives
// LVGL, so it also needs esp_lvgl_port/lvgl/esp_lcd_gc9a01 as build
// dependencies — see this component's CMakeLists.txt for how those are
// made conditional (only pulled in for consumers that already use lvgl,
// so projects using the other drivers here are unaffected).
//
// This driver has no notion of any particular app's "what should the
// clock show" concept (temporal anomalies, alarms, whatever) — it just
// renders whatever time it's handed by an IDisplayTimeProvider each tick
// (see i_display_time_provider.h and setTimeProvider()). A consuming app
// can plug in a provider that just calls localtime_r() on real time and
// get an ordinary accurate clock, or something far stranger — the driver
// doesn't care. (First consumer: temporal_anomaly_clock's
// TemporalAnomalyTimeSource, a sinusoidal-wobble + damped-random-walk
// "chaos" time source — see that project if you want an example.)
//
// Two responsibilities layered on one LVGL screen:
//
//   1. IDisplayDriver — satisfies BaseNtpClockApp/ClockFsmManager/
//      SceneManager's character-grid contract so the engine's boot/WiFi/
//      NTP/AP-mode status messages keep working unmodified. There's no
//      real segment glass behind this: setChar()/setBuffer() etc. just
//      maintain a plain text buffer, and writeDisplay() pushes it to an
//      LVGL status label. Segment-mask methods are accepted but no-op —
//      this driver doesn't drive segment-style scene animations (fine as
//      long as the consuming app's SceneManager playlist is empty).
//
//   2. The analog + digital watch face — an hour/minute/second hand
//      analog face (tight-bounding-box + roundf() hand rendering to keep
//      per-tick SPI redraw regions small) plus a six-slot digital
//      readout, an optional two-line brand watermark (setBrandText()),
//      and an optional temperature/humidity info-row rotation
//      (setWeatherData()). Lives on its own LVGL screen (not nested in a
//      container on the boot screen) so its elements are direct children
//      of a screen-level object; showClockFace() switches between the
//      two screens via lv_scr_load() instead of a show/hide flag on a
//      wrapping container.
//
// KNOWN LIMITATION: unlike this component's other drivers (e.g.
// DispDriverMAX6921's constructor), pin assignment, SPI clock, panel
// resolution, hand lengths, and mirror/color-order flags are all
// `#define`d at the top of the .cpp, not constructor/method parameters —
// so a board wired differently than the one this was built for (JCZN
// "ESP32-2424S012"-style: SCLK=6, MOSI=7, CS=10, DC=2, no dedicated RST,
// BL=3) needs those edited directly in the .cpp. Worth parameterizing
// properly before a second physical board relies on this driver; not
// done yet because there's been exactly one consumer so far
// (temporal_anomaly_clock).

#pragma once

#include "i_display_driver.h"
#include "i_display_time_provider.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#include <cstdint>
#include <vector>

class DispDriverGc9a01RoundClock : public IDisplayDriver {
public:
    DispDriverGc9a01RoundClock();

    // --- IDisplayDriver ----------------------------------------------------
    void begin() override;
    int  getDisplaySize() override { return kStatusCells; }
    void setBrightness(uint8_t level) override;
    void clear() override;
    void setChar(int position, char character, bool dot = false) override;
    void setSegments(int position, uint16_t mask) override;
    void setDot(int position, bool on) override;
    unsigned long mapAsciiToSegment(char ascii_char, bool dot) override;
    void setBuffer(const std::vector<unsigned long>& newBuffer) override;
    void writeDisplay() override;
    bool needsContinuousUpdate() const override { return false; }
    void getFrameData(unsigned long* buffer) override;

    // Swap between the boot/status text line and the watch face. Both are
    // already built at begin()-time; this just toggles visibility.
    void showClockFace(bool show);

    // What time the watch face renders. Must be called before the tick
    // timer starts producing meaningful frames (a call any time before or
    // during setupHardware() is fine) — until it is, ticks are a no-op.
    // The driver doesn't own or care what the provider does internally;
    // see i_display_time_provider.h.
    void setTimeProvider(IDisplayTimeProvider* provider) { _timeProvider = provider; }

    // Feeds the latest weather reading in for the temperature/humidity face
    // rotation. Safe to call every app loop() tick; only used when valid.
    void setWeatherData(float tempF, int humidity, bool valid);

    // Optional two-line watermark on the dial (e.g. an author name + a
    // year), rendered as-is with no formatting applied. Must be called
    // before begin() — buildUi() bakes these into fixed labels created
    // once at startup, not re-read per tick. Skippable: unset text just
    // renders as empty labels, since this driver has no opinion on
    // branding. Either argument may be nullptr to leave that line as-is.
    void setBrandText(const char* line1, const char* line2);

private:
    static constexpr int kStatusCells = 32;

    void buildUi();
    void tickWatchFace();
    static void tickTimerCb(lv_timer_t* timer);
    void updateFaceModeVisibility();

    char _statusBuf[kStatusCells + 1] = {};
    bool _began = false;
    bool _showingFace = false;

    // See setBrandText().
    char _brandLine1[24] = {};
    char _brandLine2[8] = {};

    lv_obj_t* _bootScreen = nullptr;
    lv_obj_t* _statusLabel = nullptr;
    lv_obj_t* _faceScreen = nullptr;

    // Watch face objects/state (see gc9a01_round_display_test for the
    // rendering approach these mirror). Each hand is drawn as kHandSegs
    // stacked line segments — this used to be 6 (tapered width + color
    // gradient, a "sword hand" look faked with discrete segments, since a
    // plain lv_line can't taper/gradient a single stroke) but that made
    // fast chaos-mode sweeps visibly lag: each segment is an independent
    // lv_obj with its own SPI-flushed redraw region, and measurement
    // (comparing 12 total segment-objects vs 3) showed per-object flush
    // overhead — not SPI clock speed or the dial's background gradient,
    // both ruled out separately — dominates the per-tick cost on this
    // MCU/display combo. 1 segment = a plain flat-color hand, but keeps
    // chaos motion tracking real time instead of falling ~5-10x behind.
    static constexpr int kHandSegs = 1;
    lv_obj_t* _hourSegs[kHandSegs] = {};
    lv_obj_t* _minSegs[kHandSegs] = {};
    lv_obj_t* _secSegs[kHandSegs] = {};
    lv_point_precise_t _hourSegPts[kHandSegs][2] = {};
    lv_point_precise_t _minSegPts[kHandSegs][2] = {};
    lv_point_precise_t _secSegPts[kHandSegs][2] = {};
    lv_obj_t* _digitSlots[6] = {}; // H tens, H ones, M tens, M ones, S tens, S ones
    lv_obj_t* _colonSlots[2] = {};
    uint32_t _digitsShownSec = UINT32_MAX;

    // Day/date complication ("MON 15"), 3 o'clock position. Updates once
    // per day, not every tick.
    lv_obj_t* _dateLabel = nullptr;
    int _dateShownMday = -1; // -1: not shown yet (tm_mday is always 1..31)

    // Info-row rotation: alternates the digit readout between time,
    // temperature, and humidity every kFaceModeDurationUs. Hands keep
    // ticking continuously regardless of mode — see tickWatchFace().
    enum class FaceMode { TIME, TEMPERATURE, HUMIDITY };
    static constexpr int64_t kFaceModeDurationUs = 5 * 1000000; // 5s per mode
    FaceMode _faceMode = FaceMode::TIME;
    int64_t _faceModeSinceUs = 0;
    lv_obj_t* _infoLabel = nullptr;
    float _tempF = 0.0f;
    int _humidity = 0;
    bool _weatherValid = false;

    // What time to render each tick; see setTimeProvider(). Not owned —
    // the caller (app layer) owns the concrete provider's lifetime.
    IDisplayTimeProvider* _timeProvider = nullptr;

    esp_lcd_panel_io_handle_t _ioHandle = nullptr;
    esp_lcd_panel_handle_t _panelHandle = nullptr;
    lv_timer_t* _tickTimer = nullptr;
};
