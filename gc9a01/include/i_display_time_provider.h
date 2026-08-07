// i_display_time_provider.h — the contract disp_driver_gc9a01_round_clock
// (or any similar analog+digital round-LCD watch face driver) needs from
// whoever decides "what time to show". Exists so the display driver
// itself stays generic and reusable: it renders whatever DisplayTime it's
// given, with no notion of real vs. perturbed time, wobble, chaos, or any
// other app-specific concept — see temporal_anomaly_time_source.h for
// where that lives in this app.

#pragma once

// A fully-resolved, ready-to-render time. `second` carries the fraction
// (0.0-59.999...) so hands can sweep smoothly; a provider that wants a
// hand to visibly jump instead of sweep (e.g. this app's chaos level 11)
// can just return a whole-number second — the driver doesn't treat that
// as a special case, it just draws whatever it's given.
struct DisplayTime {
    int hour = 0;         // 0-23
    int minute = 0;       // 0-59
    double second = 0.0;  // 0.0-59.999...
    int wday = 0;         // 0-6, Sunday = 0 (struct tm convention)
    int mday = 1;         // 1-31
};

class IDisplayTimeProvider {
public:
    virtual ~IDisplayTimeProvider() = default;

    // Called once per watch-face tick (~30ms).
    virtual DisplayTime getDisplayTime() = 0;
};
