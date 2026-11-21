#include "tides.hpp"
#include "day_night.hpp"
#include "signalk_client.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>

static const char *TAG = "tides";

static const char *TIDE_FILE = "/dash/tides.bin";
static const char *TIDE_TMP  = "/dash/tides.tmp";
static const char *NVS_NS    = "tides";

static const time_t   EPOCH_2020       = 1577836800;   // table times are minutes since 2020-01-01 UTC
static const double   AUTO_MAX_NM      = 50.0;          // nearest port must be within this
static const double   RESELECT_NM      = 2.0;           // re-pick the nearest port after moving this far
static const uint32_t DEPTH_MAX_AGE_MS = 30000;
static const uint32_t BARO_MAX_AGE_MS  = 15 * 60 * 1000;
static const double   D2R              = M_PI / 180.0;

static const char *P_SURFACE = "environment.depth.belowSurface";
static const char *P_TRANSD  = "environment.depth.belowTransducer";
static const char *P_S2T     = "environment.depth.surfaceToTransducer";
static const char *P_PRESS   = "environment.outside.pressure";

// ---------------------------------------------------------------------------
// tides.bin v1 (little endian, see tools/linz_tides_pack.py)
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct FileHdr {
    char     magic[4];
    uint16_t version, std_count, port_count, reserved;
    uint32_t event_count, first_min, last_min, build_unix, reserved2;
};
struct StdRec {
    uint32_t first, count;
    int16_t  msl_cm;
    uint16_t port_index;
    uint32_t reserved;
};
struct PortRec {
    char     name[32];
    float    lat, lon;
    uint16_t std_index;
    int16_t  hw_diff, lw_diff, msl_cm;
    uint16_t ratio;        // x1000, 0 = heights unknown
    uint8_t  type, flags;  // type 0 standard, 1 secondary; flags bit0 estimated
};
#pragma pack(pop)
static_assert(sizeof(FileHdr) == 32, "header");
static_assert(sizeof(StdRec) == 16, "std");
static_assert(sizeof(PortRec) == 52, "port");

struct Table {
    uint8_t        *buf;
    size_t          size;
    const FileHdr  *hdr;
    const StdRec   *std;
    const PortRec  *port;
    const uint32_t *times;
    const int16_t  *heights;
};

struct Settings {
    char  port[32];     // "" = nearest
    bool  offset_set;
    float offset_m;     // transducer depth below the surface
    bool  baro;
};

static SemaphoreHandle_t s_mtx;
static Table     s_tab;
static uint32_t  s_tab_gen;
static char      s_load_err[64] = "not loaded yet";
static Settings  s_set = { "", false, 0.0f, true };

static int       s_sel = -1;
static bool      s_sel_done, s_sel_manual;
static uint32_t  s_sel_gen;
static double    s_sel_lat, s_sel_lon;

static TideInfo  s_info;
static MoonInfo  s_moon;
static bool      s_subscribed;

#define LOCK()   xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

static bool clock_valid(time_t t)
{
    struct tm g;
    gmtime_r(&t, &g);
    return g.tm_year >= 2024 - 1900;
}

static int local_tod(time_t t)
{
    struct tm lt;
    localtime_r(&t, &lt);
    return lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
}

static double dist_nm(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * D2R, p2 = lat2 * D2R;
    double dp = p2 - p1, dl = (lon2 - lon1) * D2R;
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 2 * 3440.065 * asin(fmin(1.0, sqrt(a)));
}

// ---------------------------------------------------------------------------
// Table access (caller holds the lock)
// ---------------------------------------------------------------------------

static bool port_has_heights(const PortRec &p) { return p.type == 0 || p.ratio > 0; }

static int ev_count(const Table &tb, int port)
{
    return (int)tb.std[tb.port[port].std_index].count;
}

static bool ev_at(const Table &tb, int port, int k, float baro, TideEvent *e)
{
    const PortRec &p = tb.port[port];
    const StdRec &s = tb.std[p.std_index];
    if (k < 0 || (uint32_t)k >= s.count) return false;
    uint32_t raw = tb.times[s.first + k];
    bool high = (raw & 0x80000000u) != 0;
    int64_t min = raw & 0x7FFFFFFFu;
    float hs = tb.heights[s.first + k] / 100.0f;
    if (p.type == 1) {
        min += high ? p.hw_diff : p.lw_diff;
        hs = p.msl_cm / 100.0f + (hs - s.msl_cm / 100.0f) * (p.ratio / 1000.0f);
    }
    e->t = EPOCH_2020 + (time_t)min * 60;
    e->h = hs + baro;
    e->high = high;
    return true;
}

