#include "dash_model.hpp"
#include "unit_config.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>

#ifndef SIMULATOR_BUILD
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_littlefs.h"
#endif

static const char *TAG = "dash_cfg";

// ---------------------------------------------------------------------------
// Layouts (DASH_GRID x DASH_GRID unit grid). In the "large + small" layouts the
// small tiles are square and the large tile(s) on top take the remaining height.
// ---------------------------------------------------------------------------

struct LayoutDef {
    const char *id;
    const char *name;
    int         count;
    DashCell    cells[DASH_MAX_TILES];
};

static const LayoutDef s_layouts[DASH_LAYOUT_COUNT] = {
    { "1",   "Single",                1, { {0,0,12,12} } },
    { "2r",  "2 rows",                2, { {0,0,12,6}, {0,6,12,6} } },
    { "2c",  "2 columns",             2, { {0,0,6,12}, {6,0,6,12} } },
    { "3r",  "3 rows",                3, { {0,0,12,4}, {0,4,12,4}, {0,8,12,4} } },
    { "1+2", "1 large + 2",           3, { {0,0,12,6}, {0,6,6,6}, {6,6,6,6} } },
    { "2x2", "2 x 2 grid",            4, { {0,0,6,6}, {6,0,6,6}, {0,6,6,6}, {6,6,6,6} } },
    { "2x3", "2 columns x 3 rows",    6, { {0,0,6,4}, {6,0,6,4}, {0,4,6,4}, {6,4,6,4},
                                           {0,8,6,4}, {6,8,6,4} } },
    { "3x2", "3 columns x 2 rows",    6, { {0,0,4,6}, {4,0,4,6}, {8,0,4,6},
                                           {0,6,4,6}, {4,6,4,6}, {8,6,4,6} } },
    { "3x3", "3 x 3 grid",            9, { {0,0,4,4}, {4,0,4,4}, {8,0,4,4},
                                           {0,4,4,4}, {4,4,4,4}, {8,4,4,4},
                                           {0,8,4,4}, {4,8,4,4}, {8,8,4,4} } },
    { "1+3", "1 large + 3",           4, { {0,0,12,8}, {0,8,4,4}, {4,8,4,4}, {8,8,4,4} } },
    { "1+4", "1 large + 4",           5, { {0,0,12,9}, {0,9,3,3}, {3,9,3,3}, {6,9,3,3}, {9,9,3,3} } },
    { "2+3", "2 large + 3",           5, { {0,0,6,8}, {6,0,6,8}, {0,8,4,4}, {4,8,4,4}, {8,8,4,4} } },
    { "2+4", "2 large + 4",           6, { {0,0,6,9}, {6,0,6,9},
                                           {0,9,3,3}, {3,9,3,3}, {6,9,3,3}, {9,9,3,3} } },
    { "4r",  "4 rows",                4, { {0,0,12,3}, {0,3,12,3}, {0,6,12,3}, {0,9,12,3} } },
    { "4x4", "4 x 4 grid",           16, { {0,0,3,3}, {3,0,3,3}, {6,0,3,3}, {9,0,3,3},
                                           {0,3,3,3}, {3,3,3,3}, {6,3,3,3}, {9,3,3,3},
                                           {0,6,3,3}, {3,6,3,3}, {6,6,3,3}, {9,6,3,3},
                                           {0,9,3,3}, {3,9,3,3}, {6,9,3,3}, {9,9,3,3} } },
};

static uint8_t clamp_layout(uint8_t l) { return l < DASH_LAYOUT_COUNT ? l : DASH_LAYOUT_2X2; }

int dash_layout_tile_count(uint8_t layout)        { return s_layouts[clamp_layout(layout)].count; }
const DashCell *dash_layout_cells(uint8_t layout) { return s_layouts[clamp_layout(layout)].cells; }
const char *dash_layout_id(uint8_t layout)        { return s_layouts[clamp_layout(layout)].id; }
const char *dash_layout_name(uint8_t layout)      { return s_layouts[clamp_layout(layout)].name; }

static const char *s_group_ids[DASH_GROUP_COUNT] = { "", "anchor", "sail" };
const char *dash_group_id(uint8_t group) { return s_group_ids[group < DASH_GROUP_COUNT ? group : 0]; }

int dash_group_first_page(const DashConfig *cfg, uint8_t group)
{
    if (group == DASH_GROUP_NONE) return -1;
    for (int p = 0; p < cfg->page_count; p++) if (cfg->pages[p].group == group) return p;
    return -1;
}

