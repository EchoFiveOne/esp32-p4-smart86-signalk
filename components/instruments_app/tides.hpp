#pragma once

// NZ tide predictions from the LINZ tables (packed by tools/linz_tides_pack.py
// into /dash/tides.bin) plus the moon / solunar calculator.
//
// tides_init/tides_tick run on the LVGL thread; every other function is
// thread-safe (web server, catalog task).

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

struct cJSON;

struct TideEvent {
    time_t t;
    float  h;       // m above chart datum (barometric correction applied)
    bool   high;
};

enum TideDepthSource : uint8_t {
    TIDE_DEPTH_NONE = 0,
    TIDE_DEPTH_SURFACE,       // environment.depth.belowSurface
    TIDE_DEPTH_SK_OFFSET,     // belowTransducer + environment.depth.surfaceToTransducer
    TIDE_DEPTH_MANUAL,        // belowTransducer + offset set in the web editor
};

struct TideInfo {
    bool      loaded;         // tide table present
    bool      valid;          // port selected, clock valid, inside the table
    bool      heights;        // heights known for this port (false = times only)
    bool      manual;         // port chosen by the user
    char      port[32];
    float     distance_m;     // < 0 = unknown
    float     height;         // now, m above chart datum
    float     rate_mh;        // m per hour, + rising
    float     range;          // between the previous and next event
    float     baro_m;         // correction applied (m)
    bool      baro;           // pressure available and correction on
    uint8_t   depth_src;      // TideDepthSource
    float     depth;          // m below surface now
    TideEvent prev, next, next_high, next_low;
    float     depth_high, depth_low;  // projected depth below surface at those events
};

void tides_init(void);
void tides_tick(void);

// panel.tide.* and panel.moon.* / panel.solunar.* virtual values
bool tides_virtual_value(const char *path, float *out);

bool tides_get_info(TideInfo *out);
// Events of the selected port with from <= t <= to (sorted). Returns count.
int  tides_events(time_t from, time_t to, TideEvent *out, int max);
// n evenly spaced heights over [from, to]; NAN where outside the table.
bool tides_curve(time_t from, time_t to, int n, float *out);

// Web editor
cJSON      *tides_status_json(bool with_ports);
const char *tides_upload_path(void);           // temporary file for uploads
size_t      tides_upload_max(void);            // bytes that may be uploaded now
bool        tides_install_upload(char *err, size_t err_len);
// JSON body: {"port": "" | name, "offset": null | metres, "baro": bool}
bool        tides_apply_settings(const cJSON *body, char *err, size_t err_len);

// --- Moon / solunar ---------------------------------------------------------
enum MoonEventType : uint8_t { MOON_RISE = 0, MOON_SET, MOON_TRANSIT, MOON_UNDERFOOT };

struct MoonEvent { time_t t; uint8_t type; };

struct SolunarPeriod { time_t start, end, centre; bool major; };

struct MoonInfo {
    bool          valid;      // clock valid (phase / illumination)
    bool          located;    // position known (events / periods)
    float         illum;      // 0..1
    float         phase;      // 0 new, 0.25 first quarter, 0.5 full, 0.75 last quarter
    const char   *phase_name;
    float         lat;        // last known latitude (lit limb is mirrored south of the equator)
    float         lon;
    time_t        day_start;  // local midnight today
    int           nev;
    MoonEvent     ev[16];     // from 12 h before today through tomorrow, sorted
    int           nper;
    SolunarPeriod per[16];    // sorted by start
};

bool moon_get(MoonInfo *out);
// Altitude above the rise/set horizon and true azimuth (deg) at a given time
float moon_altitude_at(double lat, double lon, time_t t);
float moon_azimuth_at(double lat, double lon, time_t t);