// Index of the last event at or before `when` (-1 if none)
static int ev_find(const Table &tb, int port, time_t when)
{
    int lo = 0, hi = ev_count(tb, port) - 1, ans = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        TideEvent e;
        ev_at(tb, port, mid, 0, &e);
        if (e.t <= when) { ans = mid; lo = mid + 1; }
        else             { hi = mid - 1; }
    }
    return ans;
}

// Cosine interpolation between the surrounding high and low water
static bool height_at(const Table &tb, int port, time_t when, float baro, float *h, float *rate)
{
    int k = ev_find(tb, port, when);
    TideEvent a, b;
    if (k < 0 || !ev_at(tb, port, k, baro, &a) || !ev_at(tb, port, k + 1, baro, &b)) return false;
    double dt = (double)(b.t - a.t);
    if (dt <= 0) return false;
    double f = (double)(when - a.t) / dt;
    *h = (float)(a.h + (b.h - a.h) * (1.0 - cos(M_PI * f)) / 2.0);
    if (rate) *rate = (float)((b.h - a.h) * M_PI / 2.0 * sin(M_PI * f) / dt * 3600.0);
    return true;
}

static int port_by_name(const Table &tb, const char *name)
{
    for (int i = 0; i < tb.hdr->port_count; i++) {
        if (strncasecmp(tb.port[i].name, name, sizeof(tb.port[i].name)) == 0) return i;
    }
    return -1;
}

static void select_port(const DayNightStatus &pos)
{
    if (!s_tab.buf) { s_sel = -1; s_sel_done = false; return; }
    if (s_sel_done && s_sel_gen != s_tab_gen) s_sel_done = false;

    if (s_set.port[0]) {
        if (s_sel_done && s_sel_manual) return;
        s_sel = port_by_name(s_tab, s_set.port);
        s_sel_manual = true;
        s_sel_done = true;
        s_sel_gen = s_tab_gen;
        return;
    }
    if (s_sel_done && s_sel_manual) s_sel_done = false;   // override cleared
    if (!pos.pos_valid) {
        if (!s_sel_done) s_sel = -1;
        return;
    }
    if (s_sel_done && dist_nm(pos.lat, pos.lon, s_sel_lat, s_sel_lon) < RESELECT_NM) return;

    int best = -1;
    double best_d = 1e9;
    for (int i = 0; i < s_tab.hdr->port_count; i++) {
        const PortRec &p = s_tab.port[i];
        if (!port_has_heights(p)) continue;
        double d = dist_nm(pos.lat, pos.lon, p.lat, p.lon);
        if (d < best_d) { best_d = d; best = i; }
    }
    int prev = s_sel;
    s_sel = (best >= 0 && best_d <= AUTO_MAX_NM) ? best : -1;
    s_sel_manual = false;
    s_sel_done = true;
    s_sel_gen = s_tab_gen;
    s_sel_lat = pos.lat;
    s_sel_lon = pos.lon;
    if (s_sel != prev) {
        if (s_sel >= 0) ESP_LOGI(TAG, "Nearest tide port: %s (%.1f NM)", s_tab.port[s_sel].name, best_d);
        else            ESP_LOGI(TAG, "No tide port within %.0f NM", AUTO_MAX_NM);
    }
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

static void table_free(Table *t)
{
    free(t->buf);
    memset(t, 0, sizeof(*t));
}

static bool table_load(const char *path, Table *out, char *err, size_t err_len)
{
    memset(out, 0, sizeof(*out));
    struct stat st;
    if (stat(path, &st) != 0) { snprintf(err, err_len, "no tide table uploaded"); return false; }
    size_t sz = (size_t)st.st_size;
    if (sz < sizeof(FileHdr) || sz > 8 * 1024 * 1024) { snprintf(err, err_len, "bad file size"); return false; }
    uint8_t *buf = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!buf) { snprintf(err, err_len, "out of memory"); return false; }
    FILE *f = fopen(path, "rb");
    size_t got = 0;
    if (f) {
        while (got < sz) {
            size_t r = fread(buf + got, 1, sz - got > 16384 ? 16384 : sz - got, f);
            if (r == 0) break;
            got += r;
        }
        fclose(f);
    }
    if (got != sz) { free(buf); snprintf(err, err_len, "read failed"); return false; }

    const FileHdr *h = (const FileHdr *)buf;
    size_t ns = h->std_count, np = h->port_count, ne = h->event_count;
    size_t need = sizeof(FileHdr) + ns * sizeof(StdRec) + np * sizeof(PortRec) + ne * 6;
    if (memcmp(h->magic, "NZT1", 4) != 0 || h->version != 1) {
        free(buf); snprintf(err, err_len, "not a tide table (NZT1 v1)"); return false;
    }
    if (need != sz || ns == 0 || np < ns) {
        free(buf); snprintf(err, err_len, "corrupt tide table"); return false;
    }
    out->buf = buf;
    out->size = sz;
    out->hdr = h;
    out->std = (const StdRec *)(buf + sizeof(FileHdr));
    out->port = (const PortRec *)(buf + sizeof(FileHdr) + ns * sizeof(StdRec));
    out->times = (const uint32_t *)((const uint8_t *)out->port + np * sizeof(PortRec));
    out->heights = (const int16_t *)((const uint8_t *)out->times + ne * 4);
    for (size_t i = 0; i < ns; i++) {
        if ((size_t)out->std[i].first + out->std[i].count > ne) {
            table_free(out); snprintf(err, err_len, "corrupt event index"); return false;
        }
    }
    for (size_t i = 0; i < np; i++) {
        if (out->port[i].std_index >= ns) {
            table_free(out); snprintf(err, err_len, "corrupt port table"); return false;
        }
    }
    return true;
}

