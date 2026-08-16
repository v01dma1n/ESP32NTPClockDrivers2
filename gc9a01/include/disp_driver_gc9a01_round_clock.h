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

    // Swaps the programmatic dial (radial-gradient background + bezel
    // ring + hour/minute tick marks, all drawn from #define'd geometry
    // in buildUi()) for a static 240x240 RGB565 image instead -- e.g. a
    // pre-rendered watch-face graphic with its own numerals/bezel baked
    // in. The driver still draws the hands/gauge/digits/labels in code
    // on top of it; only the background changes. As with setPhotos(),
    // the driver has no opinion on the image's content and the caller
    // owns its storage, which must outlive this driver. Pass nullptr
    // (the default, if never called) to keep the original programmatic
    // dial. Must be called before begin() — read once at buildUi()-time.
    void setDialBackground(const lv_image_dsc_t* img);

    // Feeds a value onto the half-circle rate gauge on the left side of
    // the dial (a "how fast is the clock currently running" indicator —
    // generic in the sense that the driver has no idea what "value"
    // means, it just points a needle). value is shown relative to
    // [-fullScale, +fullScale]: 0 = needle rests pointing left (9
    // o'clock), +fullScale = needle at top (12 o'clock, green arc),
    // -fullScale = needle at bottom (6 o'clock, red arc); values beyond
    // the range saturate at the needle's max deflection rather than
    // going out of bounds. Safe to call every app loop() tick.
    void setRateGauge(float value, float fullScale);

    // Feeds one or more static photos into the face-mode rotation
    // (alongside TIME/TEMPERATURE/HUMIDITY, see
    // updateFaceModeVisibility()) — a full-face 240x240 RGB565 image
    // shown in place of the hands/dial/digits for one rotation slot,
    // then hidden again. With more than one image, each entry into
    // photo mode advances to the next image round-robin (see
    // _photoIndex in tickWatchFace()'s completion path), so repeated
    // visits alternate between them rather than always showing the
    // same one. The driver has no opinion on what the images are
    // (mirrors setTimeProvider()'s IDisplayTimeProvider seam): the
    // caller owns both the array and the lv_image_dsc_t's it points to,
    // which must outlive this driver. count is clamped to kMaxPhotos;
    // count == 0 (or imgs == nullptr) drops photo mode from the
    // rotation. Must be called before begin() — like setBrandText(),
    // it's read once at buildUi()-time, not re-read per tick.
    void setPhotos(const lv_image_dsc_t* const* imgs, int count);

