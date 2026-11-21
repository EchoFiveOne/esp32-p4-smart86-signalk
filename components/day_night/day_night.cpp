#include "day_night.hpp"
#include "signalk_client.hpp"
#include "unit_config.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <sys/time.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "day_night";
static const char *NVS_NS = "disp";

static const int    TICK_MS        = 250;
static const double RAMP_TOP_DEG   = 6.0;     // full day brightness above this sun elevation
static const double HYST_DEG       = 0.25;    // night switch hysteresis (each side)
static const double RISE_SET_DEG   = -0.833;  // standard sunrise/sunset (refraction + disc)
static const uint32_t POS_MAX_AGE_MS = 10 * 60 * 1000;

static const char *LAT_PATH = "navigation.position.latitude";
static const char *LON_PATH = "navigation.position.longitude";

static SemaphoreHandle_t   s_lock;
static DayNightSettings    s_set = { DN_MODE_AUTO, 100, 20, true, 0 };
static DayNightStatus      s_st  = {};
static volatile bool       s_dirty = true;

static lv_disp_t                *s_disp;
static day_night_backlight_cb_t  s_backlight;
static void (*s_orig_flush)(lv_disp_drv_t *, const lv_area_t *, lv_color_t *);

// Read by the flush callback
static volatile bool     s_red   = false;
static volatile uint16_t s_scale = 256;     // software dim, 256 = none

static volatile bool s_sntp_synced = false;
static int64_t s_sk_set_us   = 0;
static int     s_hw_pct      = -1;
static bool    s_subscribed  = false;
static double  s_saved_lat = 0, s_saved_lon = 0;
static bool    s_have_saved = false;
static SunDay  s_sd_today = {}, s_sd_tomorrow = {};
static SunInfo s_sun = {};
static int     s_rs_yday = -1;
static double  s_rs_lat = 999, s_rs_lon = 999;
static int64_t s_rs_us = 0;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

// ---------------------------------------------------------------------------
// Sun position (low precision solar ephemeris, ~0.01 deg)
// ---------------------------------------------------------------------------

static double sun_pos(double lat, double lon, double t_unix, double *az_deg = nullptr)
{
    const double D2R = M_PI / 180.0;
    double n   = t_unix / 86400.0 - 10957.5;            // days since J2000.0
    double L   = fmod(280.460 + 0.9856474 * n, 360.0);
    double g   = fmod(357.528 + 0.9856003 * n, 360.0) * D2R;
    double lam = (L + 1.915 * sin(g) + 0.020 * sin(2 * g)) * D2R;
    double eps = (23.439 - 0.0000004 * n) * D2R;
    double ra  = atan2(cos(eps) * sin(lam), cos(lam));
    double dec = asin(sin(eps) * sin(lam));
    double gmst = fmod(280.46061837 + 360.98564736629 * n, 360.0);
    double ha  = (gmst + lon) * D2R - ra;
    double la  = lat * D2R;
    double el  = asin(sin(la) * sin(dec) + cos(la) * cos(dec) * cos(ha));
    if (az_deg) {
        double az = atan2(-sin(ha) * cos(dec), sin(dec) * cos(la) - cos(dec) * sin(la) * cos(ha)) / D2R;
        *az_deg = az < 0 ? az + 360.0 : az;
    }
    return el / D2R;
}

static double sun_elev_deg(double lat, double lon, double t_unix) { return sun_pos(lat, lon, t_unix); }

static time_t local_midnight(time_t t, int add_days)
{
    struct tm lt;
    localtime_r(&t, &lt);
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    lt.tm_mday += add_days;
    lt.tm_isdst = -1;
    return mktime(&lt);
}