static void table_swap(Table *t)
{
    Table old;
    LOCK();
    old = s_tab;
    s_tab = *t;
    s_tab_gen++;
    s_sel_done = false;
    s_load_err[0] = '\0';
    UNLOCK();
    table_free(&old);
    memset(t, 0, sizeof(*t));
}

static void load_task(void *)
{
    Table t;
    char err[64];
    int64_t t0 = esp_timer_get_time();
    if (table_load(TIDE_FILE, &t, err, sizeof(err))) {
        ESP_LOGI(TAG, "Tide table loaded: %u ports, %u events, %u KB in %d ms",
                 t.hdr->port_count, (unsigned)t.hdr->event_count, (unsigned)(t.size / 1024),
                 (int)((esp_timer_get_time() - t0) / 1000));
        table_swap(&t);
    } else {
        ESP_LOGI(TAG, "Tide table: %s", err);
        LOCK();
        snprintf(s_load_err, sizeof(s_load_err), "%s", err);
        UNLOCK();
    }
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Settings (NVS)
// ---------------------------------------------------------------------------

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_set.port);
    if (nvs_get_str(h, "port", s_set.port, &len) != ESP_OK) s_set.port[0] = '\0';
    int32_t mm;
    if (nvs_get_i32(h, "offs_mm", &mm) == ESP_OK) { s_set.offset_set = true; s_set.offset_m = mm / 1000.0f; }
    uint8_t b;
    if (nvs_get_u8(h, "baro", &b) == ESP_OK) s_set.baro = b != 0;
    nvs_close(h);
}

