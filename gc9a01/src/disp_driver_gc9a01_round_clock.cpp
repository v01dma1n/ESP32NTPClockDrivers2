#include "disp_driver_gc9a01_round_clock.h"

#include "esp_log.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lvgl_port.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static const char* TAG = "disp_gc9a01";

// ---- Pin mapping, confirmed working on this board (see
// gc9a01_round_display_test's bring-up notes) ----
#define LCD_SPI_HOST    SPI2_HOST
#define PIN_LCD_SCLK    GPIO_NUM_6
#define PIN_LCD_MOSI    GPIO_NUM_7
#define PIN_LCD_MISO    GPIO_NUM_NC   // display is write-only, no MISO
#define PIN_LCD_CS      GPIO_NUM_10
#define PIN_LCD_DC      GPIO_NUM_2
#define PIN_LCD_RST     GPIO_NUM_NC   // no dedicated reset line on this board
#define PIN_LCD_BL      GPIO_NUM_3

#define LCD_H_RES       240
#define LCD_V_RES       240
#define LCD_BITS_PER_PIXEL 16
#define LCD_SPI_CLOCK_HZ   (40 * 1000 * 1000)

#define CLOCK_CENTER_X   (LCD_H_RES / 2)
#define CLOCK_CENTER_Y   (LCD_V_RES / 2)
#define HOUR_HAND_LEN    55
#define MIN_HAND_LEN     85
#define SEC_HAND_LEN     100

#define TIME_DIGIT_W 20
#define TIME_COLON_W 8
#define TIME_ROW_Y   60

// Rate gauge (half-circle sub-dial, left side) — see setRateGauge().
// Pivot sits inside the tick ring (which starts at radius 92 from
// CLOCK_CENTER), offset toward 9 o'clock; needle rests pointing due
// left (270 deg in this file's angle convention: 0=north, clockwise)
// and sweeps through the left half of the circle: 180 deg (south, max
// negative) up to 360/0 deg (north, max positive).
#define GAUGE_CENTER_X      (CLOCK_CENTER_X - 42)
#define GAUGE_CENTER_Y      CLOCK_CENTER_Y
#define GAUGE_TICK_OUTER_R  22
#define GAUGE_TICK_INNER_R  16
#define GAUGE_NEEDLE_LEN    19
#define GAUGE_BG_RADIUS     (GAUGE_TICK_OUTER_R + 3)

// Photo mode's CRT scan-bar overlay — see tickWatchFace(). Real per-tick
// wall time isn't a clean 30ms under load (SPI flush cost varies with
// what's on screen), so treat PHOTO_SCANBAR_SPEED as a starting point to
// retune against the actual dwell (kPhotoModeDurationUs) on hardware
// rather than trusting the nominal px/tick * 30ms math.
#define PHOTO_SCANBAR_H_MIN 3
#define PHOTO_SCANBAR_H_MAX 20
#define PHOTO_SCANBAR_SPEED 4 // px/tick
// Dial-reveal sweep (see tickWatchFace()) reuses _photoScanBar as its
// wipe line but moves faster than the photo distortion's own scan --
// tuned independently on hardware, not derived from PHOTO_SCANBAR_SPEED.
#define DIAL_REVEAL_SPEED 12 // px/tick

// Bezel/tick accent color -- was a warm copper (0xB87333) when the dial
// had a matching warm-brown gradient background; both went grayscale
// (see kDialGradStops) so the transition into photo mode's B&W images
// isn't such a jarring color-to-monochrome jump. The seconds hand stays
// its original red (see secColors in buildUi()) so it's still easy to
// track at a glance against an otherwise B&W face.
static lv_color_t dialAccentColor() { return lv_color_hex(0xC0C0C0); }

// CRT scanline overlay, tiled across the whole face screen (both the
// dial and photo mode -- it's the topmost sibling in _faceScreen, see
// buildUi()) so the whole watch face reads as viewed through a CRT, not
// just the photo distortion effects. A single tiled A8 (alpha-only, 1
// byte/pixel) image rather than many thin lv_obj bars: this MCU/display
// combo has a real per-*object* redraw cost (see the README's "Why the
// hands are flat, not tapered" and the hand-segment-count history) --
// tens of extra always-on objects sitting on top of the hands would pay
// that cost every tick even though scanlines themselves never move.
// LVGL's software renderer draws an A8 bg-image as a mask tinted by
// bg_image_recolor (see lv_draw_sw_img.c), so this tile carries no
// color, just the alternating opaque/transparent alpha pattern; one
// dark row every other row is enough to read as scanlines without
// meaningfully dimming digits/hands underneath.
static const uint8_t kScanlineTileData[16] = {
    70, 70, 70, 70, 70, 70, 70, 70, // dark row
    0,  0,  0,  0,  0,  0,  0,  0,  // transparent row
};
static const lv_image_dsc_t kScanlineTile = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_A8,
        .flags = 0,
        .w = 8,
        .h = 2,
        .stride = 8,
        .reserved_2 = 0,
    },
    .data_size = sizeof(kScanlineTileData),
    .data = kScanlineTileData,
    .reserved = nullptr,
    .reserved_2 = nullptr,
};

DispDriverGc9a01RoundClock::DispDriverGc9a01RoundClock() {
    std::memset(_statusBuf, 0, sizeof(_statusBuf));
}

void DispDriverGc9a01RoundClock::setBrandText(const char* line1, const char* line2) {
    if (line1) {
        std::strncpy(_brandLine1, line1, sizeof(_brandLine1) - 1);
        _brandLine1[sizeof(_brandLine1) - 1] = '\0';
    }
    if (line2) {
        std::strncpy(_brandLine2, line2, sizeof(_brandLine2) - 1);
        _brandLine2[sizeof(_brandLine2) - 1] = '\0';
    }
}

void DispDriverGc9a01RoundClock::setDialBackground(const lv_image_dsc_t* img) {
    _dialBgImg = img;
}

void DispDriverGc9a01RoundClock::setVersionTag(const char* text) {
    if (!text) return;
    std::strncpy(_versionTag, text, sizeof(_versionTag) - 1);
    _versionTag[sizeof(_versionTag) - 1] = '\0';
}

// --- Panel + LVGL bring-up ---------------------------------------------------