int dash_layout_from_id(const char *id)
{
    if (!id) return -1;
    for (int i = 0; i < DASH_LAYOUT_COUNT; i++) {
        if (strcmp(s_layouts[i].id, id) == 0) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Units
// ---------------------------------------------------------------------------

struct UnitDef {
    const char *sk;       // Signal K unit
    const char *id;       // display unit id
    const char *label;    // label shown on screen
    float       factor;
    float       offset;
    int8_t      decimals; // automatic decimals
};

#define DEG "\xc2\xb0"

static const UnitDef s_units[] = {
    { "m/s",   "kn",    "kn",      1.943844f,     0, 1 },
    { "m/s",   "kmh",   "km/h",    3.6f,          0, 1 },
    { "m/s",   "mph",   "mph",     2.236936f,     0, 1 },
    { "m/s",   "ms",    "m/s",     1.0f,          0, 1 },
    { "m/s",   "bft",   "Bft",     0.0f,          0, 0 },   // Beaufort (special)
    { "m",     "m",     "m",       1.0f,          0, 1 },
    { "m",     "ft",    "ft",      3.28084f,      0, 1 },
    { "m",     "fm",    "fm",      0.546807f,     0, 1 },
    { "m",     "nm",    "NM",      1.0f / 1852.0f,     0, 2 },
    { "m",     "km",    "km",      0.001f,        0, 2 },
    { "m",     "mi",    "mi",      1.0f / 1609.344f,   0, 2 },
    { "rad",   "deg",   DEG,       57.2957795f,   0, 0 },
    { "rad/s", "dps",   DEG "/s",  57.2957795f,   0, 1 },
    { "rad/s", "dpm",   DEG "/min",3437.74677f,   0, 0 },
    { "K",     "C",     DEG "C",   1.0f,    -273.15f, 1 },
    { "K",     "F",     DEG "F",   1.8f,    -459.67f, 1 },
    { "K",     "K",     "K",       1.0f,          0, 1 },
    { "Pa",    "hPa",   "hPa",     0.01f,         0, 0 },
    { "Pa",    "mbar",  "mbar",    0.01f,         0, 0 },
    { "Pa",    "inHg",  "inHg",    0.00029530f,   0, 2 },
    { "Pa",    "kPa",   "kPa",     0.001f,        0, 0 },
    { "Pa",    "psi",   "psi",     0.000145038f,  0, 0 },
    { "Pa",    "bar",   "bar",     0.00001f,      0, 2 },
    { "ratio", "pct",   "%",       100.0f,        0, 0 },
    { "ratio", "ratio", "",        1.0f,          0, 2 },
    { "m3",    "L",     "L",       1000.0f,       0, 0 },
    { "m3",    "gal",   "gal",     264.172f,      0, 1 },
    { "m3",    "m3",    "m3",      1.0f,          0, 2 },
    { "m3/s",  "Lh",    "L/h",     3600000.0f,    0, 1 },
    { "m3/s",  "galh",  "gal/h",   951019.4f,     0, 1 },
    { "Hz",    "rpm",   "rpm",     60.0f,         0, 0 },
    { "Hz",    "Hz",    "Hz",      1.0f,          0, 1 },
    { "s",     "s",     "s",       1.0f,          0, 0 },
    { "s",     "min",   "min",     1.0f / 60.0f,  0, 0 },
    { "s",     "h",     "h",       1.0f / 3600.0f,0, 1 },
    { "C",     "Ah",    "Ah",      1.0f / 3600.0f,0, 0 },
    { "C",     "Cb",    "C",       1.0f,          0, 0 },
    { "J",     "Wh",    "Wh",      1.0f / 3600.0f,0, 0 },
    { "J",     "kWh",   "kWh",     1.0f / 3600000.0f, 0, 1 },
    { "J",     "J",     "J",       1.0f,          0, 0 },
    { "V",     "V",     "V",       1.0f,          0, 2 },
    { "A",     "A",     "A",       1.0f,          0, 1 },
    { "W",     "W",     "W",       1.0f,          0, 0 },
    { "W",     "kW",    "kW",      0.001f,        0, 2 },
    { "deg",   "deg",   DEG,       1.0f,          0, 0 },
    { "tod",   "hhmm",  "",        1.0f / 3600.0f,0, 2 },   // panel clock (seconds of day)
    { "date",  "ddmm",  "",        1.0f,          0, 0 },   // panel date (month*100+day)
    { "dur",   "hmm",   "",        1.0f / 3600.0f,0, 2 },   // countdown (seconds) shown as H:MM
    { "dur",   "h",     "h",       1.0f / 3600.0f,0, 1 },
    { "dur",   "min",   "min",     1.0f / 60.0f,  0, 0 },
    { "m/h",   "mh",    "m/h",     1.0f,          0, 2 },   // tide rate
    { "m/h",   "cmh",   "cm/h",    100.0f,        0, 0 },
    { "m/h",   "fth",   "ft/h",    3.28084f,      0, 1 },
};
static const int s_unit_count = sizeof(s_units) / sizeof(s_units[0]);

static const UnitDef *unit_by_id(const char *sk, const char *id)
{
    for (int i = 0; i < s_unit_count; i++) {
        if (strcmp(s_units[i].sk, sk) == 0 && strcmp(s_units[i].id, id) == 0) return &s_units[i];
    }
    return nullptr;
}

static const UnitDef *unit_first(const char *sk)
{
    for (int i = 0; i < s_unit_count; i++) {
        if (strcmp(s_units[i].sk, sk) == 0) return &s_units[i];
    }
    return nullptr;
}

int dash_unit_options(const char *sk_units, const DashUnitOption **out)
{
    static DashUnitOption opts[12];
    int n = 0;
    opts[n++] = { "", "Auto" };
    for (int i = 0; i < s_unit_count && n < 12; i++) {
        if (sk_units && strcmp(s_units[i].sk, sk_units) == 0) {
            opts[n++] = { s_units[i].id, s_units[i].label };
        }
    }
    *out = opts;
    return n;
}

static bool path_has(const char *path, const char *needle)
{
    return path && strstr(path, needle) != nullptr;
}

static const char *auto_unit_id(const char *sk, const char *path)
{
    if (strcmp(sk, "m/s") == 0) {
        static const char *ids[] = { "kn", "kmh", "mph", "ms" };
        int i = unit_config_get(UNIT_CAT_SPEED);
        return ids[(i >= 0 && i < 4) ? i : 0];
    }
    if (strcmp(sk, "m") == 0) {
        bool tide = path_has(path, "panel.tide") && !path_has(path, "distance");
        if (tide || path_has(path, "depth") || path_has(path, "Depth")) {
            static const char *ids[] = { "m", "ft", "fm" };
            int i = unit_config_get(UNIT_CAT_DEPTH);
            return ids[(i >= 0 && i < 3) ? i : 0];
        }
        if (path_has(path, "log") || path_has(path, "trip") ||
            path_has(path, "distance") || path_has(path, "Distance")) {
            static const char *ids[] = { "nm", "mi", "km" };
            int i = unit_config_get(UNIT_CAT_DISTANCE);
            return ids[(i >= 0 && i < 3) ? i : 0];
        }
        return "m";
    }
    if (strcmp(sk, "K") == 0) {
        return unit_config_get(UNIT_CAT_TEMPERATURE) == 1 ? "F" : "C";
    }
    if (strcmp(sk, "Pa") == 0) {
        if (path && strncmp(path, "environment", 11) == 0) {
            static const char *ids[] = { "hPa", "mbar", "inHg" };
            int i = unit_config_get(UNIT_CAT_PRESSURE);
            return ids[(i >= 0 && i < 3) ? i : 0];
        }
        return "kPa";
    }
    if (strcmp(sk, "m3") == 0)   return unit_config_get(UNIT_CAT_VOLUME) == 1 ? "gal" : "L";
    if (strcmp(sk, "m3/s") == 0) return unit_config_get(UNIT_CAT_VOLUME) == 1 ? "galh" : "Lh";
    if (strcmp(sk, "rad/s") == 0) return path_has(path, "rateOfTurn") ? "dpm" : "dps";
    if (strcmp(sk, "Hz") == 0)    return path_has(path, "revolutions") ? "rpm" : "Hz";
    if (strcmp(sk, "s") == 0) {
        if (path_has(path, "runTime")) return "h";
        if (path_has(path, "timeToGo")) return "min";
        return "s";
    }
    if (strcmp(sk, "C") == 0)     return "Ah";
    if (strcmp(sk, "m/h") == 0)   return unit_config_get(UNIT_CAT_DEPTH) == 1 ? "fth" : "mh";
    if (strcmp(sk, "J") == 0)     return "Wh";
    if (strcmp(sk, "ratio") == 0) return "pct";
    const UnitDef *u = unit_first(sk);
    return u ? u->id : "";
}

struct KnownPath { const char *path; const char *label; const char *units; };

static const KnownPath s_known[] = {
    { "navigation.speedOverGround",                     "SOG",    "m/s" },
    { "navigation.speedThroughWater",                   "STW",    "m/s" },
    { "navigation.courseOverGroundTrue",                "COG",    "rad" },
    { "navigation.courseOverGroundMagnetic",            "COG M",  "rad" },
    { "navigation.headingMagnetic",                     "HDG",    "rad" },
    { "navigation.headingTrue",                         "HDG T",  "rad" },
    { "navigation.magneticVariation",                   "VAR",    "rad" },
    { "navigation.rateOfTurn",                          "ROT",    "rad/s" },
    { "navigation.log",                                 "LOG",    "m" },
    { "navigation.trip.log",                            "TRIP",   "m" },
    { "navigation.leewayAngle",                         "LEEWAY", "rad" },
    { "navigation.courseRhumbline.crossTrackError",     "XTE",    "m" },
    { "navigation.courseGreatCircle.crossTrackError",   "XTE",    "m" },
    { "navigation.courseRhumbline.nextPoint.bearingTrue",   "BTW", "rad" },
    { "navigation.courseGreatCircle.nextPoint.bearingTrue", "BTW", "rad" },
    { "navigation.courseRhumbline.nextPoint.distance",      "DTW", "m" },
    { "navigation.courseGreatCircle.nextPoint.distance",    "DTW", "m" },
    { "navigation.courseRhumbline.nextPoint.velocityMadeGood",   "VMC", "m/s" },
    { "navigation.courseGreatCircle.nextPoint.velocityMadeGood", "VMC", "m/s" },
    { "navigation.courseRhumbline.nextPoint.timeToGo",      "TTG", "s" },
    { "navigation.courseGreatCircle.nextPoint.timeToGo",    "TTG", "s" },
    { "performance.velocityMadeGood",                   "VMG",    "m/s" },
    { "performance.polarSpeed",                         "POLAR",  "m/s" },
    { "performance.polarSpeedRatio",                    "POLAR %","ratio" },
    { "environment.wind.speedApparent",                 "AWS",    "m/s" },
    { "environment.wind.angleApparent",                 "AWA",    "rad" },
    { "environment.wind.speedTrue",                     "TWS",    "m/s" },
    { "environment.wind.angleTrueWater",                "TWA",    "rad" },
    { "environment.wind.angleTrueGround",               "TWA G",  "rad" },
    { "environment.wind.directionTrue",                 "TWD",    "rad" },
    { "environment.wind.directionMagnetic",             "TWD M",  "rad" },
    { "environment.wind.speedOverGround",               "GWS",    "m/s" },
    { "environment.depth.belowTransducer",              "DEPTH",  "m" },
    { "environment.depth.belowKeel",                    "DEPTH K","m" },
    { "environment.depth.belowSurface",                 "DEPTH S","m" },
    { "environment.water.temperature",                  "SEA TEMP","K" },
    { "environment.outside.temperature",                "AIR TEMP","K" },
    { "environment.inside.temperature",                 "CABIN",  "K" },
    { "environment.outside.pressure",                   "BARO",   "Pa" },
    { "environment.outside.relativeHumidity",           "HUMIDITY","ratio" },
    { "environment.outside.dewPointTemperature",        "DEW PT", "K" },
    { "steering.rudderAngle",                           "RUDDER", "rad" },
    { "steering.autopilot.target.headingMagnetic",      "AP HDG", "rad" },
    { "steering.autopilot.target.windAngleApparent",    "AP AWA", "rad" },
    // Panel virtual values (computed on the device, see day_night)
    { "panel.time.local",                               "TIME",    "tod" },
    { "panel.time.utc",                                 "UTC",     "tod" },
    { "panel.date.local",                               "DATE",    "date" },
    { "panel.sun",                                      "SUN",     "rad" },
    { "panel.sun.sunrise",                              "SUNRISE", "tod" },
    { "panel.sun.sunset",                               "SUNSET",  "tod" },
    { "panel.sun.sunrise.in",                           "SUNRISE IN","dur" },
    { "panel.sun.sunset.in",                            "SUNSET IN","dur" },
    { "panel.sun.noon",                                 "NOON",    "tod" },
    { "panel.sun.daylength",                            "DAYLIGHT","dur" },
    { "panel.sun.dawn.civil",                           "CIV DAWN","tod" },
    { "panel.sun.dusk.civil",                           "CIV DUSK","tod" },
    { "panel.sun.dawn.nautical",                        "NAUT DAWN","tod" },
    { "panel.sun.dusk.nautical",                        "NAUT DUSK","tod" },
    { "panel.sun.dawn.astronomical",                    "AST DAWN","tod" },
    { "panel.sun.dusk.astronomical",                    "AST DUSK","tod" },
    { "panel.sun.golden.morning",                       "GOLDEN AM","tod" },
    { "panel.sun.golden.evening",                       "GOLDEN PM","tod" },
    { "panel.sun.elevation",                            "SUN ELEV","rad" },
    { "panel.sun.azimuth",                              "SUN AZ",  "rad" },
    { "panel.tide.curve",                               "TIDE",    "m" },
    { "panel.tide.height",                              "TIDE HT", "m" },
    { "panel.tide.trend",                               "TIDE RATE","m/h" },
    { "panel.tide.range",                               "RANGE",   "m" },
    { "panel.tide.port.distance",                       "TIDE PORT","m" },
    { "panel.tide.next.high.time",                      "HW TIME", "tod" },
    { "panel.tide.next.high.in",                        "HW IN",   "dur" },
    { "panel.tide.next.high.height",                    "HW HT",   "m" },
    { "panel.tide.next.high.depth",                     "HW DPT", "m" },
    { "panel.tide.next.low.time",                       "LW TIME", "tod" },
    { "panel.tide.next.low.in",                         "LW IN",   "dur" },
    { "panel.tide.next.low.height",                     "LW HT",   "m" },
    { "panel.tide.next.low.depth",                      "LW DPT", "m" },
    { "panel.solunar",                                  "SOLUNAR", "ratio" },
    { "panel.wind.rose",                                "WIND",    "rad" },
    { "panel.moon.illumination",                        "MOON",    "ratio" },
    { "panel.moon.rise",                                "MOONRISE","tod" },
    { "panel.moon.set",                                 "MOONSET", "tod" },
    { "panel.solunar.major.next",                       "MAJOR",   "tod" },
    { "panel.solunar.minor.next",                       "MINOR",   "tod" },
};
static const int s_known_count = sizeof(s_known) / sizeof(s_known[0]);

static const KnownPath *known_find(const char *path)
{
    for (int i = 0; i < s_known_count; i++) {
        if (strcmp(s_known[i].path, path) == 0) return &s_known[i];
    }
    return nullptr;
}

const char *dash_friendly_label(const char *path)
{
    if (!path) return nullptr;
    const KnownPath *k = known_find(path);
    if (k) return k->label;

    // Pattern based names for multi-instance groups
    struct Suffix { const char *prefix; const char *suffix; const char *label; };
    static const Suffix s_suffix[] = {
        { "propulsion.",          ".revolutions",     "RPM" },
        { "propulsion.",          ".temperature",     "ENG TEMP" },
        { "propulsion.",          ".coolantTemperature","COOLANT" },
        { "propulsion.",          ".oilPressure",     "OIL" },
        { "propulsion.",          ".fuel.rate",       "FUEL RATE" },
        { "propulsion.",          ".runTime",         "ENG HRS" },
        { "propulsion.",          ".alternatorVoltage","ALT V" },
        { "electrical.batteries.", ".voltage",        "BATT V" },
        { "electrical.batteries.", ".current",        "BATT A" },
        { "electrical.batteries.", ".capacity.stateOfCharge", "SOC" },
        { "electrical.batteries.", ".capacity.timeRemaining", "TIME REM" },
        { "electrical.batteries.", ".temperature",    "BATT TEMP" },
        { "electrical.solar.",     ".panelPower",     "SOLAR W" },
        { "tanks.fuel.",           ".currentLevel",   "FUEL" },
        { "tanks.freshWater.",     ".currentLevel",   "WATER" },
        { "tanks.wasteWater.",     ".currentLevel",   "WASTE" },
        { "tanks.blackWater.",     ".currentLevel",   "BLACK" },
    };
    size_t plen = strlen(path);
    for (const auto &s : s_suffix) {
        size_t sl = strlen(s.suffix);
        if (strncmp(path, s.prefix, strlen(s.prefix)) == 0 && plen > sl &&
            strcmp(path + plen - sl, s.suffix) == 0) {
            return s.label;
        }
    }
    return nullptr;
}

const char *dash_guess_sk_units(const char *path)
{
    if (!path) return "";
    const KnownPath *k = known_find(path);
    if (k) return k->units;
    size_t plen = strlen(path);
    struct Tail { const char *tail; const char *units; };
    static const Tail s_tails[] = {
        { ".revolutions", "Hz" }, { ".temperature", "K" }, { "Temperature", "K" },
        { ".voltage", "V" }, { "Voltage", "V" }, { ".current", "A" },
        { ".power", "W" }, { "Power", "W" }, { "Pressure", "Pa" }, { ".pressure", "Pa" },
        { ".currentLevel", "ratio" }, { ".stateOfCharge", "ratio" }, { ".runTime", "s" },
        { ".rate", "m3/s" }, { ".timeRemaining", "s" },
    };
    for (const auto &t : s_tails) {
        size_t tl = strlen(t.tail);
        if (plen > tl && strcmp(path + plen - tl, t.tail) == 0) return t.units;
    }
    return "";
}

bool dash_is_virtual(const char *path)
{
    return path && strncmp(path, "panel.", 6) == 0;
}

bool dash_is_clock(const DashTile *t)
{
    return strcmp(t->sk_units, "tod") == 0 || strcmp(t->sk_units, "date") == 0 ||
           strcmp(t->sk_units, "dur") == 0;
}

bool dash_is_graphic(const char *path)
{
    return path && (strcmp(path, "panel.tide.curve") == 0 || strcmp(path, "panel.solunar") == 0 ||
                    strcmp(path, "panel.sun") == 0 || strcmp(path, "panel.wind.rose") == 0);
}

int dash_graphic_deps(const char *path, const char *const **out)
{
    static const char *const s_wind[] = {
        "environment.wind.angleApparent", "environment.wind.speedApparent",
        "environment.wind.angleTrueWater", "environment.wind.speedTrue",
        "navigation.headingMagnetic", "navigation.headingTrue",
    };
    if (path && strcmp(path, "panel.wind.rose") == 0) {
        *out = s_wind;
        return sizeof(s_wind) / sizeof(s_wind[0]);
    }
    *out = nullptr;
    return 0;
}

bool dash_is_angle(const DashTile *t)
{
    return strcmp(t->sk_units, "rad") == 0 || strcmp(t->sk_units, "deg") == 0;
}

uint8_t dash_effective_angle_style(const DashTile *t)
{
    if (t->angle != DASH_ANGLE_AUTO && t->angle < DASH_ANGLE_COUNT) return t->angle;
    const char *p = t->path;
    if (path_has(p, "angleApparent") || path_has(p, "angleTrue") ||
        path_has(p, "rudderAngle") || path_has(p, "windAngle")) {
        return DASH_ANGLE_PS;
    }
    if (path_has(p, "attitude") || path_has(p, "leeway") || path_has(p, "Variation") ||
        path_has(p, "roll") || path_has(p, "pitch") || path_has(p, "elevation")) {
        return DASH_ANGLE_180;
    }
    return DASH_ANGLE_360;
}

static int beaufort(float ms)
{
    static const float limits[] = { 0.5f, 1.6f, 3.4f, 5.5f, 8.0f, 10.8f, 13.9f,
                                    17.2f, 20.8f, 24.5f, 28.5f, 32.7f };
    int b = 0;
    while (b < 12 && ms >= limits[b]) b++;
    return b;
}

float dash_convert(const DashTile *t, float si, const char **unit_out, int *dec_out)
{
    const char *sk = t->sk_units;
    const char *id = t->unit[0] ? t->unit : auto_unit_id(sk, t->path);
    const UnitDef *u = unit_by_id(sk, id);
    if (!u) u = unit_first(sk);
    if (!u) {
        if (unit_out) *unit_out = sk;
        if (dec_out)  *dec_out = 2;
        return si;
    }
    if (unit_out) *unit_out = u->label;
    if (dec_out)  *dec_out  = u->decimals;
    if (strcmp(u->id, "bft") == 0) return (float)beaufort(fabsf(si));
    return si * u->factor + u->offset;
}

void dash_format(const DashTile *t, float si, DashFormatted *out)
{
    memset(out, 0, sizeof(*out));
    if (strcmp(t->sk_units, "tod") == 0) {
        int s = (int)lroundf(si);
        s = ((s % 86400) + 86400) % 86400;
        snprintf(out->value, sizeof(out->value), "%02d:%02d", s / 3600, (s / 60) % 60);
        out->display = s / 3600.0f;
        return;
    }
    if (strcmp(t->sk_units, "date") == 0) {
        int md = (int)lroundf(si);
        snprintf(out->value, sizeof(out->value), "%02d/%02d", md % 100, (md / 100) % 100);
        out->display = (float)md;
        return;
    }
    if (strcmp(t->sk_units, "dur") == 0 && (!t->unit[0] || strcmp(t->unit, "hmm") == 0)) {
        int s = (int)lroundf(si);
        if (s < 0) s = 0;
        int m = (s + 30) / 60;
        snprintf(out->value, sizeof(out->value), "%d:%02d", m / 60, m % 60);
        out->display = s / 3600.0f;
        return;
    }
    const char *unit = "";
    int dec = 2;
    float v = dash_convert(t, si, &unit, &dec);
    bool angle = dash_is_angle(t);
    bool degree = angle;
    out->side = 0;

    if (angle) {
        uint8_t style = dash_effective_angle_style(t);
        float d = fmodf(v, 360.0f);
        if (style == DASH_ANGLE_360) {
            if (d < 0) d += 360.0f;
            if (d >= 359.5f) d = 0.0f;
        } else {
            if (d > 180.0f)   d -= 360.0f;
            if (d <= -180.0f) d += 360.0f;
            if (style == DASH_ANGLE_PS) {
                out->side = (d < -0.5f) ? -1 : (d > 0.5f ? 1 : 0);
                d = fabsf(d);
            }
        }
        v = d;
        if (style == DASH_ANGLE_PS) {
            unit = out->side < 0 ? "P" : (out->side > 0 ? "S" : "");
        } else if (style == DASH_ANGLE_360) {
            unit = path_has(t->path, "Magnetic") ? "M" : (path_has(t->path, "True") ? "T" : "");
        } else {
            unit = "";
        }
    }

    out->display = v;

    if (t->decimals >= 0) {
        dec = t->decimals;
    } else if (fabsf(v) >= 100.0f) {
        dec = 0;
    }

    // Degree symbol is moved into the value (the large digit fonts include it)
    if (!angle && strncmp(unit, DEG, 2) == 0) {
        degree = true;
        unit += 2;
    }

    char num[16];
    if (angle && dash_effective_angle_style(t) == DASH_ANGLE_360 && dec == 0) {
        snprintf(num, sizeof(num), "%03d", (int)lroundf(v) % 360);
    } else {
        snprintf(num, sizeof(num), "%.*f", dec, v);
        // Avoid "-0.0"
        if (num[0] == '-') {
            bool all_zero = true;
            for (const char *c = num + 1; *c; c++) {
                if (*c != '0' && *c != '.') { all_zero = false; break; }
            }
            if (all_zero) memmove(num, num + 1, strlen(num));
        }
    }
    size_t nl = strnlen(num, sizeof(out->value) - 3);
    memcpy(out->value, num, nl);
    if (degree) {
        memcpy(out->value + nl, DEG, 2);
        nl += 2;
    }
    out->value[nl] = '\0';
    snprintf(out->unit, sizeof(out->unit), "%s", unit);
}

// ---------------------------------------------------------------------------
// Tiles and pages
// ---------------------------------------------------------------------------

void dash_tile_clear(DashTile *t)
{
    memset(t, 0, sizeof(*t));
    t->decimals = -1;
    t->smooth   = 1;
}

static void label_from_path(const char *path, char *out, size_t len)
{
    const char *last = strrchr(path, '.');
    last = last ? last + 1 : path;
    size_t j = 0;
    for (size_t i = 0; last[i] && j + 1 < len; i++) {
        char c = last[i];
        if (i > 0 && isupper((unsigned char)c) && j + 2 < len) out[j++] = ' ';
        out[j++] = (char)toupper((unsigned char)c);
    }
    out[j] = '\0';
}

void dash_tile_set_path(DashTile *t, const char *path, const char *sk_units,
                        const char *display_name)
{
    bool same = strcmp(t->path, path) == 0;
    snprintf(t->path, sizeof(t->path), "%s", path);
    const char *units = (sk_units && sk_units[0]) ? sk_units : dash_guess_sk_units(path);
    snprintf(t->sk_units, sizeof(t->sk_units), "%s", units);
    if (!same) {
        t->unit[0] = '\0';
        t->angle   = DASH_ANGLE_AUTO;
        t->decimals = -1;
        t->alarm_lo_en = t->alarm_hi_en = false;
        const char *f = dash_friendly_label(path);
        if (f) {
            snprintf(t->label, sizeof(t->label), "%s", f);
        } else if (display_name && display_name[0]) {
            snprintf(t->label, sizeof(t->label), "%s", display_name);
        } else {
            label_from_path(path, t->label, sizeof(t->label));
        }
    }
}

void dash_page_init(DashPage *p, const char *name, uint8_t layout)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->layout = clamp_layout(layout);
    for (int i = 0; i < DASH_MAX_TILES; i++) dash_tile_clear(&p->tiles[i]);
}

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

struct DefaultPage { const char *name; uint8_t layout; const char *paths[4]; };

static const DefaultPage s_default_pages[] = {
    { "Sail",  DASH_LAYOUT_2X2, { "environment.wind.speedApparent", "environment.wind.angleApparent",
                                  "environment.wind.speedTrue", "environment.wind.angleTrueWater" } },
    { "Nav",   DASH_LAYOUT_2X2, { "navigation.speedOverGround", "navigation.courseOverGroundTrue",
                                  "navigation.headingMagnetic", "environment.depth.belowTransducer" } },
    { "Depth", DASH_LAYOUT_1_2, { "environment.depth.belowTransducer", "navigation.speedThroughWater",
                                  "navigation.speedOverGround", nullptr } },
    { "Env",   DASH_LAYOUT_2X2, { "environment.water.temperature", "environment.outside.temperature",
                                  "environment.outside.pressure", "environment.wind.directionTrue" } },
};

static void default_settings(DashSettings *s)
{
    s->theme      = DASH_THEME_DARK;
    s->brightness = 100;
    s->cycle_s    = 0;
    s->stale_s    = 10;
    s->locked     = false;
    s->start_page = -1;
    s->auto_switch = false;
    s->cycle_group = true;
    s->auto_kn     = 2.0f;
    s->anchor_s    = 300;
    s->sail_s      = 60;
}

void dash_config_defaults_filtered(DashConfig *out, bool (*avail)(const char *path))
{
    memset(out, 0, sizeof(*out));
    default_settings(&out->settings);
    for (const auto &dp : s_default_pages) {
        DashPage *p = &out->pages[out->page_count];
        dash_page_init(p, dp.name, dp.layout);
        int used = 0;
        for (int i = 0; i < 4 && dp.paths[i]; i++) {
            if (avail && !avail(dp.paths[i])) continue;
            dash_tile_set_path(&p->tiles[used++], dp.paths[i], nullptr, nullptr);
        }
        if (used == 0) continue;
        if (avail) {
            // Shrink the layout to the tiles actually available
            if (used == 1)      p->layout = DASH_LAYOUT_1;
            else if (used == 2) p->layout = DASH_LAYOUT_2R;
            else if (used == 3 && p->layout != DASH_LAYOUT_1_2) p->layout = DASH_LAYOUT_1_2;
        }
        out->page_count++;
    }
    if (out->page_count == 0) {
        dash_page_init(&out->pages[0], "Page 1", DASH_LAYOUT_2X2);
        out->page_count = 1;
    }
}

void dash_config_defaults(DashConfig *out)
{
    dash_config_defaults_filtered(out, nullptr);
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

static const char *s_theme_ids[DASH_THEME_COUNT] = { "dark", "day", "night", "auto" };
static const char *s_angle_ids[DASH_ANGLE_COUNT] = { "auto", "360", "180", "ps" };

static int id_index(const char *const *ids, int n, const char *v, int def)
{
    if (!v) return def;
    for (int i = 0; i < n; i++) if (strcmp(ids[i], v) == 0) return i;
    return def;
}

static void copy_str(char *dst, size_t len, const cJSON *obj, const char *key)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(j) && j->valuestring) {
        snprintf(dst, len, "%s", j->valuestring);
    }
}