// Twilight / rise / set / noon times for the local day starting at `start`
static void compute_sun_day(double lat, double lon, time_t start, SunDay *d)
{
    static const struct { double elev; uint8_t up, down; } s_thr[] = {
        { -18.0,  SUN_ASTRO_DAWN, SUN_ASTRO_DUSK },
        { -12.0,  SUN_NAUT_DAWN,  SUN_NAUT_DUSK },
        { -6.0,   SUN_CIVIL_DAWN, SUN_CIVIL_DUSK },
        { RISE_SET_DEG, SUN_RISE, SUN_SET },
        { 6.0,    SUN_GOLDEN_END, SUN_GOLDEN_START },
    };
    const int STEP = 600;
    memset(d, 0, sizeof(*d));
    d->start = start;
    d->end = local_midnight(start + 12 * 3600, 1);
    int n = (int)((d->end - start + STEP - 1) / STEP) + 1;
    if (n > 160) n = 160;
    double e[160];
    int imax = 0;
    for (int i = 0; i < n; i++) {
        time_t t = start + (time_t)i * STEP;
        if (t > d->end) t = d->end;
        e[i] = sun_elev_deg(lat, lon, (double)t);
        if (e[i] > e[imax]) imax = i;
    }
    for (const auto &th : s_thr) {
        for (int i = 1; i < n; i++) {
            double fa = e[i - 1] - th.elev, fb = e[i] - th.elev;
            if ((fa < 0) == (fb < 0)) continue;
            uint8_t which = fa < 0 ? th.up : th.down;
            if (d->ev[which]) continue;
            double a = (double)(start + (time_t)(i - 1) * STEP), b = a + STEP;
            if (b > (double)d->end) b = (double)d->end;
            for (int k = 0; k < 12; k++) {
                double m = (a + b) / 2;
                double fm = sun_elev_deg(lat, lon, m) - th.elev;
                if ((fa < 0) == (fm < 0)) { a = m; fa = fm; } else { b = m; }
            }
            d->ev[which] = (time_t)llround((a + b) / 2);
        }
    }
    // Noon: golden-section search around the highest sample
    double a = (double)start + (imax - 1) * (double)STEP, b = (double)start + (imax + 1) * (double)STEP;
    for (int k = 0; k < 30; k++) {
        double m1 = b - (b - a) * 0.618, m2 = a + (b - a) * 0.618;
        if (sun_elev_deg(lat, lon, m1) < sun_elev_deg(lat, lon, m2)) a = m1; else b = m2;
    }
    time_t noon = (time_t)llround((a + b) / 2);
    if (noon >= start && noon < d->end) d->ev[SUN_NOON] = noon;
    d->noon_elev = (float)sun_elev_deg(lat, lon, (double)noon);

    time_t rise = d->ev[SUN_RISE], set = d->ev[SUN_SET];
    if (rise && set && set > rise) d->day_length_s = (int)(set - rise);
    else if (rise && set)          d->day_length_s = (int)((d->end - rise) + (set - start));
    else if (rise)                 d->day_length_s = (int)(d->end - rise);
    else if (set)                  d->day_length_s = (int)(set - start);
    else                           d->day_length_s = d->noon_elev > RISE_SET_DEG ? (int)(d->end - start) : 0;
}

// Last event of the day (twilight over)
static time_t sun_day_last(const SunDay &d)
{
    time_t last = 0;
    for (int i = SUN_NOON; i < SUN_EVENT_COUNT; i++) if (d.ev[i] > last) last = d.ev[i];
    return last;
}

static int local_secs(time_t t)
{
    struct tm lt;
    localtime_r(&t, &lt);
    return lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
}

static bool time_is_valid(time_t t)
{
    struct tm g;
    gmtime_r(&t, &g);
    return g.tm_year >= 2024 - 1900;
}

// ---------------------------------------------------------------------------
// Pixel filter (red night vision + software dimming), RGB565
// ---------------------------------------------------------------------------