private:
    static constexpr int kStatusCells = 32;

    void buildUi();
    void tickWatchFace();
    static void tickTimerCb(lv_timer_t* timer);
    // stagedReveal: true when the caller is specifically leaving photo
    // mode and wants _dialGroup's children hidden then revealed a few
    // at a time (see _revealNextChild) rather than the normal immediate
    // digit-row/info-label visibility swap used for every other mode
    // change (TIME<->TEMPERATURE<->HUMIDITY, entering photo mode).
    void updateFaceModeVisibility(bool stagedReveal = false);

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
    // temperature, humidity, and (if set) the photo. Hands keep ticking
    // continuously regardless of mode — see tickWatchFace(). PHOTO
    // replaces the whole dial rather than just the digit row (see
    // _faceContent/_photoImg below), unlike TEMPERATURE/HUMIDITY which
    // only swap the info label.
    enum class FaceMode { TIME, TEMPERATURE, HUMIDITY, PHOTO };
    // TIME gets a much longer dwell than TEMPERATURE/HUMIDITY: the clock
    // face is what this device is *for*, so the other rotation modes
    // (weather readout, photos) should be an occasional accent, not
    // something competing with it for screen time every few seconds.
    static constexpr int64_t kTimeModeDurationUs = 30 * 1000000; // 30s
    static constexpr int64_t kFaceModeDurationUs = 5 * 1000000; // 5s per mode (TEMPERATURE/HUMIDITY only)
    // Safety fallback only: PHOTO normally exits kPhotoHoldUs after the
    // CRT scan-bar sweep reaches the bottom edge (see tickWatchFace()),
    // not on a timer -- this just bounds how long PHOTO can dwell if that
    // completion check were ever skipped.
    static constexpr int64_t kPhotoModeDurationUs = 10 * 1000000; // 10s
    // Static hold *after* the scan-out completes (see tickWatchFace()'s
    // PHOTO block): the sweep runs right from the moment the photo
    // appears, since the interference effect makes a more dramatic
    // entrance than a plain cut, then holds the now-clean photo on
    // screen before switching back. The sweep alone only takes ~1.6s at
    // PHOTO_SCANBAR_SPEED, too brief to register a photo against a 30s
    // clock-face dwell (kTimeModeDurationUs) -- roughly a 19:1 ratio.
    // This hold brings total photo-visible time to ~3.1s, landing close
    // to the requested ~10:1 clock:photo ratio without slowing down the
    // scan-out animation itself.
    static constexpr int64_t kPhotoHoldUs = 1500 * 1000; // 1.5s
    FaceMode _faceMode = FaceMode::TIME;
    int64_t _faceModeSinceUs = 0;
    lv_obj_t* _infoLabel = nullptr;
    float _tempF = 0.0f;
    int _humidity = 0;
    bool _weatherValid = false;

    // See setDialBackground(). Read once at buildUi()-time; nullptr (the
    // default) keeps the original programmatic gradient+ring+ticks dial.
    const lv_image_dsc_t* _dialBgImg = nullptr;

    // Everything on the dial except the photo (ring/ticks/hands/digits/
    // gauge) lives under this one container so FaceMode::PHOTO can hide
    // it all with a single flag flip instead of tracking every sub-object
    // individually — see buildUi() and updateFaceModeVisibility().
    lv_obj_t* _faceContent = nullptr;
    // Sub-container of _faceContent holding everything except the digit
    // row (digit slots/colon slots/_infoLabel stay direct children of
    // _faceContent) — ring, ticks, gauge, hands, hub, brand/year/date
    // labels: ~120 objects. Split out so leaving photo mode can reveal
    // *these* in stages instead of one big first-render pass; see
    // tickWatchFace()/updateFaceModeVisibility().
    lv_obj_t* _dialGroup = nullptr;
    // Post-photo-mode reveal: a top-to-bottom sweep, same direction as
    // the photo scan-bar but its own speed (see DIAL_REVEAL_SPEED and
    // tickWatchFace()) and reusing _photoScanBar itself as the visible
    // wipe line, so leaving photo mode reads as a continuation of the
    // same scan motion rather than an unrelated effect. Each
    // _dialGroup child is revealed the moment the sweep passes its
    // pre-captured on-screen Y (_dialRevealY, indexed to match child
    // order, filled in at buildUi()-time from each element's actual
    // geometry) — ticks/gauge segments near the top of the dial appear
    // before ones near the bottom, matching their real position, not
    // just creation order.
    static constexpr int kMaxDialChildren = 150; // headroom above the ~120 actually created
    int16_t _dialRevealY[kMaxDialChildren] = {};
    bool _revealActive = false;
    int16_t _revealScanY = 0;
    lv_obj_t* _photoImg = nullptr;   // sibling of _faceContent, drawn on top
    bool _photoSet = false;          // see setPhotos()
    // Round-robin photo set, see setPhotos(). _photoIndex is the index
    // shown on the *next* entry into photo mode (advanced in
    // updateFaceModeVisibility()), not the one currently on screen.
    static constexpr int kMaxPhotos = 4;
    const lv_image_dsc_t* _photos[kMaxPhotos] = {};
    int _photoCount = 0;
    int _photoIndex = 0;

    // CRT-style distortion overlay for photo mode (see tickWatchFace()): a
    // translucent scan-bar that sweeps top-to-bottom, with its own
    // height/opacity randomized per tick, plus a smaller independent
    // "noise fleck" bar that flashes near it on some ticks, plus a
    // subtle per-tick opacity jitter on _photoImg itself for flicker.
    // Purely a rendering effect layered on setPhotos()'s images — the
    // driver still has no opinion on what they depict.
    lv_obj_t* _photoScanBar = nullptr;
    lv_obj_t* _photoNoiseBar = nullptr;
    int16_t _photoScanBarY = 0;
    // 0 while the scan-out sweep is still running; set to the timestamp
    // it finished at once _photoScanBarY passes the bottom edge, so
    // tickWatchFace() can hold the clean photo on screen for kPhotoHoldUs
    // before actually leaving PHOTO mode. See kPhotoHoldUs's comment for
    // why the hold comes after the scan, not before.
    int64_t _photoScanDoneUs = 0;

    // What time to render each tick; see setTimeProvider(). Not owned —
    // the caller (app layer) owns the concrete provider's lifetime.
    IDisplayTimeProvider* _timeProvider = nullptr;

    // Rate gauge: a half-circle sub-dial on the left side, see
    // setRateGauge(). Only the needle updates per tick (tight-bounding-box
    // technique, same as the hands); the tick marks are static, built once.
    lv_obj_t* _gaugeNeedle = nullptr;
    lv_point_precise_t _gaugeNeedlePts[2] = {};
    float _gaugeNormalized = 0.0f; // -1..+1, see setRateGauge()

    esp_lcd_panel_io_handle_t _ioHandle = nullptr;
    esp_lcd_panel_handle_t _panelHandle = nullptr;
    lv_timer_t* _tickTimer = nullptr;
};