static double get_num(const cJSON *obj, const char *key, double def)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(j) ? j->valuedouble : def;
}

static bool get_bool(const cJSON *obj, const char *key, bool def)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(j)) return cJSON_IsTrue(j);
    return def;
}

static int clampi(double v, int lo, int hi)
{
    if (isnan(v)) return lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (int)v;
}

cJSON *dash_config_to_json(const DashConfig *cfg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON *s = cJSON_AddObjectToObject(root, "settings");
    cJSON_AddStringToObject(s, "theme", s_theme_ids[cfg->settings.theme % DASH_THEME_COUNT]);
    cJSON_AddNumberToObject(s, "brightness", cfg->settings.brightness);
    cJSON_AddNumberToObject(s, "cycle", cfg->settings.cycle_s);
    cJSON_AddNumberToObject(s, "stale", cfg->settings.stale_s);
    cJSON_AddBoolToObject(s, "locked", cfg->settings.locked);
    cJSON_AddNumberToObject(s, "startPage", cfg->settings.start_page);
    cJSON_AddBoolToObject(s, "autoSwitch", cfg->settings.auto_switch);
    cJSON_AddNumberToObject(s, "autoSpeed", roundf(cfg->settings.auto_kn * 10.0f) / 10.0);
    cJSON_AddNumberToObject(s, "anchorDelay", cfg->settings.anchor_s);
    cJSON_AddNumberToObject(s, "sailDelay", cfg->settings.sail_s);
    cJSON_AddBoolToObject(s, "cycleGroup", cfg->settings.cycle_group);

    cJSON *pages = cJSON_AddArrayToObject(root, "pages");
    for (int p = 0; p < cfg->page_count; p++) {
        const DashPage *pg = &cfg->pages[p];
        cJSON *jp = cJSON_CreateObject();
        cJSON_AddStringToObject(jp, "name", pg->name);
        cJSON_AddStringToObject(jp, "layout", dash_layout_id(pg->layout));
        if (pg->group) cJSON_AddStringToObject(jp, "group", dash_group_id(pg->group));
        cJSON *tiles = cJSON_AddArrayToObject(jp, "tiles");
        int n = dash_layout_tile_count(pg->layout);
        for (int i = 0; i < n; i++) {
            const DashTile *t = &pg->tiles[i];
            cJSON *jt = cJSON_CreateObject();
            cJSON_AddStringToObject(jt, "path", t->path);
            if (t->path[0]) {
                cJSON_AddStringToObject(jt, "label", t->label);
                cJSON_AddStringToObject(jt, "skUnits", t->sk_units);
                cJSON_AddStringToObject(jt, "unit", t->unit);
                cJSON_AddNumberToObject(jt, "decimals", t->decimals);
                cJSON_AddStringToObject(jt, "angle", s_angle_ids[t->angle % DASH_ANGLE_COUNT]);
                cJSON_AddNumberToObject(jt, "smooth", t->smooth);
                cJSON_AddNumberToObject(jt, "trend", t->trend);
                if (t->alarm_lo_en) cJSON_AddNumberToObject(jt, "alarmLo", t->alarm_lo);
                if (t->alarm_hi_en) cJSON_AddNumberToObject(jt, "alarmHi", t->alarm_hi);
                cJSON_AddBoolToObject(jt, "beep", t->alarm_beep);
            }
            cJSON_AddItemToArray(tiles, jt);
        }
        cJSON_AddItemToArray(pages, jp);
    }
    return root;
}