static inline uint16_t filter_px(uint16_t v, bool red, uint32_t sc)
{
    uint32_t r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f;
    if (red) {
        // Luminance on a 6-bit scale, shown on the red channel only
        uint32_t y = (r * 2 * 77 + g * 150 + b * 2 * 29) >> 8;
        y = (y * sc) >> 8;
        return (uint16_t)((y >> 1) << 11);
    }
    r = (r * sc) >> 8;
    g = (g * sc) >> 8;
    b = (b * sc) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void day_night_filter_pixels(uint16_t *px, size_t count)
{
    bool red = s_red;
    uint32_t sc = s_scale;
    if (!red && sc >= 256) return;
    uint16_t last_in = px[0] ^ 0xffff, last_out = 0;
    for (size_t i = 0; i < count; i++) {
        uint16_t v = px[i];
        if (v != last_in) { last_in = v; last_out = filter_px(v, red, sc); }
        px[i] = last_out;
    }
}

static void flush_wrap(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    if (s_red || s_scale < 256) {
        day_night_filter_pixels((uint16_t *)px, (size_t)lv_area_get_size(area));
    }
    s_orig_flush(drv, area, px);
}

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------

static void clamp_settings(DayNightSettings *s)
{
    if (s->mode >= DN_MODE_COUNT) s->mode = DN_MODE_AUTO;
    if (s->day_pct < 5)     s->day_pct = 5;
    if (s->day_pct > 100)   s->day_pct = 100;
    if (s->night_pct < 1)   s->night_pct = 1;
    if (s->night_pct > 100) s->night_pct = 100;
    if (s->switch_elev > 6)   s->switch_elev = 6;
    if (s->switch_elev < -18) s->switch_elev = -18;
}

static void load_settings(int default_day_pct)
{
    nvs_handle_t h;
    bool have_day = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t u8;
        int8_t i8;
        int32_t i32a, i32b;
        if (nvs_get_u8(h, "mode", &u8) == ESP_OK)  s_set.mode = u8;
        if (nvs_get_u8(h, "day", &u8) == ESP_OK)   { s_set.day_pct = u8; have_day = true; }
        if (nvs_get_u8(h, "night", &u8) == ESP_OK) s_set.night_pct = u8;
        if (nvs_get_u8(h, "red", &u8) == ESP_OK)   s_set.red_filter = u8 != 0;
        if (nvs_get_i8(h, "sw", &i8) == ESP_OK)    s_set.switch_elev = i8;
        if (nvs_get_i32(h, "lat", &i32a) == ESP_OK && nvs_get_i32(h, "lon", &i32b) == ESP_OK) {
            s_saved_lat = i32a / 1e5;
            s_saved_lon = i32b / 1e5;
            s_have_saved = true;
        }
        nvs_close(h);
    }
    if (!have_day && default_day_pct > 0) {
        // Old Instruments brightness (10..100 % backlight) on the new scale
        s_set.day_pct = (uint8_t)(20 + default_day_pct * 80 / 100);
    }
    clamp_settings(&s_set);
}

static void save_settings(const DayNightSettings *s)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "mode", s->mode);
    nvs_set_u8(h, "day", s->day_pct);
    nvs_set_u8(h, "night", s->night_pct);
    nvs_set_u8(h, "red", s->red_filter ? 1 : 0);
    nvs_set_i8(h, "sw", s->switch_elev);
    nvs_commit(h);
    nvs_close(h);
}

static void save_position(double lat, double lon)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "lat", (int32_t)lround(lat * 1e5));
    nvs_set_i32(h, "lon", (int32_t)lround(lon * 1e5));
    nvs_commit(h);
    nvs_close(h);
    s_saved_lat = lat;
    s_saved_lon = lon;
    s_have_saved = true;
}

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------