static void settings_save(const Settings &s)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "port", s.port);
    if (s.offset_set) nvs_set_i32(h, "offs_mm", (int32_t)lroundf(s.offset_m * 1000.0f));
    else              nvs_erase_key(h, "offs_mm");
    nvs_set_u8(h, "baro", s.baro ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// Moon (low precision lunar ephemeris, ~0.3 deg / a few minutes in rise times)
// ---------------------------------------------------------------------------

static double sind(double d) { return sin(d * D2R); }
static double cosd(double d) { return cos(d * D2R); }

static void moon_ecliptic(double t_unix, double *lam, double *bet, double *par)
{
    double T = (t_unix / 86400.0 - 10957.5) / 36525.0;
    double l = 218.32 + 481267.881 * T
             + 6.29 * sind(135.0 + 477198.87 * T) - 1.27 * sind(259.3 - 413335.36 * T)
             + 0.66 * sind(235.7 + 890534.22 * T) + 0.21 * sind(269.9 + 954397.74 * T)
             - 0.19 * sind(357.5 + 35999.05 * T) - 0.11 * sind(186.5 + 966404.03 * T);
    double b = 5.13 * sind(93.3 + 483202.02 * T) + 0.28 * sind(228.2 + 960400.89 * T)
             - 0.28 * sind(318.3 + 6003.15 * T) - 0.17 * sind(217.6 - 407332.21 * T);
    double p = 0.9508 + 0.0518 * cosd(135.0 + 477198.87 * T) + 0.0095 * cosd(259.3 - 413335.36 * T)
             + 0.0078 * cosd(235.7 + 890534.22 * T) + 0.0028 * cosd(269.9 + 954397.74 * T);
    *lam = fmod(fmod(l, 360.0) + 360.0, 360.0);
    *bet = b;
    *par = p;
}

static double sun_longitude(double t_unix)
{
    double n = t_unix / 86400.0 - 10957.5;
    double L = fmod(280.460 + 0.9856474 * n, 360.0);
    double g = fmod(357.528 + 0.9856003 * n, 360.0);
    return L + 1.915 * sind(g) + 0.020 * sind(2 * g);
}

// Altitude above the rise/set threshold (deg) and hour angle (rad, -pi..pi); optional declination (rad)
static void moon_horizon(double lat, double lon, double t_unix, double *alt_rel, double *ha, double *dec_out = nullptr)
{
    double lam, bet, par;
    moon_ecliptic(t_unix, &lam, &bet, &par);
    double cb = cosd(bet), sb = sind(bet), cl = cosd(lam), sl = sind(lam);
    double x = cb * cl;
    double y = 0.9175 * cb * sl - 0.3978 * sb;
    double z = 0.3978 * cb * sl + 0.9175 * sb;
    double ra = atan2(y, x), dec = asin(z);
    double d = t_unix / 86400.0 - 10957.5;
    double gmst = fmod(280.46061837 + 360.98564736629 * d, 360.0);
    double h = (gmst + lon) * D2R - ra;
    h = fmod(h, 2 * M_PI);
    if (h > M_PI) h -= 2 * M_PI;
    if (h < -M_PI) h += 2 * M_PI;
    double alt = asin(sind(lat) * sin(dec) + cosd(lat) * cos(dec) * cos(h)) / D2R;
    *alt_rel = alt - (0.7275 * par - 0.5667);
    *ha = h;
    if (dec_out) *dec_out = dec;
}

float moon_altitude_at(double lat, double lon, time_t t)
{
    double a, h;
    moon_horizon(lat, lon, (double)t, &a, &h);
    return (float)a;
}

float moon_azimuth_at(double lat, double lon, time_t t)
{
    double a, h, dec;
    moon_horizon(lat, lon, (double)t, &a, &h, &dec);
    double az = atan2(sin(h), cos(h) * sind(lat) - tan(dec) * cosd(lat)) / D2R + 180.0;
    return (float)fmod(az + 360.0, 360.0);
}

static const char *phase_name(double ph)
{
    if (ph < 0.0339 || ph >= 0.9661) return "New moon";
    if (ph < 0.216) return "Waxing crescent";
    if (ph < 0.284) return "First quarter";
    if (ph < 0.466) return "Waxing gibbous";
    if (ph < 0.534) return "Full moon";
    if (ph < 0.716) return "Waning gibbous";
    if (ph < 0.784) return "Last quarter";
    return "Waning crescent";
}

static int cmp_period(const void *a, const void *b)
{
    time_t x = ((const SolunarPeriod *)a)->start, y = ((const SolunarPeriod *)b)->start;
    return x < y ? -1 : (x > y ? 1 : 0);
}

// Rise, set, transit and underfoot from 12 h before day_start to the end of tomorrow, plus solunar periods
static void moon_events(double lat, double lon, time_t day_start, MoonInfo *m)
{
    const int STEP = 600;
    m->nev = 0;
    day_start -= 12 * 3600;
    double pa, ph;
    moon_horizon(lat, lon, (double)day_start, &pa, &ph);
    for (int s = STEP; s <= 60 * 3600 && m->nev < 16; s += STEP) {
        double t1 = (double)(day_start + s), t0 = t1 - STEP;
        double a, h;
        moon_horizon(lat, lon, t1, &a, &h);
        // Rise / set: altitude crosses the threshold
        if ((pa < 0) != (a < 0)) {
            double lo = t0, hi = t1, flo = pa;
            for (int k = 0; k < 12; k++) {
                double mid = (lo + hi) / 2, fm, hm;
                moon_horizon(lat, lon, mid, &fm, &hm);
                if ((flo < 0) == (fm < 0)) { lo = mid; flo = fm; } else { hi = mid; }
            }
            m->ev[m->nev++] = { (time_t)llround((lo + hi) / 2), (uint8_t)(pa < 0 ? MOON_RISE : MOON_SET) };
        }
        // Transit (hour angle - to +) and underfoot (+pi to -pi)
        bool transit = ph < 0 && h >= 0 && fabs(h - ph) < M_PI;
        bool under = ph > 0 && h < 0 && fabs(h - ph) > M_PI;
        if ((transit || under) && m->nev < 16) {
            double lo = t0, hi = t1;
            for (int k = 0; k < 12; k++) {
                double mid = (lo + hi) / 2, am, hm;
                moon_horizon(lat, lon, mid, &am, &hm);
                bool before = transit ? hm < 0 : hm > 0;
                if (before) lo = mid; else hi = mid;
            }
            m->ev[m->nev++] = { (time_t)llround((lo + hi) / 2), (uint8_t)(transit ? MOON_TRANSIT : MOON_UNDERFOOT) };
        }
        pa = a;
        ph = h;
    }
    m->nper = 0;
    for (int i = 0; i < m->nev && m->nper < 16; i++) {
        bool major = m->ev[i].type == MOON_TRANSIT || m->ev[i].type == MOON_UNDERFOOT;
        time_t half = major ? 3600 : 1800;
        m->per[m->nper++] = { m->ev[i].t - half, m->ev[i].t + half, m->ev[i].t, major };
    }
    qsort(m->per, m->nper, sizeof(m->per[0]), cmp_period);
}

static void moon_update(const DayNightStatus &pos, time_t now)
{
    static time_t s_day = 0;
    static double s_lat = 999, s_lon = 999;
    MoonInfo m;
    LOCK();
    m = s_moon;
    UNLOCK();

    m.valid = clock_valid(now);
    if (!m.valid) {
        m.located = false;
    } else {
        double lam, bet, par;
        moon_ecliptic((double)now, &lam, &bet, &par);
        double el = lam - sun_longitude((double)now);
        m.phase = (float)(fmod(fmod(el, 360.0) + 360.0, 360.0) / 360.0);
        m.illum = (float)((1.0 - cosd(bet) * cosd(el)) / 2.0);
        m.phase_name = phase_name(m.phase);

        struct tm lt;
        localtime_r(&now, &lt);
        lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
        lt.tm_isdst = -1;
        time_t day = mktime(&lt);
        if (pos.pos_valid && (day != s_day || fabs(pos.lat - s_lat) > 0.1 || fabs(pos.lon - s_lon) > 0.1 || !m.located)) {
            s_day = day;
            s_lat = pos.lat;
            s_lon = pos.lon;
            m.day_start = day;
            moon_events(pos.lat, pos.lon, day, &m);
            m.located = true;
        } else if (!pos.pos_valid) {
            m.located = false;
        }
        if (pos.pos_valid) {
            m.lat = (float)pos.lat;
            m.lon = (float)pos.lon;
        }
    }
    LOCK();
    s_moon = m;
    UNLOCK();
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

void tides_init(void)
{
    if (s_mtx) return;
    s_mtx = xSemaphoreCreateMutex();
    settings_load();
    s_info.distance_m = -1;
    xTaskCreatePinnedToCore(load_task, "tides_load", 4096, nullptr, 2, nullptr, tskNO_AFFINITY);
}

void tides_tick(void)
{
    static int64_t s_last_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (!s_mtx || (s_last_us && now_us - s_last_us < 1000000)) return;
    s_last_us = now_us;

    DayNightStatus pos;
    day_night_get_status(&pos);
    time_t now = time(nullptr);
    moon_update(pos, now);

    LOCK();
    bool loaded = s_tab.buf != nullptr;
    UNLOCK();
    if (!loaded) return;

    if (!s_subscribed) {
        s_subscribed = true;
        signalk_client_subscribe(P_SURFACE, 1000);
        signalk_client_subscribe(P_TRANSD, 1000);
        signalk_client_subscribe(P_S2T, 5000);
        signalk_client_subscribe(P_PRESS, 5000);
    }
    const char *paths[4] = { P_SURFACE, P_TRANSD, P_S2T, P_PRESS };
    PathValue v[4];
    if (!signalk_client_get_values(paths, 4, v)) {
        for (auto &x : v) x = PathValue();
    }
    uint32_t ms = now_ms();
    auto fresh = [&](int i, uint32_t max_age) { return v[i].valid && ms - v[i].last_update_ms < max_age; };

    TideInfo in = {};
    in.loaded = true;
    in.distance_m = -1;

    LOCK();
    Settings set = s_set;
    select_port(pos);
    if (s_sel >= 0) {
        const PortRec &p = s_tab.port[s_sel];
        snprintf(in.port, sizeof(in.port), "%.31s", p.name);
        in.manual = s_sel_manual;
        in.heights = port_has_heights(p);
        if (pos.pos_valid) in.distance_m = (float)(dist_nm(pos.lat, pos.lon, p.lat, p.lon) * 1852.0);
        if (set.baro && fresh(3, BARO_MAX_AGE_MS) && v[3].value > 85000 && v[3].value < 110000) {
            in.baro = true;
            in.baro_m = -(v[3].value / 100.0f - 1013.25f) * 0.01f;
        }
        int k = pos.time_valid ? ev_find(s_tab, s_sel, now) : -1;
        TideEvent a, b, c;
        if (k >= 0 && ev_at(s_tab, s_sel, k, in.baro_m, &a) && ev_at(s_tab, s_sel, k + 1, in.baro_m, &b)) {
            in.valid = true;
            in.prev = a;
            in.next = b;
            in.range = fabsf(b.h - a.h);
            height_at(s_tab, s_sel, now, in.baro_m, &in.height, &in.rate_mh);
            bool has_c = ev_at(s_tab, s_sel, k + 2, in.baro_m, &c);
            if (b.high) { in.next_high = b; if (has_c) in.next_low = c; }
            else        { in.next_low = b;  if (has_c) in.next_high = c; }
        }
    }
    UNLOCK();

    if (fresh(0, DEPTH_MAX_AGE_MS)) {
        in.depth_src = TIDE_DEPTH_SURFACE;
        in.depth = v[0].value;
    } else if (fresh(1, DEPTH_MAX_AGE_MS) && v[2].valid) {
        in.depth_src = TIDE_DEPTH_SK_OFFSET;
        in.depth = v[1].value + v[2].value;
    } else if (fresh(1, DEPTH_MAX_AGE_MS) && set.offset_set) {
        in.depth_src = TIDE_DEPTH_MANUAL;
        in.depth = v[1].value + set.offset_m;
    }
    if (in.valid && in.heights && in.depth_src) {
        if (in.next_high.t) in.depth_high = in.depth + (in.next_high.h - in.height);
        if (in.next_low.t)  in.depth_low  = in.depth + (in.next_low.h - in.height);
    }

    LOCK();
    s_info = in;
    UNLOCK();
}

bool tides_get_info(TideInfo *out)
{
    if (!s_mtx) { memset(out, 0, sizeof(*out)); return false; }
    LOCK();
    *out = s_info;
    UNLOCK();
    return out->valid;
}

bool moon_get(MoonInfo *out)
{
    if (!s_mtx) { memset(out, 0, sizeof(*out)); return false; }
    LOCK();
    *out = s_moon;
    UNLOCK();
    return out->valid;
}

int tides_events(time_t from, time_t to, TideEvent *out, int max)
{
    if (!s_mtx) return 0;
    int n = 0;
    LOCK();
    float baro = s_info.baro_m;
    if (s_tab.buf && s_sel >= 0) {
        int k = ev_find(s_tab, s_sel, from);
        if (k < 0) k = 0;
        TideEvent e;
        for (; n < max && ev_at(s_tab, s_sel, k, baro, &e) && e.t <= to; k++) {
            if (e.t >= from) out[n++] = e;
        }
    }
    UNLOCK();
    return n;
}

bool tides_curve(time_t from, time_t to, int n, float *out)
{
    if (!s_mtx || n < 2) return false;
    bool any = false;
    LOCK();
    float baro = s_info.baro_m;
    bool ok = s_tab.buf && s_sel >= 0 && port_has_heights(s_tab.port[s_sel]);
    for (int i = 0; i < n; i++) {
        time_t t = from + (time_t)((double)(to - from) * i / (n - 1));
        if (ok && height_at(s_tab, s_sel, t, baro, &out[i], nullptr)) any = true;
        else out[i] = NAN;
    }
    UNLOCK();
    return any;
}

bool tides_virtual_value(const char *path, float *out)
{
    if (!s_mtx || !path || strncmp(path, "panel.", 6) != 0) return false;
    const char *p = path + 6;
    time_t now = time(nullptr);

    if (strncmp(p, "moon.", 5) == 0 || strncmp(p, "solunar", 7) == 0) {
        MoonInfo m;
        if (!moon_get(&m)) return false;
        if (strcmp(p, "moon.illumination") == 0 || strcmp(p, "solunar") == 0) { *out = m.illum; return true; }
        if (strcmp(p, "moon.phase") == 0) { *out = m.phase; return true; }
        if (!m.located) return false;
        if (strcmp(p, "moon.rise") == 0 || strcmp(p, "moon.set") == 0) {
            uint8_t type = strcmp(p, "moon.rise") == 0 ? MOON_RISE : MOON_SET;
            for (int i = 0; i < m.nev; i++) {
                if (m.ev[i].type == type && m.ev[i].t > now) { *out = (float)local_tod(m.ev[i].t); return true; }
            }
            return false;
        }
        if (strcmp(p, "solunar.major.next") == 0 || strcmp(p, "solunar.minor.next") == 0) {
            bool major = strcmp(p, "solunar.major.next") == 0;
            for (int i = 0; i < m.nper; i++) {
                if (m.per[i].major == major && m.per[i].end > now) { *out = (float)local_tod(m.per[i].centre); return true; }
            }
            return false;
        }
        return false;
    }

    if (strncmp(p, "tide.", 5) != 0) return false;
    p += 5;
    TideInfo in;
    tides_get_info(&in);
    if (strcmp(p, "port.distance") == 0) {
        if (in.distance_m < 0 || !in.port[0]) return false;
        *out = in.distance_m;
        return true;
    }
    if (!in.valid) return false;
    if (strcmp(p, "height") == 0 || strcmp(p, "curve") == 0) { if (!in.heights) return false; *out = in.height; return true; }
    if (strcmp(p, "trend") == 0) { if (!in.heights) return false; *out = in.rate_mh; return true; }
    if (strcmp(p, "range") == 0) { if (!in.heights) return false; *out = in.range; return true; }
    const TideEvent *e = nullptr;
    float depth = 0;
    if (strncmp(p, "next.high.", 10) == 0)     { e = &in.next_high; depth = in.depth_high; p += 10; }
    else if (strncmp(p, "next.low.", 9) == 0)  { e = &in.next_low;  depth = in.depth_low;  p += 9; }
    if (!e || !e->t) return false;
    if (strcmp(p, "time") == 0) { *out = (float)local_tod(e->t); return true; }
    if (strcmp(p, "in") == 0)   { *out = (float)(e->t - now > 0 ? e->t - now : 0); return true; }
    if (strcmp(p, "height") == 0) { if (!in.heights) return false; *out = e->h; return true; }
    if (strcmp(p, "depth") == 0)  { if (!in.heights || !in.depth_src) return false; *out = depth; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Web editor
// ---------------------------------------------------------------------------

static void add_time(cJSON *o, const char *key, time_t t, const char *fmt = "%a %d %b %H:%M")
{
    char buf[32];
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(buf, sizeof(buf), fmt, &lt);
    cJSON_AddStringToObject(o, key, buf);
}

static void add_event(cJSON *arr, const TideEvent &e, bool heights, float depth, bool depth_ok, time_t now)
{
    if (!e.t) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", e.high ? "high" : "low");
    add_time(o, "time", e.t);
    cJSON_AddNumberToObject(o, "inS", (double)(e.t - now));
    if (heights) cJSON_AddNumberToObject(o, "height", roundf(e.h * 100) / 100);
    if (heights && depth_ok) cJSON_AddNumberToObject(o, "depth", roundf(depth * 100) / 100);
    cJSON_AddItemToArray(arr, o);
}

cJSON *tides_status_json(bool with_ports)
{
    cJSON *root = cJSON_CreateObject();
    if (!s_mtx) return root;
    TideInfo in;
    MoonInfo m;
    tides_get_info(&in);
    moon_get(&m);
    time_t now = time(nullptr);

    LOCK();
    cJSON_AddBoolToObject(root, "loaded", s_tab.buf != nullptr);
    if (!s_tab.buf) cJSON_AddStringToObject(root, "error", s_load_err);
    if (s_tab.buf) {
        cJSON *f = cJSON_AddObjectToObject(root, "file");
        cJSON_AddNumberToObject(f, "bytes", (double)s_tab.size);
        cJSON_AddNumberToObject(f, "ports", s_tab.hdr->port_count);
        cJSON_AddNumberToObject(f, "standardPorts", s_tab.hdr->std_count);
        cJSON_AddNumberToObject(f, "events", s_tab.hdr->event_count);
        add_time(f, "from", EPOCH_2020 + (time_t)s_tab.hdr->first_min * 60, "%d %b %Y");
        add_time(f, "to", EPOCH_2020 + (time_t)s_tab.hdr->last_min * 60, "%d %b %Y");
        add_time(f, "built", (time_t)s_tab.hdr->build_unix, "%d %b %Y");
    }
    cJSON *js = cJSON_AddObjectToObject(root, "settings");
    cJSON_AddStringToObject(js, "port", s_set.port);
    if (s_set.offset_set) cJSON_AddNumberToObject(js, "offset", s_set.offset_m);
    else                  cJSON_AddNullToObject(js, "offset");
    cJSON_AddBoolToObject(js, "baro", s_set.baro);
    if (with_ports && s_tab.buf) {
        cJSON *arr = cJSON_AddArrayToObject(root, "ports");
        for (int i = 0; i < s_tab.hdr->port_count; i++) {
            char name[33];
            snprintf(name, sizeof(name), "%.32s", s_tab.port[i].name);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", name);
            if (!port_has_heights(s_tab.port[i])) cJSON_AddBoolToObject(o, "timesOnly", true);
            cJSON_AddItemToArray(arr, o);
        }
    }
    UNLOCK();

    cJSON *ji = cJSON_AddObjectToObject(root, "info");
    if (in.port[0]) {
        cJSON_AddStringToObject(ji, "port", in.port);
        cJSON_AddBoolToObject(ji, "manual", in.manual);
        cJSON_AddBoolToObject(ji, "heights", in.heights);
        if (in.distance_m >= 0) cJSON_AddNumberToObject(ji, "distanceNm", roundf(in.distance_m / 185.2f) / 10);
    }
    cJSON_AddBoolToObject(ji, "valid", in.valid);
    if (in.valid) {
        if (in.heights) {
            cJSON_AddNumberToObject(ji, "height", roundf(in.height * 100) / 100);
            cJSON_AddNumberToObject(ji, "rate", roundf(in.rate_mh * 100) / 100);
            cJSON_AddNumberToObject(ji, "range", roundf(in.range * 100) / 100);
        }
        if (in.baro) cJSON_AddNumberToObject(ji, "baroCorrection", roundf(in.baro_m * 100) / 100);
        static const char *src[] = { "none", "belowSurface", "belowTransducer + surfaceToTransducer",
                                     "belowTransducer + manual offset" };
        cJSON_AddStringToObject(ji, "depthSource", src[in.depth_src & 3]);
        if (in.depth_src) cJSON_AddNumberToObject(ji, "depth", roundf(in.depth * 100) / 100);
        cJSON *ev = cJSON_AddArrayToObject(ji, "next");
        bool hf = in.next_high.t && (!in.next_low.t || in.next_high.t < in.next_low.t);
        const TideEvent &e1 = hf ? in.next_high : in.next_low;
        const TideEvent &e2 = hf ? in.next_low : in.next_high;
        add_event(ev, e1, in.heights, hf ? in.depth_high : in.depth_low, in.depth_src != 0, now);
        add_event(ev, e2, in.heights, hf ? in.depth_low : in.depth_high, in.depth_src != 0, now);
    }

    cJSON *jm = cJSON_AddObjectToObject(root, "moon");
    if (m.valid) {
        cJSON_AddStringToObject(jm, "phase", m.phase_name);
        cJSON_AddNumberToObject(jm, "illumination", roundf(m.illum * 100));
        if (m.located) {
            cJSON *ev = cJSON_AddArrayToObject(jm, "periods");
            for (int i = 0; i < m.nper; i++) {
                if (m.per[i].end < now || m.per[i].start > now + 86400) continue;
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "type", m.per[i].major ? "major" : "minor");
                add_time(o, "start", m.per[i].start);
                add_time(o, "end", m.per[i].end);
                cJSON_AddItemToArray(ev, o);
            }
            for (int i = 0; i < m.nev; i++) {
                if (m.ev[i].t < now) continue;
                if (m.ev[i].type == MOON_RISE && !cJSON_GetObjectItem(jm, "rise")) add_time(jm, "rise", m.ev[i].t);
                if (m.ev[i].type == MOON_SET && !cJSON_GetObjectItem(jm, "set")) add_time(jm, "set", m.ev[i].t);
            }
        }
    }
    return root;
}

const char *tides_upload_path(void) { return TIDE_TMP; }

size_t tides_upload_max(void)
{
    size_t total = 0, used = 0;
    if (esp_littlefs_info("spiffs", &total, &used) != ESP_OK || used >= total) return 0;
    size_t free_b = total - used;
    struct stat st;
    if (stat(TIDE_TMP, &st) == 0) free_b += (size_t)st.st_size;
    return free_b > 64 * 1024 ? free_b - 64 * 1024 : 0;
}

bool tides_install_upload(char *err, size_t err_len)
{
    Table t;
    if (!table_load(TIDE_TMP, &t, err, err_len)) {
        remove(TIDE_TMP);
        return false;
    }
    remove(TIDE_FILE);
    if (rename(TIDE_TMP, TIDE_FILE) != 0) {
        table_free(&t);
        remove(TIDE_TMP);
        snprintf(err, err_len, "rename failed");
        return false;
    }
    ESP_LOGI(TAG, "New tide table installed: %u ports, %u events",
             t.hdr->port_count, (unsigned)t.hdr->event_count);
    table_swap(&t);
    return true;
}

bool tides_apply_settings(const cJSON *body, char *err, size_t err_len)
{
    if (!s_mtx) { snprintf(err, err_len, "not ready"); return false; }
    LOCK();
    Settings s = s_set;
    const cJSON *j = cJSON_GetObjectItem(body, "port");
    if (cJSON_IsString(j)) {
        if (j->valuestring[0] && s_tab.buf && port_by_name(s_tab, j->valuestring) < 0) {
            UNLOCK();
            snprintf(err, err_len, "unknown port");
            return false;
        }
        snprintf(s.port, sizeof(s.port), "%s", j->valuestring);
    }
    j = cJSON_GetObjectItem(body, "offset");
    if (cJSON_IsNull(j)) {
        s.offset_set = false;
    } else if (cJSON_IsNumber(j)) {
        if (j->valuedouble < -2 || j->valuedouble > 10) {
            UNLOCK();
            snprintf(err, err_len, "offset out of range");
            return false;
        }
        s.offset_set = true;
        s.offset_m = (float)j->valuedouble;
    }
    j = cJSON_GetObjectItem(body, "baro");
    if (cJSON_IsBool(j)) s.baro = cJSON_IsTrue(j);
    s_set = s;
    s_sel_done = false;
    UNLOCK();
    settings_save(s);
    ESP_LOGI(TAG, "Tide settings: port '%s', offset %s%.2f, baro %d", s.port,
             s.offset_set ? "" : "(unset) ", s.offset_m, s.baro);
    return true;
}