bool dash_config_from_json(const cJSON *root, DashConfig *out)
{
    if (!cJSON_IsObject(root)) return false;
    const cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
    if (!cJSON_IsArray(pages)) return false;

    memset(out, 0, sizeof(*out));
    default_settings(&out->settings);

    const cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "settings");
    if (cJSON_IsObject(s)) {
        const cJSON *th = cJSON_GetObjectItemCaseSensitive(s, "theme");
        out->settings.theme = (uint8_t)id_index(s_theme_ids, DASH_THEME_COUNT,
                                                cJSON_IsString(th) ? th->valuestring : nullptr, 0);
        out->settings.brightness = (uint8_t)clampi(get_num(s, "brightness", 100), 10, 100);
        out->settings.cycle_s    = (uint8_t)clampi(get_num(s, "cycle", 0), 0, 120);
        out->settings.stale_s    = (uint8_t)clampi(get_num(s, "stale", 10), 2, 120);
        out->settings.locked     = get_bool(s, "locked", false);
        out->settings.start_page = (int8_t)clampi(get_num(s, "startPage", -1), -1, DASH_MAX_PAGES - 1);
        out->settings.auto_switch = get_bool(s, "autoSwitch", false);
        out->settings.cycle_group = get_bool(s, "cycleGroup", true);
        double kn = get_num(s, "autoSpeed", 2.0);
        out->settings.auto_kn  = isnan(kn) ? 2.0f : (float)fmin(fmax(kn, 0.2), 20.0);
        out->settings.anchor_s = (uint16_t)clampi(get_num(s, "anchorDelay", 300), 5, 3600);
        out->settings.sail_s   = (uint16_t)clampi(get_num(s, "sailDelay", 60), 5, 3600);
    }

    const cJSON *jp;
    cJSON_ArrayForEach(jp, pages) {
        if (out->page_count >= DASH_MAX_PAGES) break;
        if (!cJSON_IsObject(jp)) continue;
        DashPage *pg = &out->pages[out->page_count];
        const cJSON *lj = cJSON_GetObjectItemCaseSensitive(jp, "layout");
        int layout = dash_layout_from_id(cJSON_IsString(lj) ? lj->valuestring : nullptr);
        dash_page_init(pg, "", layout < 0 ? DASH_LAYOUT_2X2 : (uint8_t)layout);
        copy_str(pg->name, sizeof(pg->name), jp, "name");
        if (!pg->name[0]) snprintf(pg->name, sizeof(pg->name), "Page %d", out->page_count + 1);
        const cJSON *gj = cJSON_GetObjectItemCaseSensitive(jp, "group");
        pg->group = (uint8_t)id_index(s_group_ids, DASH_GROUP_COUNT,
                                      cJSON_IsString(gj) ? gj->valuestring : nullptr, 0);

        const cJSON *tiles = cJSON_GetObjectItemCaseSensitive(jp, "tiles");
        int i = 0;
        const cJSON *jt;
        cJSON_ArrayForEach(jt, tiles) {
            if (i >= DASH_MAX_TILES) break;
            DashTile *t = &pg->tiles[i++];
            if (!cJSON_IsObject(jt)) continue;
            copy_str(t->path, sizeof(t->path), jt, "path");
            if (!t->path[0]) continue;
            copy_str(t->label, sizeof(t->label), jt, "label");
            copy_str(t->sk_units, sizeof(t->sk_units), jt, "skUnits");
            if (!t->sk_units[0]) {
                snprintf(t->sk_units, sizeof(t->sk_units), "%s", dash_guess_sk_units(t->path));
            }
            copy_str(t->unit, sizeof(t->unit), jt, "unit");
            if (t->unit[0] && !unit_by_id(t->sk_units, t->unit)) t->unit[0] = '\0';
            t->decimals = (int8_t)clampi(get_num(jt, "decimals", -1), -1, 4);
            const cJSON *aj = cJSON_GetObjectItemCaseSensitive(jt, "angle");
            t->angle  = (uint8_t)id_index(s_angle_ids, DASH_ANGLE_COUNT,
                                          cJSON_IsString(aj) ? aj->valuestring : nullptr, 0);
            t->smooth = (uint8_t)clampi(get_num(jt, "smooth", 1), 0, 3);
            t->trend  = (uint8_t)clampi(get_num(jt, "trend", 0), 0, 4);
            const cJSON *lo = cJSON_GetObjectItemCaseSensitive(jt, "alarmLo");
            const cJSON *hi = cJSON_GetObjectItemCaseSensitive(jt, "alarmHi");
            t->alarm_lo_en = cJSON_IsNumber(lo);
            t->alarm_hi_en = cJSON_IsNumber(hi);
            t->alarm_lo = t->alarm_lo_en ? (float)lo->valuedouble : 0.0f;
            t->alarm_hi = t->alarm_hi_en ? (float)hi->valuedouble : 0.0f;
            t->alarm_beep = get_bool(jt, "beep", false);
            if (!t->label[0]) {
                const char *f = dash_friendly_label(t->path);
                if (f) snprintf(t->label, sizeof(t->label), "%s", f);
                else   label_from_path(t->path, t->label, sizeof(t->label));
            }
        }
        out->page_count++;
    }
    if (out->page_count == 0) {
        dash_page_init(&out->pages[0], "Page 1", DASH_LAYOUT_2X2);
        out->page_count = 1;
    }
    return true;
}

