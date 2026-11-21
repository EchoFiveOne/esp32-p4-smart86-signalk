#pragma once

// Instruments dashboard — configuration model, persistence and value formatting.
// The configuration is shared between the LVGL thread (device UI) and the
// HTTP server task (web editor); always use the copy/set functions.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct cJSON;

static const int DASH_MAX_PAGES = 12;
static const int DASH_MAX_TILES = 16;         // per page (4x4)

enum DashLayout : uint8_t {
    DASH_LAYOUT_1 = 0,
    DASH_LAYOUT_2R,     // 2 rows
    DASH_LAYOUT_2C,     // 2 columns
    DASH_LAYOUT_3R,     // 3 rows
    DASH_LAYOUT_1_2,    // 1 large on top, 2 below
    DASH_LAYOUT_2X2,
    DASH_LAYOUT_2X3,    // 2 columns x 3 rows
    DASH_LAYOUT_3X2,    // 3 columns x 2 rows
    DASH_LAYOUT_3X3,
    DASH_LAYOUT_1_3,    // 1 large on top, 3 below
    DASH_LAYOUT_1_4,    // 1 large on top, 4 below
    DASH_LAYOUT_2_3,    // 2 large side by side, 3 below
    DASH_LAYOUT_2_4,    // 2 large side by side, 4 below
    DASH_LAYOUT_4R,     // 4 rows
    DASH_LAYOUT_4X4,
    DASH_LAYOUT_COUNT
};

// Cell rectangle on a DASH_GRID x DASH_GRID unit grid
static const int DASH_GRID = 12;
struct DashCell { uint8_t x, y, w, h; };

enum DashAngleStyle : uint8_t {
    DASH_ANGLE_AUTO = 0,
    DASH_ANGLE_360,      // 0..359
    DASH_ANGLE_180,      // -180..+180
    DASH_ANGLE_PS,       // 0..180 with port (red) / starboard (green)
    DASH_ANGLE_COUNT
};

enum DashTheme : uint8_t {
    DASH_THEME_DARK = 0,
    DASH_THEME_DAY,
    DASH_THEME_NIGHT,
    DASH_THEME_AUTO,        // Day colours in daylight, Dark at night (panel day/night)
    DASH_THEME_COUNT
};

struct DashTile {
    char    path[96];
    char    label[24];
    char    sk_units[12];   // units reported by the server meta ("m/s", "rad", ...)
    char    unit[8];        // display unit id, "" = automatic
    int8_t  decimals;       // -1 = automatic
    uint8_t angle;          // DashAngleStyle
    uint8_t smooth;         // 0 off, 1 low, 2 medium, 3 high
    uint8_t trend;          // 0 off, 1 = 1 min, 2 = 5 min, 3 = 15 min, 4 = 60 min
    bool    alarm_lo_en;
    bool    alarm_hi_en;
    bool    alarm_beep;
    float   alarm_lo;       // display units
    float   alarm_hi;       // display units
};

// Page groups for automatic anchor / sail switching
enum DashGroup : uint8_t {
    DASH_GROUP_NONE = 0,
    DASH_GROUP_ANCHOR,
    DASH_GROUP_SAIL,
    DASH_GROUP_COUNT
};

struct DashPage {
    char     name[20];
    uint8_t  layout;        // DashLayout
    uint8_t  group;         // DashGroup
    DashTile tiles[DASH_MAX_TILES];
};

struct DashSettings {
    uint8_t theme;          // DashTheme
    uint8_t brightness;     // 10..100 %
    uint8_t cycle_s;        // auto page cycling, 0 = off
    uint8_t stale_s;        // seconds without data before showing ---
    bool    locked;         // disable on-device editing
    int8_t  start_page;     // -1 = last viewed
    // Automatic anchor / sail page switching from speed over ground
    bool     auto_switch;
    bool     cycle_group;   // auto cycling stays within the active group
    float    auto_kn;       // SOG threshold (knots)
    uint16_t anchor_s;      // below the threshold this long -> anchor group
    uint16_t sail_s;        // above the threshold this long -> sail group
};