void DispDriverGc9a01RoundClock::begin() {
    if (_began) return;

    ESP_LOGI(TAG, "configuring backlight on GPIO%d", PIN_LCD_BL);
    gpio_config_t bl_cfg = {};
    bl_cfg.pin_bit_mask = 1ULL << PIN_LCD_BL;
    bl_cfg.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(PIN_LCD_BL, 1);

    ESP_LOGI(TAG, "initializing SPI bus on SCLK=%d MOSI=%d", PIN_LCD_SCLK, PIN_LCD_MOSI);
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = PIN_LCD_SCLK;
    buscfg.mosi_io_num = PIN_LCD_MOSI;
    buscfg.miso_io_num = PIN_LCD_MISO;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "attaching panel IO, CS=%d DC=%d", PIN_LCD_CS, PIN_LCD_DC);
    esp_lcd_panel_io_spi_config_t io_config = GC9A01_PANEL_IO_SPI_CONFIG(
        PIN_LCD_CS, PIN_LCD_DC, nullptr, nullptr);
    io_config.pclk_hz = LCD_SPI_CLOCK_HZ;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                              &io_config, &_ioHandle));

    ESP_LOGI(TAG, "creating GC9A01 panel, RST=%d", PIN_LCD_RST);
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = PIN_LCD_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR; // confirmed by test; RGB swaps red/blue
    panel_config.bits_per_pixel = LCD_BITS_PER_PIXEL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(_ioHandle, &panel_config, &_panelHandle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(_panelHandle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(_panelHandle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(_panelHandle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(_panelHandle, true));

    ESP_LOGI(TAG, "starting LVGL port");
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = _ioHandle;
    disp_cfg.panel_handle = _panelHandle;
    disp_cfg.buffer_size = LCD_H_RES * 40;
    disp_cfg.double_buffer = true;
    disp_cfg.hres = LCD_H_RES;
    disp_cfg.vres = LCD_V_RES;
    disp_cfg.monochrome = false;
    disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
    // Whether this physical panel needs a horizontal mirror to render
    // right-reading varies by GC9A01 module/wiring batch — must be set here
    // (not via a separate esp_lcd_panel_mirror() call before
    // lvgl_port_add_disp()) because lvgl_port_add_disp() re-applies orientation
    // from this struct internally, silently overwriting anything set earlier.
    // The board attached 2026-08-21 needs mirror_x=true, confirmed stable
    // across 4 consecutive true power-cycles (unplug/replug, not just an
    // EN/software reset). An earlier false reading of "false" as correct
    // came from testing only across warm resets, which never actually
    // power-cycled the panel -- a true POR is the only valid test for this
    // setting on RST-less GC9A01 boards.
    disp_cfg.rotation.mirror_x = true;
    disp_cfg.flags.buff_dma = true;
    disp_cfg.flags.swap_bytes = true;
    lv_disp_t* disp = lvgl_port_add_disp(&disp_cfg);
    (void)disp;

    ESP_LOGI(TAG, "building UI");
    lvgl_port_lock(0);
    buildUi();
    lvgl_port_unlock();

    _tickTimer = lv_timer_create(tickTimerCb, 30, this);

    _began = true;
}

// --- UI construction ---------------------------------------------------------

// Fills out[0..count-1] with a linear interpolation from `from` to `to`
// (inclusive at both ends). Used for both the width and radius-boundary
// ramps below.
static void fillIntRamp(int16_t out[], int count, int16_t from, int16_t to) {
    if (count == 1) { out[0] = from; return; } // avoid divide-by-zero below
    for (int i = 0; i < count; i++) {
        out[i] = (int16_t)(from + (int32_t)(to - from) * i / (count - 1));
    }
}

// Fills out[0..count-1] with a color ramp from `from` to `to` using
// lv_color_mix() (mix=255 -> full first arg, 0 -> full second arg) instead
// of hand-picked hex stops, so the segment count can change without
// re-deriving intermediate colors by hand.
static void fillColorRamp(lv_color_t out[], int count, lv_color_t from, lv_color_t to) {
    if (count == 1) { out[0] = from; return; } // avoid divide-by-zero below
    for (int i = 0; i < count; i++) {
        uint8_t mix = (uint8_t)(255 * (count - 1 - i) / (count - 1));
        out[i] = lv_color_mix(from, to, mix);
    }
}

// Fills out[0..segCount] with the segCount+1 radius boundaries (0 to
// total_len) that split a hand of length total_len into segCount equal
// segments — see setTaperedHand().
static void fillRadii(int16_t out[], int segCount, int16_t total_len) {
    for (int i = 0; i <= segCount; i++) {
        out[i] = (int16_t)((int32_t)total_len * i / segCount);
    }
}

// Creates the `count` line objects for one tapered hand (base -> tip).
// widths[i]/colors[i] are used for segs[i] — see setTaperedHand() for how
// the segments are positioned along the hand each tick. A plain lv_line
// stroke can't have a smooth gradient along its length, so colors[] fakes
// one with `count` discrete shades, same trick as the width taper.
static void createTaperedHand(lv_obj_t* parent, lv_obj_t* segs[],
                               const int16_t widths[], const lv_color_t colors[], int count) {
    for (int i = 0; i < count; i++) {
        segs[i] = lv_line_create(parent);
        lv_obj_set_style_line_width(segs[i], widths[i], 0);
        lv_obj_set_style_line_color(segs[i], colors[i], 0);
        lv_obj_set_style_line_rounded(segs[i], true, 0);
    }
}

// Forward-declared: real definition (with the tight-bounding-box
// technique doc comment) is down in the watch-face-ticking section; used
// here in buildUi() to set the rate gauge needle's initial position too.
static void setHandSeg(lv_obj_t* seg, lv_point_precise_t* pts, float angle_deg,
                        int16_t r0, int16_t r1, int16_t cx, int16_t cy);

static lv_obj_t* createTimeSlot(lv_obj_t* parent, int16_t width, int16_t x_offset, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_width(lbl, width);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, x_offset, TIME_ROW_Y);
    if (text) lv_label_set_text(lbl, text);
    return lbl;
}

void DispDriverGc9a01RoundClock::buildUi() {
    // --- boot screen: status text line (shown until RUNNING_NORMAL) ---
    _bootScreen = lv_scr_act();
    lv_obj_set_style_bg_color(_bootScreen, lv_color_black(), 0);

    _statusLabel = lv_label_create(_bootScreen);
    lv_obj_set_width(_statusLabel, 200);
    lv_label_set_long_mode(_statusLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(_statusLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(_statusLabel, lv_color_white(), 0);
    lv_obj_center(_statusLabel);
    lv_label_set_text(_statusLabel, "");

    // Small, dim, permanent version footer -- see setVersionTag(). Below
    // and smaller than _statusLabel so it reads as a footnote, not
    // competing with the actual boot status text above it. Explicit
    // width + wrap (like _statusLabel) rather than letting a long string
    // run past the round glass: git describe grows with commits-since-tag
    // (e.g. "v1.1.0-12-g1a2b3c4-dirty"), and re-tagging -- the whole
    // point of adopting this scheme -- resets that back to short, but it
    // shouldn't clip in the meantime. y=58 (vs _statusLabel's y=0)
    // leaves enough of the circle's chord width for a two-line wrap
    // without the second line crowding the bezel.
    _versionLabel = lv_label_create(_bootScreen);
    lv_obj_set_width(_versionLabel, 160);
    lv_label_set_long_mode(_versionLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(_versionLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(_versionLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(_versionLabel, lv_color_hex(0x808080), 0);
    lv_obj_align(_versionLabel, LV_ALIGN_CENTER, 0, 58);
    lv_label_set_text(_versionLabel, _versionTag);

    // --- watch face: its own screen, loaded via showClockFace() ---
    _faceScreen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_faceScreen, lv_color_black(), 0);

    // Sunburst dial: a subtle radial gradient (gray center fading to
    // black at the edge) rather than a flat fill, like a brushed
    // stainless watch dial -- grayscale rather than warm-toned so the
    // switch into photo mode's B&W images doesn't also cross a color-to-
    // monochrome jump (see dialAccentColor()). The lv_grad_dsc_t must
    // outlive this function (LVGL's style system stores the pointer, not
    // a copy of the struct), hence `static` here rather than a local.
    // Skipped when a caller-supplied dial background image is in play
    // (see setDialBackground()) -- that image fully covers the round
    // face, so this gradient would never actually show through it.
    if (!_dialBgImg) {
        static lv_grad_dsc_t s_dialGrad;
        static const lv_color_t kDialGradStops[2] = { lv_color_hex(0x2E2E2E), lv_color_black() };
        lv_grad_init_stops(&s_dialGrad, kDialGradStops, nullptr, nullptr, 2);
        lv_grad_radial_init(&s_dialGrad, CLOCK_CENTER_X, CLOCK_CENTER_Y,
                             CLOCK_CENTER_X + 116, CLOCK_CENTER_Y, LV_GRAD_EXTEND_PAD);
        lv_obj_set_style_bg_grad(_faceScreen, &s_dialGrad, 0);
    }

    // Everything below except the photo image parents to _faceContent
    // rather than _faceScreen directly, so FaceMode::PHOTO can hide the
    // whole dial (ring/ticks/hands/digits/gauge) with one flag flip — see
    // updateFaceModeVisibility(). _photoImg is created as _faceContent's
    // sibling further down, after _faceContent so it paints on top.
    _faceContent = lv_obj_create(_faceScreen);
    lv_obj_remove_style_all(_faceContent);
    lv_obj_set_size(_faceContent, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(_faceContent, 0, 0);
    lv_obj_clear_flag(_faceContent, LV_OBJ_FLAG_SCROLLABLE);

    // _dialGroup holds everything except the digit row (digit slots,
    // colon slots, info label stay direct children of _faceContent) --
    // this is the ~130-object "background" (ring/ticks/gauge/hands/hub/
    // brand/year/date labels) that's expensive to render all at once.
    // Split out specifically so the post-photo-mode reveal can hide/
    // un-hide *these* objects a few at a time without touching the
    // cheap, already-fast digit-row visibility logic — see
    // updateFaceModeVisibility() and tickWatchFace()'s staged reveal.
    _dialGroup = lv_obj_create(_faceContent);
    lv_obj_remove_style_all(_dialGroup);
    lv_obj_set_size(_dialGroup, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(_dialGroup, 0, 0);
    lv_obj_clear_flag(_dialGroup, LV_OBJ_FLAG_SCROLLABLE);

    if (_dialBgImg) {
        // Static pre-rendered dial background (see setDialBackground())
        // standing in for the gradient+ring+tick-mark drawing below --
        // numerals and bezel detail are baked into the image; only the
        // dynamic hands/gauge/digits/labels are still drawn in code, on
        // top of it. First child of _dialGroup (so those paint over it)
        // and first in the reveal sweep (Y=0 -- the "canvas" appears
        // before the details that sit on top of it).
        _dialRevealY[lv_obj_get_child_cnt(_dialGroup)] = 0;
        lv_obj_t* dialBg = lv_image_create(_dialGroup);
        lv_obj_set_size(dialBg, LCD_H_RES, LCD_V_RES);
        lv_obj_set_pos(dialBg, 0, 0);
        lv_image_set_src(dialBg, _dialBgImg);
    } else {
        // outer bezel ring — reveal Y is its top edge (radius 116 from
        // center), so the top-to-bottom post-photo sweep (see
        // tickWatchFace()) uncovers it almost immediately.
        _dialRevealY[lv_obj_get_child_cnt(_dialGroup)] = (int16_t)(CLOCK_CENTER_Y - 116);
        lv_obj_t* ring = lv_obj_create(_dialGroup);
        lv_obj_remove_style_all(ring);
        lv_obj_set_size(ring, 232, 232);
        lv_obj_center(ring);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 2, 0);
        lv_obj_set_style_border_color(ring, dialAccentColor(), 0);

        // hour tick marks — 12/3/6/9 (cardinal) drawn thicker and longer
        // than the other 8
        for (int i = 0; i < 12; i++) {
            bool cardinal = (i % 3 == 0);
            float rad = i * 30.0f * ((float)M_PI / 180.0f);
            int16_t r_out = cardinal ? 114 : 112;
            int16_t r_in = cardinal ? 92 : 98;
            static lv_point_precise_t tick_pts[12][2];
            tick_pts[i][0].x = CLOCK_CENTER_X + (int16_t)(r_out * sinf(rad));
            tick_pts[i][0].y = CLOCK_CENTER_Y - (int16_t)(r_out * cosf(rad));
            tick_pts[i][1].x = CLOCK_CENTER_X + (int16_t)(r_in * sinf(rad));
            tick_pts[i][1].y = CLOCK_CENTER_Y - (int16_t)(r_in * cosf(rad));

            // Reveal Y: topmost of the tick's two endpoints, so ticks near
            // 12 o'clock uncover before ticks near 6 o'clock, matching
            // their actual position rather than loop/creation order.
            int16_t tickMinY = (tick_pts[i][0].y < tick_pts[i][1].y) ? tick_pts[i][0].y : tick_pts[i][1].y;
            _dialRevealY[lv_obj_get_child_cnt(_dialGroup)] = tickMinY;
            lv_obj_t* tick = lv_line_create(_dialGroup);
            lv_obj_set_pos(tick, 0, 0);
            lv_obj_set_size(tick, LCD_H_RES, LCD_V_RES);
            lv_line_set_points(tick, tick_pts[i], 2);
            lv_obj_set_style_line_width(tick, cardinal ? 5 : 3, 0);
            lv_obj_set_style_line_color(tick, dialAccentColor(), 0);
        }

        // minute ticks: short, thin marks at the 48 positions between the
        // hour ticks (skips every 5th, since those already have an hour
        // tick)
        for (int i = 0; i < 60; i++) {
            if (i % 5 == 0) continue;
            float rad = i * 6.0f * ((float)M_PI / 180.0f);
            int16_t r_out = 112, r_in = 105;
            static lv_point_precise_t minute_tick_pts[60][2];
            minute_tick_pts[i][0].x = CLOCK_CENTER_X + (int16_t)(r_out * sinf(rad));
            minute_tick_pts[i][0].y = CLOCK_CENTER_Y - (int16_t)(r_out * cosf(rad));
            minute_tick_pts[i][1].x = CLOCK_CENTER_X + (int16_t)(r_in * sinf(rad));
            minute_tick_pts[i][1].y = CLOCK_CENTER_Y - (int16_t)(r_in * cosf(rad));

            int16_t tickMinY = (minute_tick_pts[i][0].y < minute_tick_pts[i][1].y)
                ? minute_tick_pts[i][0].y : minute_tick_pts[i][1].y;
            _dialRevealY[lv_obj_get_child_cnt(_dialGroup)] = tickMinY;
            lv_obj_t* tick = lv_line_create(_dialGroup);
            lv_obj_set_pos(tick, 0, 0);
            lv_obj_set_size(tick, LCD_H_RES, LCD_V_RES);
            lv_line_set_points(tick, minute_tick_pts[i], 2);
            lv_obj_set_style_line_width(tick, 1, 0);
            lv_obj_set_style_line_color(tick, dialAccentColor(), 0);
        }
    }

    // Rate gauge backing half-disc — gray so the black needle (and dark
    // tick colors) stay visible against the dial's black background, with
    // a light-at-top/dark-at-bottom gradient (via the same fillColorRamp()
    // used for the hand color ramps) so it reads as a curved/domed surface
    // catching light from above rather than a flat wedge. A true
    // half-circle (not a full disc): built as a fan of overlapping thick
    // radial lines from the pivot, same technique as every other
    // line-based element on this face, rather than a new widget type.
    // Static — built once, no ongoing per-tick cost.
    {
        static constexpr int kGaugeFanSegs = 46; // 4deg steps -- finer angular
                                                  // resolution + rounded caps
                                                  // (below) smooth the outer
                                                  // edge; free at runtime since
                                                  // this is a one-time static
                                                  // build, not per-tick.
        static lv_point_precise_t gauge_fan_pts[kGaugeFanSegs][2];
        lv_color_t gaugeGrad[kGaugeFanSegs];
        fillColorRamp(gaugeGrad, kGaugeFanSegs, lv_color_hex(0x181818), lv_color_hex(0x707070));
        for (int i = 0; i < kGaugeFanSegs; i++) {
            float angle_deg = 180.0f + i * (180.0f / (kGaugeFanSegs - 1)); // 180..360
            float rad = angle_deg * ((float)M_PI / 180.0f);
            gauge_fan_pts[i][0].x = GAUGE_CENTER_X;
            gauge_fan_pts[i][0].y = GAUGE_CENTER_Y;
            gauge_fan_pts[i][1].x = GAUGE_CENTER_X + (int16_t)roundf(GAUGE_BG_RADIUS * sinf(rad));
            gauge_fan_pts[i][1].y = GAUGE_CENTER_Y - (int16_t)roundf(GAUGE_BG_RADIUS * cosf(rad));

            int16_t wedgeMinY = (gauge_fan_pts[i][0].y < gauge_fan_pts[i][1].y)
                ? gauge_fan_pts[i][0].y : gauge_fan_pts[i][1].y;
            _dialRevealY[lv_obj_get_child_cnt(_dialGroup)] = wedgeMinY;
            lv_obj_t* wedge = lv_line_create(_dialGroup);
            lv_obj_set_pos(wedge, 0, 0);
            lv_obj_set_size(wedge, LCD_H_RES, LCD_V_RES);
            lv_line_set_points(wedge, gauge_fan_pts[i], 2);
            lv_obj_set_style_line_width(wedge, 5, 0);
            lv_obj_set_style_line_color(wedge, gaugeGrad[i], 0);
            lv_obj_set_style_line_rounded(wedge, true, 0);
        }
    }

    // Rate gauge (see setRateGauge()): 7 static tick marks across the
    // left-facing half circle (180deg south/max-negative through 270deg
    // west/zero to 360deg north/max-positive), red for the negative half,
    // white for zero, green for the positive half. Static — built once,
    // never redrawn per tick, so no ongoing cost; only the needle (below)
    // updates per tick.
    {
        static const float kGaugeTickAngles[7] = { 180, 210, 240, 270, 300, 330, 360 };
        static const lv_color_t kGaugeTickColors[7] = {
            lv_color_hex(0xFF3333), lv_color_hex(0xFF3333), lv_color_hex(0xFF3333),
            lv_color_hex(0xFFFFFF),
            lv_color_hex(0x33CC33), lv_color_hex(0x33CC33), lv_color_hex(0x33CC33),
        };
        static lv_point_precise_t gauge_tick_pts[7][2];
        for (int i = 0; i < 7; i++) {
            float rad = kGaugeTickAngles[i] * ((float)M_PI / 180.0f);
            gauge_tick_pts[i][0].x = GAUGE_CENTER_X + (int16_t)roundf(GAUGE_TICK_OUTER_R * sinf(rad));
            gauge_tick_pts[i][0].y = GAUGE_CENTER_Y - (int16_t)roundf(GAUGE_TICK_OUTER_R * cosf(rad));
            gauge_tick_pts[i][1].x = GAUGE_CENTER_X + (int16_t)roundf(GAUGE_TICK_INNER_R * sinf(rad));
            gauge_tick_pts[i][1].y = GAUGE_CENTER_Y - (int16_t)roundf(GAUGE_TICK_INNER_R * cosf(rad));

            int16_t tickMinY = (gauge_tick_pts[i][0].y < gauge_tick_pts[i][1].y)
                ? gauge_tick_pts[i][0].y : gauge_tick_pts[i][1].y;
            _dialRevealY[lv_obj_get_child_cnt(_dialGroup)] = tickMinY;
            lv_obj_t* tick = lv_line_create(_dialGroup);
            lv_obj_set_pos(tick, 0, 0);
            lv_obj_set_size(tick, LCD_H_RES, LCD_V_RES);
            lv_line_set_points(tick, gauge_tick_pts[i], 2);
            lv_obj_set_style_line_width(tick, (i == 3) ? 3 : 2, 0); // zero tick slightly bolder
            lv_obj_set_style_line_color(tick, kGaugeTickColors[i], 0);
            lv_obj_set_style_line_rounded(tick, true, 0);
        }
    }

    // Rate gauge needle: black, pivots at GAUGE_CENTER, updated per tick
    // in tickWatchFace() via setHandSeg() (same tight-bounding-box
    // technique as the clock hands). Starts pointing left (zero/rest).
    // Reveal Y uses the pivot itself — the needle's actual endpoint
    // moves every tick regardless of hidden state, so there's no single
    // "real" position to capture the way the static gauge ticks have.
    _dialRevealY[lv_obj_get_child_cnt(_dialGroup)] = (int16_t)GAUGE_CENTER_Y;
    _gaugeNeedle = lv_line_create(_dialGroup);
    lv_obj_set_style_line_width(_gaugeNeedle, 2, 0);
    lv_obj_set_style_line_color(_gaugeNeedle, lv_color_black(), 0);
    lv_obj_set_style_line_rounded(_gaugeNeedle, true, 0);
    setHandSeg(_gaugeNeedle, _gaugeNeedlePts, 270.0f, 0, GAUGE_NEEDLE_LEN,
               GAUGE_CENTER_X, GAUGE_CENTER_Y);

    // Optional watermark text (see setBrandText()), placed between the
    // ticks and the hands so painter's-order z-order puts the hands on
    // top — LVGL draws each screen's children in add order, later = on
    // top. Empty strings render as empty labels, which is fine — this
    // driver has no opinion on branding, that's the app's call.
    _dialRevealY[lv_obj_get_child_cnt(_dialGroup)] = (int16_t)(CLOCK_CENTER_Y - 46);
    lv_obj_t* brandLabel = lv_label_create(_dialGroup);
    lv_obj_set_style_text_font(brandLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(brandLabel, lv_color_white(), 0);
    lv_label_set_text(brandLabel, _brandLine1);
    lv_obj_align(brandLabel, LV_ALIGN_CENTER, 0, -46);

    _dialRevealY[lv_obj_get_child_cnt(_dialGroup)] = (int16_t)(CLOCK_CENTER_Y - 28);
    lv_obj_t* yearLabel = lv_label_create(_dialGroup);
    lv_obj_set_style_text_font(yearLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(yearLabel, lv_color_white(), 0);
    lv_label_set_text(yearLabel, _brandLine2);
    lv_obj_align(yearLabel, LV_ALIGN_CENTER, 0, -28);

    // Date complication: sits inside the dial background's faceted
    // "crystal" cutout at the 3 o'clock position (see
    // setDialBackground()) -- a light, glassy window bounded roughly
    // x:[170,222] y:[95,145] in the 240x240 output, so offset (76, 0)
    // centers text in it. Dark text, not the white used everywhere else
    // on the dial, since the crystal itself is near-white -- white text
    // would wash out against it. Font is montserrat_34, 2.5x the size it
    // started at (14) per explicit request, sized to fill the window.
    _dialRevealY[lv_obj_get_child_cnt(_dialGroup)] = (int16_t)CLOCK_CENTER_Y;
    _dateLabel = lv_label_create(_dialGroup);
    lv_obj_set_style_text_font(_dateLabel, &lv_font_montserrat_34, 0);
    lv_obj_set_style_text_color(_dateLabel, lv_color_hex(0x202020), 0);
    lv_obj_align(_dateLabel, LV_ALIGN_CENTER, 76, 0);

    // widths taper base -> mid -> tip; see setTaperedHand() for the
    // matching radius breakpoints used each tick. Widths and colors ramp
    // base -> tip in kHandSegs steps, simulating light catching a
    // beveled/polished metal hand (darker+narrower near the hub,
    // brighter+narrower toward the tip). Hour/minute are grayscale
    // (steel rather than gilt) so the second hand's red is the one
    // splash of color on the whole dial -- easy to track at a glance,
    // and keeps the clock-to-photo-mode transition from also being a
    // color-to-monochrome jump (see dialAccentColor()).
    int16_t hourWidths[kHandSegs], minWidths[kHandSegs], secWidths[kHandSegs];
    fillIntRamp(hourWidths, kHandSegs, 8, 3);
    fillIntRamp(minWidths, kHandSegs, 6, 2);
    fillIntRamp(secWidths, kHandSegs, 3, 1);

    // Base-end colors brightened from the original 0x505050/0x8B0000 --
    // against the art-deco dial's near-black background (see
    // setDialBackground()) that dark a base read as almost invisible
    // near the hub, especially before the tip's taper widens it.
    lv_color_t hourColors[kHandSegs], minColors[kHandSegs], secColors[kHandSegs];
    fillColorRamp(hourColors, kHandSegs, lv_color_hex(0x9A9A9A), lv_color_hex(0xFFFFFF));
    fillColorRamp(minColors, kHandSegs, lv_color_hex(0x9A9A9A), lv_color_hex(0xFFFFFF));
    fillColorRamp(secColors, kHandSegs, lv_color_hex(0xC23B2E), lv_color_hex(0xFF7A66));

    // Hands pivot at CLOCK_CENTER and sweep continuously regardless of
    // hidden state (see tickWatchFace()), so — like the gauge needle —
    // there's no single "real" position to capture; use the pivot for
    // every segment createTaperedHand() is about to make (kHandSegs
    // per hand, currently 1, but this loop covers it either way).
    for (int i = 0; i < kHandSegs; i++) {
        _dialRevealY[lv_obj_get_child_cnt(_dialGroup) + (uint32_t)i] = (int16_t)CLOCK_CENTER_Y;
    }
    createTaperedHand(_dialGroup, _hourSegs, hourWidths, hourColors, kHandSegs);
    for (int i = 0; i < kHandSegs; i++) {
        _dialRevealY[lv_obj_get_child_cnt(_dialGroup) + (uint32_t)i] = (int16_t)CLOCK_CENTER_Y;
    }
    createTaperedHand(_dialGroup, _minSegs, minWidths, minColors, kHandSegs);
    for (int i = 0; i < kHandSegs; i++) {
        _dialRevealY[lv_obj_get_child_cnt(_dialGroup) + (uint32_t)i] = (int16_t)CLOCK_CENTER_Y;
    }
    createTaperedHand(_dialGroup, _secSegs, secWidths, secColors, kHandSegs);

    // center hub, drawn last so it sits above the hands
    _dialRevealY[lv_obj_get_child_cnt(_dialGroup)] = (int16_t)CLOCK_CENTER_Y;
    lv_obj_t* hub = lv_obj_create(_dialGroup);
    lv_obj_remove_style_all(hub);
    lv_obj_set_size(hub, 10, 10);
    lv_obj_center(hub);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);

    // digital readout, below center: fixed-position slots (see header comment
    // on why each digit gets its own fixed-width slot rather than one label).
    // Retired (see updateFaceModeVisibility()'s showDigits comment) and
    // hidden right away here rather than relying on that function to hide
    // them -- it's only ever called from tickWatchFace()'s mode-transition
    // logic, never once at startup, so leaving these at LVGL's default
    // visible state left the row on screen for the whole first TIME dwell
    // after every boot.
    _digitSlots[0] = createTimeSlot(_faceContent, TIME_DIGIT_W, -58, nullptr);
    _digitSlots[1] = createTimeSlot(_faceContent, TIME_DIGIT_W, -38, nullptr);
    _colonSlots[0] = createTimeSlot(_faceContent, TIME_COLON_W, -24, ":");
    _digitSlots[2] = createTimeSlot(_faceContent, TIME_DIGIT_W, -10, nullptr);
    _digitSlots[3] = createTimeSlot(_faceContent, TIME_DIGIT_W, 10, nullptr);
    _colonSlots[1] = createTimeSlot(_faceContent, TIME_COLON_W, 24, ":");
    _digitSlots[4] = createTimeSlot(_faceContent, TIME_DIGIT_W, 38, nullptr);
    _digitSlots[5] = createTimeSlot(_faceContent, TIME_DIGIT_W, 58, nullptr);
    for (lv_obj_t* slot : _digitSlots) lv_obj_add_flag(slot, LV_OBJ_FLAG_HIDDEN);
    for (lv_obj_t* colon : _colonSlots) lv_obj_add_flag(colon, LV_OBJ_FLAG_HIDDEN);

    // Info label: same row, shown instead of the digit slots when cycling
    // to temperature/humidity. Hidden until the rotation selects it.
    _infoLabel = lv_label_create(_faceContent);
    lv_obj_set_style_text_font(_infoLabel, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_infoLabel, lv_color_white(), 0);
    lv_obj_align(_infoLabel, LV_ALIGN_CENTER, 0, TIME_ROW_Y);
    lv_obj_add_flag(_infoLabel, LV_OBJ_FLAG_HIDDEN);

    // Photo face: a sibling of _faceContent (not a child of it), created
    // after it so it paints on top and fully occludes the dial — see
    // setPhotos()/updateFaceModeVisibility(). Sized to cover the whole
    // round panel; hidden until FaceMode::PHOTO is selected, and only
    // ever selected once at least one photo has actually been set. Which
    // image shows is picked in updateFaceModeVisibility() (round-robin),
    // not here — the initial src doesn't matter since it's hidden.
    _photoImg = lv_image_create(_faceScreen);
    lv_obj_set_size(_photoImg, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(_photoImg, 0, 0);
    if (_photoCount > 0) lv_image_set_src(_photoImg, _photos[0]);
    lv_obj_add_flag(_photoImg, LV_OBJ_FLAG_HIDDEN);

    // CRT scan-bar: a plain translucent rect, sibling of _photoImg
    // created after it so it paints on top. Height/opacity/position
    // jitter logic lives in tickWatchFace(); this just builds the
    // object once (initial size doesn't matter, always overwritten
    // before the first frame it's visible in).
    _photoScanBar = lv_obj_create(_faceScreen);
    lv_obj_remove_style_all(_photoScanBar);
    lv_obj_set_size(_photoScanBar, LCD_H_RES, PHOTO_SCANBAR_H_MAX);
    lv_obj_set_style_bg_color(_photoScanBar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(_photoScanBar, LV_OPA_60, 0);
    lv_obj_set_pos(_photoScanBar, 0, -PHOTO_SCANBAR_H_MAX);
    lv_obj_add_flag(_photoScanBar, LV_OBJ_FLAG_HIDDEN);

    // Static/noise fleck: a second, independent smaller bar that flashes
    // near the main scan-bar on some ticks (not every one, see
    // tickWatchFace()) at a random offset/height/opacity, so the overlay
    // reads as scattered interference rather than one clean moving band.
    _photoNoiseBar = lv_obj_create(_faceScreen);
    lv_obj_remove_style_all(_photoNoiseBar);
    lv_obj_set_style_bg_color(_photoNoiseBar, lv_color_white(), 0);
    lv_obj_set_size(_photoNoiseBar, LCD_H_RES, 2);
    lv_obj_set_pos(_photoNoiseBar, 0, -PHOTO_SCANBAR_H_MAX);
    lv_obj_add_flag(_photoNoiseBar, LV_OBJ_FLAG_HIDDEN);

    // Scanline overlay: created last so it's the topmost _faceScreen
    // child, drawn over the dial, the digits, and photo mode alike.
    // Never hidden/toggled — see kScanlineTile's comment for why this
    // is one tiled image rather than many line objects.
    lv_obj_t* scanlines = lv_obj_create(_faceScreen);
    lv_obj_remove_style_all(scanlines);
    lv_obj_set_size(scanlines, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(scanlines, 0, 0);
    lv_obj_set_style_bg_image_src(scanlines, &kScanlineTile, 0);
    lv_obj_set_style_bg_image_tiled(scanlines, true, 0);
    lv_obj_set_style_bg_image_recolor(scanlines, lv_color_black(), 0);
    lv_obj_set_style_bg_image_recolor_opa(scanlines, LV_OPA_COVER, 0);
}

// --- Watch face ticking -------------------------------------------------------

// Positions one segment of a tapered hand, running from radius r0 to r1
// along angle_deg, pivoting around (cx,cy) — segments don't have to
// start at the pivot, e.g. the mid/tip segments start where the
// previous one ended. Same tight-bounding-box technique as the original
// single-piece hands: each segment's line object is sized/positioned to
// just cover its own two endpoints, not the whole screen, so redraws stay
// small. Width is set once at creation (createTaperedHand), not here.
// (cx,cy) generalizes this beyond the clock hands to any other
// pivot-and-sweep indicator on the face — see the rate gauge's needle.
static void setHandSeg(lv_obj_t* seg, lv_point_precise_t* pts, float angle_deg,
                        int16_t r0, int16_t r1, int16_t cx, int16_t cy) {
    float rad = angle_deg * ((float)M_PI / 180.0f);
    int16_t x0 = cx + (int16_t)roundf(r0 * sinf(rad));
    int16_t y0 = cy - (int16_t)roundf(r0 * cosf(rad));
    int16_t x1 = cx + (int16_t)roundf(r1 * sinf(rad));
    int16_t y1 = cy - (int16_t)roundf(r1 * cosf(rad));

    int16_t min_x = (x0 < x1) ? x0 : x1;
    int16_t max_x = (x0 > x1) ? x0 : x1;
    int16_t min_y = (y0 < y1) ? y0 : y1;
    int16_t max_y = (y0 > y1) ? y0 : y1;
    const int16_t pad = 5; // headroom for the widest segment + rounded caps

    int16_t ox = min_x - pad;
    int16_t oy = min_y - pad;
    int16_t ow = (max_x - min_x) + 2 * pad;
    int16_t oh = (max_y - min_y) + 2 * pad;

    pts[0].x = x0 - ox;
    pts[0].y = y0 - oy;
    pts[1].x = x1 - ox;
    pts[1].y = y1 - oy;

    lv_obj_set_pos(seg, ox, oy);
    lv_obj_set_size(seg, ow, oh);
    lv_line_set_points(seg, pts, 2);
}

// Updates all `count` segments of a tapered hand. radii has count+1
// boundary radii (segment i runs radii[i] -> radii[i+1]) — radii[0] is
// normally 0 (the hub). See fillRadii() for how these are generated.
static void setTaperedHand(lv_obj_t* segs[], lv_point_precise_t segPts[][2],
                            float angle_deg, const int16_t radii[], int count) {
    for (int i = 0; i < count; i++) {
        setHandSeg(segs[i], segPts[i], angle_deg, radii[i], radii[i + 1],
                   CLOCK_CENTER_X, CLOCK_CENTER_Y);
    }
}

static void setDigit(lv_obj_t* slot, uint32_t value) {
    char buf[2] = { (char)('0' + value), '\0' };
    lv_label_set_text(slot, buf);
}

// Bursty CRT-style step size: stutters, jumps, and everything between,
// rather than a tight jitter band around baseSpeed -- see the PHOTO-mode
// call site's original comment for why (a dithered constant rate reads
// as smooth; this doesn't). Shared by the photo-mode distortion overlay
// and the post-photo dial reveal so leaving photo mode reads as one
// continuous interference effect, not two different animations stitched
// together.
static int16_t crtScanStep(int16_t baseSpeed) {
    uint32_t r = esp_random() % 100;
    if (r < 12) return 0; // stutter: hold for a tick
    if (r < 88) return baseSpeed + (int16_t)(esp_random() % 5) - 2;
    return baseSpeed * 3 + (int16_t)(esp_random() % 6); // glitch jump
}

// Randomizes the scan-bar's height/opacity and the separate noise-fleck
// static, both centered on barY -- see crtScanStep()'s comment.
static void updateScanBarNoise(lv_obj_t* scanBar, lv_obj_t* noiseBar, int16_t barY) {
    int16_t barH = PHOTO_SCANBAR_H_MIN
        + (int16_t)(esp_random() % (PHOTO_SCANBAR_H_MAX - PHOTO_SCANBAR_H_MIN + 1));
    lv_obj_set_height(scanBar, barH);
    lv_obj_set_pos(scanBar, 0, barY);
    lv_obj_set_style_bg_opa(scanBar, (lv_opa_t)(30 + esp_random() % 200), 0);

    if (esp_random() % 100 < 35) {
        int16_t offset = (int16_t)(esp_random() % 81) - 40; // -40..+40
        int16_t noiseY = barY + offset;
        if (noiseY < -4) noiseY = -4;
        if (noiseY > LCD_V_RES) noiseY = LCD_V_RES;
        lv_obj_set_height(noiseBar, 1 + (int16_t)(esp_random() % 4));
        lv_obj_set_pos(noiseBar, 0, noiseY);
        lv_obj_set_style_bg_opa(noiseBar, (lv_opa_t)(40 + esp_random() % 180), 0);
        lv_obj_clear_flag(noiseBar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(noiseBar, LV_OBJ_FLAG_HIDDEN);
    }
}

void DispDriverGc9a01RoundClock::tickWatchFace() {
    if (!_timeProvider) return; // not wired up yet

    // Post-photo-mode reveal (set up by updateFaceModeVisibility(true)):
    // a top-to-bottom sweep, same direction as the photo scan-bar and now
    // sharing its bursty crtScanStep()/updateScanBarNoise() styling (see
    // their comments) so leaving photo mode reads as a continuation of
    // the same interference effect rather than a plain, quieter wipe.
    // Each _dialGroup child un-hides the moment the sweep passes its
    // pre-captured _dialRevealY (set in buildUi()) -- spreads ~120
    // objects' first-render cost across the sweep instead of one big
    // pass, in an order that matches their actual on-screen position
    // (top ticks/labels before bottom ones), not creation order. The
    // sweep always runs the full panel height even after every element
    // has been revealed (previously it stopped the moment the last one
    // appeared, which was well before the bottom edge and cut the
    // interference short) so the noisy scan-bar/fleck overlay gets a
    // full, consistent pass every time.
    if (_revealActive) {
        _revealScanY += crtScanStep(DIAL_REVEAL_SPEED);

        uint32_t childCnt = lv_obj_get_child_cnt(_dialGroup);
        for (uint32_t i = 0; i < childCnt; i++) {
            lv_obj_t* child = lv_obj_get_child(_dialGroup, i);
            if (!lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
            if (_dialRevealY[i] <= _revealScanY) {
                lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (_revealScanY > LCD_V_RES) {
            _revealActive = false;
            lv_obj_add_flag(_photoScanBar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(_photoNoiseBar, LV_OBJ_FLAG_HIDDEN);
        } else {
            updateScanBarNoise(_photoScanBar, _photoNoiseBar, _revealScanY);
        }
    }

    DisplayTime dt = _timeProvider->getDisplayTime();

    double s = dt.second;
    uint32_t m = (uint32_t)dt.minute;
    uint32_t h = (uint32_t)(dt.hour % 12);
    uint32_t sec_whole = (uint32_t)dt.second;

    // Date complication — only touches the label once per day. Just the
    // day-of-month number (no weekday, no month), matching a mechanical
    // watch's date window -- see the dial background's "crystal" cutout
    // (setDialBackground()) that it now sits inside.
    if (dt.mday != _dateShownMday) {
        _dateShownMday = dt.mday;
        char dateBuf[4];
        snprintf(dateBuf, sizeof(dateBuf), "%d", dt.mday);
        lv_label_set_text(_dateLabel, dateBuf);
    }

    // Hands always run, regardless of what the info row below is showing —
    // they're the "at a glance" clock and shouldn't disappear while the
    // rotation is on temperature/humidity. Radius boundaries are cheap to
    // recompute every tick (a handful of integer divides) rather than
    // worth caching as members.
    int16_t hourRadii[kHandSegs + 1], minRadii[kHandSegs + 1], secRadii[kHandSegs + 1];
    fillRadii(hourRadii, kHandSegs, HOUR_HAND_LEN);
    fillRadii(minRadii, kHandSegs, MIN_HAND_LEN);
    fillRadii(secRadii, kHandSegs, SEC_HAND_LEN);
    setTaperedHand(_hourSegs, _hourSegPts, h * 30.0f + m * 0.5f, hourRadii, kHandSegs);
    setTaperedHand(_minSegs, _minSegPts, m * 6.0f + (float)s * 0.1f, minRadii, kHandSegs);
    setTaperedHand(_secSegs, _secSegPts, (float)s * 6.0f, secRadii, kHandSegs);

    // Rate gauge needle: 270deg (west/left) at zero, sweeping toward
    // 360/0deg (north) for positive values and 180deg (south) for
    // negative, per setRateGauge()'s contract.
    setHandSeg(_gaugeNeedle, _gaugeNeedlePts, 270.0f + _gaugeNormalized * 90.0f,
               0, GAUGE_NEEDLE_LEN, GAUGE_CENTER_X, GAUGE_CENTER_Y);

    // CRT distortion overlay, only worth animating while photo mode is
    // actually visible: a scan-bar sweeps top->bottom right from the
    // moment the photo appears (more dramatic than a plain cut to a
    // clean image), with a subtle per-tick opacity jitter on the photo
    // itself for flicker; esp_random() is the hardware RNG -- cheap, no
    // seeding needed. Once the sweep reaches the bottom edge, the photo
    // holds clean on screen for kPhotoHoldUs before actually leaving
    // photo mode -- see _photoScanDoneUs's comment.
    if (_faceMode == FaceMode::PHOTO) {
        if (_photoScanDoneUs == 0) {
            _photoScanBarY += crtScanStep(PHOTO_SCANBAR_SPEED);
            if (_photoScanBarY > LCD_V_RES) {
                lv_obj_add_flag(_photoScanBar, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(_photoNoiseBar, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_image_opa(_photoImg, 255, 0);
                _photoScanDoneUs = esp_timer_get_time();
            } else {
                updateScanBarNoise(_photoScanBar, _photoNoiseBar, _photoScanBarY);
                uint8_t opa = 255 - (uint8_t)(esp_random() % 70);
                lv_obj_set_style_image_opa(_photoImg, opa, 0);
            }
        } else if (esp_timer_get_time() - _photoScanDoneUs >= kPhotoHoldUs) {
            _faceMode = FaceMode::TIME;
            _faceModeSinceUs = esp_timer_get_time();
            updateFaceModeVisibility(/*stagedReveal=*/true);
        }
    }

    // Info row: cycles time -> temperature -> humidity -> photo -> time.
    // Skips temperature/humidity entirely (stays on TIME) until a weather
    // reading has actually succeeded at least once; skips photo the same
    // way until setPhotos() has actually been given at least one image.
    int64_t faceModeNowUs = esp_timer_get_time();
    int64_t modeDurationUs;
    if (_faceMode == FaceMode::PHOTO) modeDurationUs = kPhotoModeDurationUs;
    else if (_faceMode == FaceMode::TIME) modeDurationUs = kTimeModeDurationUs;
    else modeDurationUs = kFaceModeDurationUs;
    if (faceModeNowUs - _faceModeSinceUs > modeDurationUs) {
        _faceModeSinceUs = faceModeNowUs;
        bool wasPhoto = (_faceMode == FaceMode::PHOTO);
        do {
            _faceMode = static_cast<FaceMode>((static_cast<int>(_faceMode) + 1) % 4);
        } while (_faceMode != FaceMode::TIME
                 && !(_faceMode == FaceMode::PHOTO ? _photoSet : _weatherValid));
        // Stage the reveal here too on the rare path where
        // kPhotoModeDurationUs's fallback fires before the scan-bar
        // sweep reaches the bottom edge (see tickWatchFace()'s PHOTO
        // block) -- still leaving photo mode, same ~130-object dial as
        // the normal completion path.
        updateFaceModeVisibility(/*stagedReveal=*/wasPhoto);
    }

    // Packs (hour, minute, whole-second) into one comparable value to gate
    // digit refreshes to actual changes — uses the raw 0-23 hour (not the
    // 0-11 `h` used for hand angle above) so AM/PM don't alias to the same
    // key.
    uint32_t shownKey = (uint32_t)dt.hour * 3600u + m * 60u + sec_whole;
    if (_faceMode == FaceMode::TIME && shownKey != _digitsShownSec) {
        _digitsShownSec = shownKey;
        uint32_t hh = h;
        if (hh == 0) hh = 12;
        setDigit(_digitSlots[0], hh / 10);
        setDigit(_digitSlots[1], hh % 10);
        setDigit(_digitSlots[2], m / 10);
        setDigit(_digitSlots[3], m % 10);
        setDigit(_digitSlots[4], sec_whole / 10);
        setDigit(_digitSlots[5], sec_whole % 10);
    }
}

void DispDriverGc9a01RoundClock::updateFaceModeVisibility(bool stagedReveal) {
    // Photo mode occludes the whole dial (hands/ticks/digits/gauge, all
    // under _faceContent) rather than just swapping the info row — flip
    // both containers and return before touching anything below, which
    // only applies to the digit-row/TEMPERATURE/HUMIDITY split.
    bool showPhoto = (_faceMode == FaceMode::PHOTO);
    if (showPhoto) {
        lv_obj_add_flag(_faceContent, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_photoImg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_photoScanBar, LV_OBJ_FLAG_HIDDEN);
        // Round-robin: show _photoIndex, then advance it for next time,
        // so repeated visits to photo mode alternate through the set
        // set via setPhotos() rather than always showing the same one.
        if (_photoCount > 0) {
            lv_image_set_src(_photoImg, _photos[_photoIndex]);
            _photoIndex = (_photoIndex + 1) % _photoCount;
        }
        // Reset the distortion overlay so every entry into photo mode
        // starts the same way: full brightness, scan-bar off the top
        // edge ready to sweep down, noise fleck hidden until the first
        // tick decides whether to show it — see tickWatchFace(). The
        // scan-out runs right away (see kPhotoHoldUs's comment for why
        // the hold comes after it instead), so _photoScanDoneUs resets
        // to "not done yet" here too.
        lv_obj_set_style_image_opa(_photoImg, 255, 0);
        _photoScanBarY = -PHOTO_SCANBAR_H_MAX;
        lv_obj_set_pos(_photoScanBar, 0, _photoScanBarY);
        _photoScanDoneUs = 0;
        lv_obj_add_flag(_photoNoiseBar, LV_OBJ_FLAG_HIDDEN);
        // Cancel any reveal still in progress from a previous exit —
        // harmless to let it keep running underneath the (opaque) photo
        // since it'd be invisible anyway, but no reason to waste ticks
        // on it; the next exit starts a fresh reveal from scratch.
        _revealActive = false;
        return;
    }
    lv_obj_clear_flag(_faceContent, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_photoImg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_photoNoiseBar, LV_OBJ_FLAG_HIDDEN);

    if (stagedReveal) {
        // Hide every _dialGroup child so tickWatchFace() can reveal them
        // as the sweep passes each one's pre-captured _dialRevealY,
        // instead of one big first-render pass — photo mode's own flat
        // bitmap blit is cheap to redraw regardless of what it depicts,
        // but ~120 vector line/gradient/label objects all doing their
        // first render in the same frame is not.
        uint32_t childCnt = lv_obj_get_child_cnt(_dialGroup);
        for (uint32_t i = 0; i < childCnt; i++) {
            lv_obj_add_flag(lv_obj_get_child(_dialGroup, i), LV_OBJ_FLAG_HIDDEN);
        }
        _revealActive = true;
        _revealScanY = -PHOTO_SCANBAR_H_MAX;
        // Reset _photoScanBar back from whatever randomized height/
        // opacity photo mode's distortion overlay last left it at —
        // reused here as the reveal's wipe line, same object, same
        // direction/speed as the photo scan-bar (see tickWatchFace()).
        lv_obj_set_height(_photoScanBar, PHOTO_SCANBAR_H_MAX);
        lv_obj_set_style_bg_opa(_photoScanBar, LV_OPA_60, 0);
        lv_obj_set_pos(_photoScanBar, 0, _revealScanY);
        lv_obj_clear_flag(_photoScanBar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(_photoScanBar, LV_OBJ_FLAG_HIDDEN);
    }

    // Digital HH:MM:SS readout is retired -- the analog hands already
    // show time, and the digit row competed for space/attention with
    // the dial background. Slots stay allocated (setDigit() in
    // tickWatchFace() keeps refreshing their now-hidden content) rather
    // than ripping the layout out, so re-enabling is a one-line flip.
    bool showDigits = false;
    for (lv_obj_t* slot : _digitSlots) {
        if (showDigits) lv_obj_clear_flag(slot, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(slot, LV_OBJ_FLAG_HIDDEN);
    }
    for (lv_obj_t* colon : _colonSlots) {
        if (showDigits) lv_obj_clear_flag(colon, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(colon, LV_OBJ_FLAG_HIDDEN);
    }

    // At this point _faceMode is TEMPERATURE or HUMIDITY (TIME took the
    // showDigits branch above, PHOTO already returned at the top of this
    // function). The rotation in tickWatchFace() already skips both
    // modes while !_weatherValid, so this check should be unreachable in
    // practice -- kept anyway as a second line of defense against ever
    // showing a stale/zeroed "0% RH" if that gate is ever loosened.
    if (showDigits || !_weatherValid) {
        lv_obj_add_flag(_infoLabel, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(_infoLabel, LV_OBJ_FLAG_HIDDEN);
    char buf[16];
    if (_faceMode == FaceMode::TEMPERATURE) {
        snprintf(buf, sizeof(buf), "%.0f F", _tempF);
    } else {
        snprintf(buf, sizeof(buf), "%d%% RH", _humidity);
    }
    lv_label_set_text(_infoLabel, buf);
}

void DispDriverGc9a01RoundClock::setWeatherData(float tempF, int humidity, bool valid) {
    _tempF = tempF;
    _humidity = humidity;
    _weatherValid = valid;
}

void DispDriverGc9a01RoundClock::setPhotos(const lv_image_dsc_t* const* imgs, int count) {
    if (!imgs || count < 0) count = 0;
    if (count > kMaxPhotos) count = kMaxPhotos;
    for (int i = 0; i < count; i++) _photos[i] = imgs[i];
    _photoCount = count;
    _photoSet = (count > 0);
    _photoIndex = 0;
    // Also apply live if buildUi() already ran, even though the header
    // documents this as a before-begin() call — cheap to support and
    // avoids a surprise if a future caller doesn't follow that ordering.
    if (_photoImg && count > 0) lv_image_set_src(_photoImg, _photos[0]);
}

void DispDriverGc9a01RoundClock::setRateGauge(float value, float fullScale) {
    if (fullScale <= 0.0f) fullScale = 1.0f;
    float normalized = value / fullScale;
    if (normalized > 1.0f) normalized = 1.0f;
    if (normalized < -1.0f) normalized = -1.0f;
    _gaugeNormalized = normalized;
}

void DispDriverGc9a01RoundClock::tickTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<DispDriverGc9a01RoundClock*>(lv_timer_get_user_data(timer));
    self->tickWatchFace();
}

void DispDriverGc9a01RoundClock::showClockFace(bool show) {
    if (!_began) return;
    lvgl_port_lock(0);
    lv_scr_load(show ? _faceScreen : _bootScreen);
    lvgl_port_unlock();
    _showingFace = show;
}

// --- IDisplayDriver: status-text bridge --------------------------------------
// No real segment glass behind this driver — setSegments()/setDot() are
// accepted but no-op, and mapAsciiToSegment() has nothing meaningful to
// encode. Only the plain-text path (setChar/setBuffer/writeDisplay) does
// anything, which is all the boot/status messages this app shows need.

void DispDriverGc9a01RoundClock::setBrightness(uint8_t /*level*/) {
    // Backlight is a plain GPIO on/off, not PWM-driven — nothing to scale.
}

void DispDriverGc9a01RoundClock::clear() {
    std::memset(_statusBuf, 0, sizeof(_statusBuf));
}

void DispDriverGc9a01RoundClock::setChar(int position, char character, bool /*dot*/) {
    if (position < 0 || position >= kStatusCells) return;
    _statusBuf[position] = character;
}

void DispDriverGc9a01RoundClock::setSegments(int /*position*/, uint16_t /*mask*/) {}

void DispDriverGc9a01RoundClock::setDot(int /*position*/, bool /*on*/) {}

unsigned long DispDriverGc9a01RoundClock::mapAsciiToSegment(char ascii_char, bool /*dot*/) {
    return (unsigned long)(unsigned char)ascii_char;
}

void DispDriverGc9a01RoundClock::setBuffer(const std::vector<unsigned long>& newBuffer) {
    std::memset(_statusBuf, 0, sizeof(_statusBuf));
    size_t n = newBuffer.size() < (size_t)kStatusCells ? newBuffer.size() : (size_t)kStatusCells;
    for (size_t i = 0; i < n; i++) {
        _statusBuf[i] = (char)newBuffer[i];
    }
}

void DispDriverGc9a01RoundClock::writeDisplay() {
    if (!_began) return;
    char text[kStatusCells + 1];
    std::memcpy(text, _statusBuf, kStatusCells);
    text[kStatusCells] = '\0';

    // The character-grid animations (StaticTextAnimation etc.) pad every
    // unused cell with spaces -- harmless on segment glass, where blank
    // cells just need clearing, but on this center-aligned wrapped label
    // that trailing run of spaces becomes part of the last visual line's
    // width, so lv_label's centering shifts the visible text left of true
    // center. Trim it before handing the string to LVGL.
    size_t len = std::strlen(text);
    while (len > 0 && text[len - 1] == ' ') text[--len] = '\0';

    lvgl_port_lock(0);
    lv_label_set_text(_statusLabel, text);
    lvgl_port_unlock();
}

void DispDriverGc9a01RoundClock::getFrameData(unsigned long* buffer) {
    for (int i = 0; i < kStatusCells; i++) {
        buffer[i] = (unsigned long)(unsigned char)_statusBuf[i];
    }
}