cJSON *dash_units_to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    for (int i = 0; i < s_unit_count; i++) {
        cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, s_units[i].sk);
        if (!arr) arr = cJSON_AddArrayToObject(root, s_units[i].sk);
        cJSON *opt = cJSON_CreateArray();
        cJSON_AddItemToArray(opt, cJSON_CreateString(s_units[i].id));
        cJSON_AddItemToArray(opt, cJSON_CreateString(s_units[i].label));
        cJSON_AddItemToArray(arr, opt);
    }
    return root;
}

// ---------------------------------------------------------------------------
// Shared storage (LittleFS on the "spiffs" data partition)
// ---------------------------------------------------------------------------

#ifndef SIMULATOR_BUILD

static const char *FS_BASE   = "/dash";
static const char *CFG_FILE  = "/dash/dashboard.json";
static const char *CFG_TMP   = "/dash/dashboard.tmp";
static const char *NVS_NS    = "dash";

static DashConfig        *s_cfg       = nullptr;
static SemaphoreHandle_t  s_lock      = nullptr;
static volatile uint32_t  s_revision  = 1;
static bool               s_is_default = true;
static bool               s_fs_ok     = false;
static int                s_last_page = 0;

static bool fs_mount(void)
{
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = FS_BASE;
    conf.partition_label = "spiffs";
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return false;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info("spiffs", &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at %s (%u KB used of %u KB)", FS_BASE,
             (unsigned)(used / 1024), (unsigned)(total / 1024));
    return true;
}