static void update_time_source(DayNightStatus *st, time_t *now)
{
    int64_t now_us = esp_timer_get_time();
    int64_t sk;
    uint32_t age;
    int src;
    if (!s_sntp_synced && signalk_client_get_time(&sk, &age, &src) && age < 30000) {
        bool sys_valid = time_is_valid(*now);
        bool drift = llabs((long long)(sk - (int64_t)*now)) > 2;
        if (!sys_valid || (drift && now_us - s_sk_set_us > 600LL * 1000000LL)) {
            struct timeval tv = { (time_t)sk, 0 };
            settimeofday(&tv, nullptr);
            s_sk_set_us = now_us;
            *now = (time_t)sk;
            ESP_LOGI(TAG, "Clock set from Signal K %s", src == 1 ? "GPS datetime" : "timestamp");
        }
    }
    st->time_valid  = time_is_valid(*now);
    st->time_source = !st->time_valid ? 0 : (s_sntp_synced || !s_sk_set_us ? 1 : 2);
}

static void update_position(DayNightStatus *st)
{
    if (!s_subscribed) s_subscribed = signalk_client_subscribe("navigation.position", 5000);

    const char *paths[2] = { LAT_PATH, LON_PATH };
    PathValue v[2];
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (signalk_client_get_values(paths, 2, v) && v[0].valid && v[1].valid &&
        now_ms - v[0].last_update_ms < POS_MAX_AGE_MS) {
        st->pos_valid = true;
        st->pos_live  = true;
        st->lat = v[0].value;
        st->lon = v[1].value;
        if (!s_have_saved || fabs(st->lat - s_saved_lat) > 0.05 || fabs(st->lon - s_saved_lon) > 0.05) {
            save_position(st->lat, st->lon);
        }
    } else if (s_have_saved) {
        st->pos_valid = true;
        st->pos_live  = false;
        st->lat = s_saved_lat;
        st->lon = s_saved_lon;
    } else {
        st->pos_valid = false;
        st->pos_live  = false;
    }
}

static void apply_output(int level, bool red)
{
    int hw = level >= 20 ? (level - 20) * 100 / 80 : 0;
    uint16_t sc = level >= 20 ? 256 : (uint16_t)(level * 256 / 20);
    if (sc < 13) sc = 13;
    if (hw != s_hw_pct) {
        s_hw_pct = hw;
        if (s_backlight) s_backlight(hw);
    }
    if (red != s_red || sc != s_scale) {
        s_red = red;
        s_scale = sc;
        if (s_disp) lv_obj_invalidate(lv_disp_get_scr_act(s_disp));
    }
}

