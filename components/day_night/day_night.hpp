#pragma once

// Panel-wide display controller: backlight, automatic day/night switching from
// the boat position and sun elevation, red night filter and software dimming
// (applied to every app via the LVGL flush callback), automatic time zone from
// position and a fallback clock source from Signal K when NTP is unavailable.
//
// day_night_init/virtual_value run on the LVGL thread. Settings and status
// accessors are thread-safe; new settings take effect on the next controller
// tick (250 ms).

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "lvgl.h"

enum DayNightMode : uint8_t {
    DN_MODE_AUTO = 0,   // follow the sun
    DN_MODE_DAY,        // always day (auto off)
    DN_MODE_NIGHT,      // always night
    DN_MODE_COUNT
};

struct DayNightSettings {
    uint8_t mode;          // DayNightMode
    uint8_t day_pct;       // 5..100 brightness in daylight
    uint8_t night_pct;     // 1..100 brightness at night
    bool    red_filter;    // render everything in red at night
    int8_t  switch_elev;   // sun elevation (deg) where night starts: 0, -6, -12
};

struct DayNightStatus {
    bool    time_valid;
    int     time_source;   // 0 none, 1 NTP, 2 Signal K (GPS datetime / timestamps)
    bool    pos_valid;
    bool    pos_live;      // false = last saved position
    double  lat, lon;
    bool    sun_valid;
    float   elevation;     // degrees
    int     sunrise_s;     // local seconds after midnight, -1 = none today
    int     sunset_s;
    bool    night;
    int     level;         // effective brightness 1..100
    char    tz_name[32];   // auto time zone region
    char    tz_posix[64];
};

typedef void (*day_night_backlight_cb_t)(int percent);

// default_day_pct seeds the day brightness on first run (no saved settings).
void day_night_init(lv_disp_t *disp, day_night_backlight_cb_t backlight, int default_day_pct);

void day_night_get_settings(DayNightSettings *out);
void day_night_set_settings(const DayNightSettings *s, bool save);
void day_night_get_status(DayNightStatus *out);
bool day_night_is_night(void);

// Call from the SNTP sync notification callback (any task).
void day_night_note_sntp_sync(void);

// Values for the virtual "panel.*" dashboard paths. Returns false if unknown
// or not currently available. Times are local seconds after midnight; the
// date is month * 100 + day; sun elevation / azimuth are radians; durations
// (day length, time to sunrise / sunset) are seconds. Sunrise, sunset, noon
// and twilight times refer to today until today's twilight has ended, then
// to tomorrow.
bool day_night_virtual_value(const char *path, float *out);

// --- Sun: twilight, rise / set, noon --------------------------------------
enum SunEvent : uint8_t {
    SUN_ASTRO_DAWN = 0,   // -18 deg rising
    SUN_NAUT_DAWN,        // -12
    SUN_CIVIL_DAWN,       // -6
    SUN_RISE,             // -0.833 (upper limb + refraction)
    SUN_GOLDEN_END,       // +6 rising: morning golden hour ends
    SUN_NOON,             // highest elevation
    SUN_GOLDEN_START,     // +6 setting: evening golden hour starts
    SUN_SET,
    SUN_CIVIL_DUSK,
    SUN_NAUT_DUSK,
    SUN_ASTRO_DUSK,
    SUN_EVENT_COUNT
};

struct SunDay {
    time_t start, end;              // local midnight .. next local midnight
    time_t ev[SUN_EVENT_COUNT];     // 0 = does not happen that day
    float  noon_elev;               // deg
    int    day_length_s;            // sunrise to sunset
};

struct SunInfo {
    bool   valid;                   // clock and position known
    double lat, lon;
    float  elevation, azimuth;      // deg now (azimuth true, 0..360)
    bool   show_tomorrow;           // today's twilight is over: tomorrow is the relevant day
    SunDay today, tomorrow;
};

enum SkyState : uint8_t { SKY_NIGHT = 0, SKY_ASTRO, SKY_NAUTICAL, SKY_CIVIL, SKY_GOLDEN, SKY_DAY };

bool        day_night_sun(SunInfo *out);
float       day_night_sun_elevation_at(double lat, double lon, time_t t);   // deg
int         day_night_sky_state(float elevation_deg);                        // SkyState
const char *day_night_sky_name(int state);

// Apply the current night filter / dimming to RGB565 pixels (screenshots).
void day_night_filter_pixels(uint16_t *px, size_t count);

// Auto time zone lookup: returns the region name and fills posix.
const char *day_night_tz_lookup(double lat, double lon, char *posix, size_t posix_len);