static bool load_file(DashConfig *out)
{
    FILE *f = fopen(CFG_FILE, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 256 * 1024) { fclose(f); return false; }
    char *buf = (char *)malloc(len + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, len, f);
    fclose(f);
    buf[rd] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    bool ok = root && dash_config_from_json(root, out);
    cJSON_Delete(root);
    return ok;
}

static bool save_file(const DashConfig *cfg)
{
    if (!s_fs_ok) return false;
    cJSON *root = dash_config_to_json(cfg);
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return false;
    bool ok = false;
    FILE *f = fopen(CFG_TMP, "wb");
    if (f) {
        size_t len = strlen(txt);
        ok = fwrite(txt, 1, len, f) == len;
        ok = (fclose(f) == 0) && ok;
        if (ok) {
            remove(CFG_FILE);
            ok = rename(CFG_TMP, CFG_FILE) == 0;
        }
    }
    cJSON_free(txt);
    if (!ok) ESP_LOGE(TAG, "Failed to save %s", CFG_FILE);
    return ok;
}

void dash_config_init(void)
{
    if (s_cfg) return;
    s_lock = xSemaphoreCreateMutex();
    s_cfg = (DashConfig *)calloc(1, sizeof(DashConfig));
    s_fs_ok = fs_mount();
    if (s_fs_ok && load_file(s_cfg)) {
        s_is_default = false;
        ESP_LOGI(TAG, "Loaded dashboard: %d pages", s_cfg->page_count);
    } else {
        dash_config_defaults(s_cfg);
        s_is_default = true;
        ESP_LOGI(TAG, "Using default dashboard (%d pages)", s_cfg->page_count);
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v = 0;
        if (nvs_get_i32(h, "last", &v) == ESP_OK) s_last_page = v;
        nvs_close(h);
    }
}