static void tick(lv_timer_t *)
{
    static int64_t last_us = 0;
    int64_t now_us = esp_timer_get_time();
    bool dirty = s_dirty;
    if (!dirty && now_us - last_us < 1000000) return;
    last_us = now_us;
    s_dirty = false;

    LOCK();
    DayNightSettings set = s_set;
    DayNightStatus st = s_st;
    UNLOCK();

    time_t now = time(nullptr);
    update_time_source(&st, &now);
    update_position(&st);

    if (st.pos_valid && unit_config_tz_is_auto()) {
        const char *name = day_night_tz_lookup(st.lat, st.lon, st.tz_posix, sizeof(st.tz_posix));
        snprintf(st.tz_name, sizeof(st.tz_name), "%s", name);
        unit_config_set_auto_tz(st.tz_posix);
    } else {
        // Manual zone (or Auto without a position yet: last saved auto zone)
        snprintf(st.tz_name, sizeof(st.tz_name), "%s", unit_option_name(UNIT_CAT_TIMEZONE));
        snprintf(st.tz_posix, sizeof(st.tz_posix), "%s", unit_config_get_tz_posix());
    }

    st.sun_valid = st.time_valid && st.pos_valid;
    SunInfo sun = {};
    if (st.sun_valid) {
        double az = 0;
        st.elevation = (float)sun_pos(st.lat, st.lon, (double)now, &az);
        struct tm lt;
        localtime_r(&now, &lt);
        if (lt.tm_yday != s_rs_yday || fabs(st.lat - s_rs_lat) > 0.05 || fabs(st.lon - s_rs_lon) > 0.05 ||
            now_us - s_rs_us > 600LL * 1000000LL || dirty) {
            time_t mid = local_midnight(now, 0);
            compute_sun_day(st.lat, st.lon, mid, &s_sd_today);
            compute_sun_day(st.lat, st.lon, s_sd_today.end, &s_sd_tomorrow);
            s_rs_yday = lt.tm_yday;
            s_rs_lat = st.lat;
            s_rs_lon = st.lon;
            s_rs_us = now_us;
        }
        st.sunrise_s = s_sd_today.ev[SUN_RISE] ? local_secs(s_sd_today.ev[SUN_RISE]) : -1;
        st.sunset_s  = s_sd_today.ev[SUN_SET]  ? local_secs(s_sd_today.ev[SUN_SET])  : -1;
        sun.valid = true;
        sun.lat = st.lat;
        sun.lon = st.lon;
        sun.elevation = st.elevation;
        sun.azimuth = (float)az;
        sun.today = s_sd_today;
        sun.tomorrow = s_sd_tomorrow;
        time_t last = sun_day_last(s_sd_today);
        sun.show_tomorrow = last && now > last;
    } else {
        st.sunrise_s = st.sunset_s = -1;
    }

    double sw = set.switch_elev;
    bool night = st.night;
    int level = set.day_pct;
    if (set.mode == DN_MODE_DAY) {
        night = false;
    } else if (set.mode == DN_MODE_NIGHT) {
        night = true;
        level = set.night_pct;
    } else if (!st.sun_valid) {
        night = false;
    } else {
        double e = st.elevation;
        if (night && e > sw + HYST_DEG) night = false;
        else if (!night && e < sw - HYST_DEG) night = true;
        if (night) {
            level = set.night_pct;
        } else {
            double top = RAMP_TOP_DEG > sw + 1 ? RAMP_TOP_DEG : sw + 1;
            double t = (e - sw) / (top - sw);
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            level = (int)lround(set.night_pct + (set.day_pct - set.night_pct) * t);
        }
    }
    if (night != st.night) {
        ESP_LOGI(TAG, "%s mode (sun %.1f deg)", night ? "Night" : "Day", st.elevation);
    }
    st.night = night;
    st.level = level;
    apply_output(level, night && set.red_filter);

    LOCK();
    s_st = st;
    s_sun = sun;
    UNLOCK();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void day_night_init(lv_disp_t *disp, day_night_backlight_cb_t backlight, int default_day_pct)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    s_disp = disp;
    s_backlight = backlight;
    load_settings(default_day_pct);
    s_st.sunrise_s = s_st.sunset_s = -1;
    if (disp && disp->driver && disp->driver->flush_cb) {
        s_orig_flush = disp->driver->flush_cb;
        disp->driver->flush_cb = flush_wrap;
    }
    s_subscribed = signalk_client_subscribe("navigation.position", 5000);
    signalk_client_subscribe("navigation.datetime", 5000);
    ESP_LOGI(TAG, "mode=%d day=%d%% night=%d%% red=%d switch=%d deg%s",
             s_set.mode, s_set.day_pct, s_set.night_pct, s_set.red_filter, s_set.switch_elev,
             s_have_saved ? " (saved position)" : "");
    tick(nullptr);
    lv_timer_create(tick, TICK_MS, nullptr);
}

void day_night_get_settings(DayNightSettings *out)
{
    LOCK();
    *out = s_set;
    UNLOCK();
}

void day_night_set_settings(const DayNightSettings *s, bool save)
{
    DayNightSettings c = *s;
    clamp_settings(&c);
    LOCK();
    s_set = c;
    UNLOCK();
    if (save) save_settings(&c);
    s_dirty = true;
}

void day_night_get_status(DayNightStatus *out)
{
    LOCK();
    *out = s_st;
    UNLOCK();
}

bool day_night_is_night(void) { return s_st.night; }