struct DashConfig {
    DashSettings settings;
    uint8_t      page_count;
    DashPage     pages[DASH_MAX_PAGES];
};

// --- Layouts ---------------------------------------------------------------
int             dash_layout_tile_count(uint8_t layout);
const DashCell *dash_layout_cells(uint8_t layout);
const char     *dash_layout_id(uint8_t layout);      // "2x2"
const char     *dash_layout_name(uint8_t layout);    // "2 x 2 grid"
const char     *dash_group_id(uint8_t group);        // "", "anchor", "sail"
// First page in a group (-1 if none)
int             dash_group_first_page(const DashConfig *cfg, uint8_t group);
int             dash_layout_from_id(const char *id); // -1 if unknown

// --- Shared configuration ---------------------------------------------------
void     dash_config_init(void);                   // load from NVS (after nvs_flash_init)
void     dash_config_get(DashConfig *out);
void     dash_config_set(const DashConfig *cfg);   // validates, saves, bumps revision
uint32_t dash_config_revision(void);
bool     dash_config_is_default(void);             // true until the user first saves
void     dash_config_defaults(DashConfig *out);
// Build defaults keeping only tiles whose path is in avail (NULL = keep all)
void     dash_config_defaults_filtered(DashConfig *out, bool (*avail)(const char *path));
void     dash_config_reset(void);

int      dash_last_page_get(void);
void     dash_last_page_set(int page);             // persisted when it changes

void     dash_tile_clear(DashTile *t);
void     dash_tile_set_path(DashTile *t, const char *path, const char *sk_units,
                            const char *display_name);
void     dash_page_init(DashPage *p, const char *name, uint8_t layout);

// --- JSON ---------------------------------------------------------------------
cJSON   *dash_config_to_json(const DashConfig *cfg);
bool     dash_config_from_json(const cJSON *root, DashConfig *out);
cJSON   *dash_units_to_json(void);                 // unit options for the web editor

// --- Units and formatting ---------------------------------------------------------
struct DashUnitOption { const char *id; const char *label; };

// Options available for a Signal K unit. Returns count; first option is always Auto.
int  dash_unit_options(const char *sk_units, const DashUnitOption **out);

// Guess the Signal K unit from well known paths when the server gives no meta.
const char *dash_guess_sk_units(const char *path);

// Friendly short label for well known paths (NULL if none).
const char *dash_friendly_label(const char *path);

// Convert an SI value to the tile's display unit.
// unit_out gets the unit label ("kn", "°", "°C" ...). dec_out gets automatic decimals.
float dash_convert(const DashTile *t, float si, const char **unit_out, int *dec_out);

// panel.* paths are computed on the device (clock, date, sun, tide, moon), not subscribed.
bool  dash_is_virtual(const char *path);
// Clock-style tiles (sk_units "tod"/"date"/"dur") are formatted HH:MM / DD/MM / H:MM and never smoothed.
bool  dash_is_clock(const DashTile *t);
// Tiles drawn as graphics instead of a number (tide curve, moon / solunar, sun, wind rose)
bool  dash_is_graphic(const char *path);
// Signal K paths a graphic tile reads (subscribed on its behalf). Returns the count.
int   dash_graphic_deps(const char *path, const char *const **out);

// True if the tile value is an angle (rad) displayed in degrees.
bool  dash_is_angle(const DashTile *t);
uint8_t dash_effective_angle_style(const DashTile *t);

struct DashFormatted {
    char value[16];    // digits, may include sign, decimal point and ° (font-safe)
    char unit[16];     // unit text without degree symbol, e.g. "kn", "C", "P"
    int  side;         // -1 port, +1 starboard, 0 none
    float display;     // converted value used for alarms and trends
};

void  dash_format(const DashTile *t, float si, DashFormatted *out);