void dash_config_get(DashConfig *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = *s_cfg;
    xSemaphoreGive(s_lock);
}

static void sanitize(DashConfig *c)
{
    if (c->page_count == 0) {
        dash_page_init(&c->pages[0], "Page 1", DASH_LAYOUT_2X2);
        c->page_count = 1;
    }
    if (c->page_count > DASH_MAX_PAGES) c->page_count = DASH_MAX_PAGES;
    for (int p = 0; p < c->page_count; p++) {
        DashPage *pg = &c->pages[p];
        pg->layout = clamp_layout(pg->layout);
        if (pg->group >= DASH_GROUP_COUNT) pg->group = DASH_GROUP_NONE;
        pg->name[sizeof(pg->name) - 1] = '\0';
        for (int i = 0; i < DASH_MAX_TILES; i++) {
            DashTile *t = &pg->tiles[i];
            t->path[sizeof(t->path) - 1] = '\0';
            t->label[sizeof(t->label) - 1] = '\0';
            if (t->smooth > 3) t->smooth = 3;
            if (t->trend > 4)  t->trend = 4;
            if (t->angle >= DASH_ANGLE_COUNT) t->angle = 0;
        }
    }
    DashSettings &s = c->settings;
    if (s.theme >= DASH_THEME_COUNT) s.theme = 0;
    if (s.brightness < 10)  s.brightness = 10;
    if (s.brightness > 100) s.brightness = 100;
    if (s.stale_s < 2)      s.stale_s = 2;
    if (s.start_page >= c->page_count) s.start_page = -1;
    if (!(s.auto_kn >= 0.2f && s.auto_kn <= 20.0f)) s.auto_kn = 2.0f;
    if (s.anchor_s < 5) s.anchor_s = 5;
    if (s.sail_s < 5)   s.sail_s = 5;
    if (s.anchor_s > 3600) s.anchor_s = 3600;
    if (s.sail_s > 3600)   s.sail_s = 3600;
}