void day_night_note_sntp_sync(void) { s_sntp_synced = true; s_dirty = true; }

bool day_night_virtual_value(const char *path, float *out)
{
    if (!path || strncmp(path, "panel.", 6) != 0) return false;
    const char *p = path + 6;
    time_t now = time(nullptr);
    if (!time_is_valid(now)) return false;
    if (strcmp(p, "time.local") == 0) { *out = (float)local_secs(now); return true; }
    if (strcmp(p, "time.utc") == 0)   { *out = (float)(now % 86400); return true; }
    if (strcmp(p, "date.local") == 0) {
        struct tm lt;
        localtime_r(&now, &lt);
        *out = (float)((lt.tm_mon + 1) * 100 + lt.tm_mday);
        return true;
    }
    if (!s_st.sun_valid) return false;
    if (strcmp(p, "sun.elevation") == 0 || strcmp(p, "sun") == 0) { *out = s_st.elevation * (float)(M_PI / 180.0); return true; }
    if (strncmp(p, "sun.", 4) != 0) return false;
    p += 4;
    SunInfo s;
    if (!day_night_sun(&s)) return false;
    if (strcmp(p, "azimuth") == 0) { *out = s.azimuth * (float)(M_PI / 180.0); return true; }
    const SunDay &d = s.show_tomorrow ? s.tomorrow : s.today;
    if (strcmp(p, "daylength") == 0) { *out = (float)d.day_length_s; return true; }
    if (strcmp(p, "sunrise.in") == 0 || strcmp(p, "sunset.in") == 0) {
        int ev = p[3] == 'r' ? SUN_RISE : SUN_SET;
        time_t t = s.today.ev[ev] > now ? s.today.ev[ev] : s.tomorrow.ev[ev] > now ? s.tomorrow.ev[ev] : 0;
        if (!t) return false;
        *out = (float)(t - now);
        return true;
    }
    static const struct { const char *name; uint8_t ev; } s_ev[] = {
        { "sunrise", SUN_RISE }, { "sunset", SUN_SET }, { "noon", SUN_NOON },
        { "dawn.astronomical", SUN_ASTRO_DAWN }, { "dawn.nautical", SUN_NAUT_DAWN },
        { "dawn.civil", SUN_CIVIL_DAWN }, { "dusk.civil", SUN_CIVIL_DUSK },
        { "dusk.nautical", SUN_NAUT_DUSK }, { "dusk.astronomical", SUN_ASTRO_DUSK },
        { "golden.morning", SUN_GOLDEN_END }, { "golden.evening", SUN_GOLDEN_START },
    };
    for (const auto &e : s_ev) {
        if (strcmp(p, e.name) == 0) {
            if (!d.ev[e.ev]) return false;
            *out = (float)local_secs(d.ev[e.ev]);
            return true;
        }
    }
    return false;
}

bool day_night_sun(SunInfo *out)
{
    if (!s_lock) { memset(out, 0, sizeof(*out)); return false; }
    LOCK();
    *out = s_sun;
    UNLOCK();
    return out->valid;
}

float day_night_sun_elevation_at(double lat, double lon, time_t t)
{
    return (float)sun_elev_deg(lat, lon, (double)t);
}

int day_night_sky_state(float e)
{
    if (e >= 6.0f)                 return SKY_DAY;
    if (e >= (float)RISE_SET_DEG)  return SKY_GOLDEN;
    if (e >= -6.0f)                return SKY_CIVIL;
    if (e >= -12.0f)               return SKY_NAUTICAL;
    if (e >= -18.0f)               return SKY_ASTRO;
    return SKY_NIGHT;
}

const char *day_night_sky_name(int state)
{
    static const char *s_names[] = { "Night", "Astronomical twilight", "Nautical twilight",
                                     "Civil twilight", "Golden hour", "Daylight" };
    return state >= 0 && state <= SKY_DAY ? s_names[state] : "";
}
