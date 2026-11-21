#pragma once

// Instruments dashboard runtime engine.
// Runs on the LVGL thread (lv_timer, 250 ms) from boot so that trends,
// min/max and alarms keep working while the dashboard app is closed.

#include "dash_model.hpp"

static const int DASH_TREND_POINTS = 60;

enum DashAlarm : uint8_t { DASH_ALARM_NONE = 0, DASH_ALARM_LOW, DASH_ALARM_HIGH };

struct DashTileState {
    char     path[96];
    uint32_t cfg_hash;
    bool     has_value;     // a value has been received at least once
    bool     stale;         // no update within the stale timeout
    float    value_si;      // smoothed SI value
    float    sx, sy;        // smoothed unit vector (angles)
    bool     minmax_valid;
    float    min_disp, max_disp;
    uint8_t  alarm;         // DashAlarm
    bool     acked;
    uint32_t last_beep_ms;
    float    hist[DASH_TREND_POINTS];
    uint8_t  hist_head;     // next write index
    uint8_t  hist_count;
    uint32_t next_sample_ms;
};

typedef void (*dash_beep_cb_t)(void);

// Backlight and day/night are owned by the day_night component.
void dash_engine_init(dash_beep_cb_t beep_cb);

// State access. Lock is required from tasks other than the LVGL thread.
void dash_engine_lock(void);
void dash_engine_unlock(void);
const DashTileState *dash_engine_state(int page, int slot);

// LVGL thread only
const DashConfig *dash_engine_config(void);
void dash_engine_reset_minmax(int page, int slot);
void dash_engine_ack(int page, int slot);
bool dash_engine_page_alarm(int page);

// Automatic anchor / sail detection from navigation.speedOverGround.
// state uses the DashGroup values (NONE = not yet determined / disabled).
struct DashMotion {
    bool     enabled;
    uint8_t  state;         // DashGroup
    bool     sog_valid;
    float    sog_kn;        // lightly smoothed
    uint8_t  pending;       // group the timer is counting towards (NONE if none)
    uint32_t pending_s;     // how long the condition has held
    uint32_t need_s;        // how long it must hold
    uint32_t seq;           // incremented on every state change
};
void dash_engine_motion(DashMotion *out);   // any task