void dash_config_set(const DashConfig *cfg)
{
    DashConfig *tmp = (DashConfig *)malloc(sizeof(DashConfig));
    if (!tmp) return;
    *tmp = *cfg;
    sanitize(tmp);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *s_cfg = *tmp;
    s_is_default = false;
    s_revision++;
    save_file(tmp);
    xSemaphoreGive(s_lock);
    free(tmp);
}

uint32_t dash_config_revision(void) { return s_revision; }
bool     dash_config_is_default(void) { return s_is_default; }

void dash_config_reset(void)
{
    DashConfig *tmp = (DashConfig *)malloc(sizeof(DashConfig));
    if (!tmp) return;
    dash_config_defaults(tmp);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *s_cfg = *tmp;
    s_is_default = true;
    s_revision++;
    if (s_fs_ok) remove(CFG_FILE);
    xSemaphoreGive(s_lock);
    free(tmp);
}

int dash_last_page_get(void) { return s_last_page; }

void dash_last_page_set(int page)
{
    if (page == s_last_page) return;
    s_last_page = page;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "last", page);
        nvs_commit(h);
        nvs_close(h);
    }
}

#else  // SIMULATOR_BUILD

static DashConfig s_cfg;
static uint32_t   s_revision = 1;
static int        s_last_page = 0;

void dash_config_init(void)              { dash_config_defaults(&s_cfg); }
void dash_config_get(DashConfig *out)    { *out = s_cfg; }
void dash_config_set(const DashConfig *c){ s_cfg = *c; s_revision++; }
uint32_t dash_config_revision(void)      { return s_revision; }
bool dash_config_is_default(void)        { return false; }
void dash_config_reset(void)             { dash_config_defaults(&s_cfg); s_revision++; }
int  dash_last_page_get(void)            { return s_last_page; }
void dash_last_page_set(int page)        { s_last_page = page; }

#endif
