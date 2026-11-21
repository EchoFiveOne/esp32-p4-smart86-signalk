#include "dash_engine.hpp"
#include "sk_catalog.hpp"
#include "signalk_client.hpp"
#include "day_night.hpp"
#include "tides.hpp"
#include "esp_log.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifndef SIMULATOR_BUILD
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

static const char *TAG = "dash_engine";

static const int TICK_MS          = 250;
static const int SUB_PERIOD_MS    = 500;
static const int BEEP_INTERVAL_MS = 4000;
static const int TOTAL_TILES      = DASH_MAX_PAGES * DASH_MAX_TILES;

static DashConfig     *s_cfg      = nullptr;
static DashTileState  *s_state    = nullptr;
static uint32_t        s_rev      = 0;
static bool            s_defaults_checked = false;

// Unique subscribed paths
static char          (*s_subs)[96] = nullptr;
static int             s_sub_count = 0;

static dash_beep_cb_t s_beep_cb = nullptr;

static const char    *SOG_PATH        = "navigation.speedOverGround";
static const uint32_t SOG_MAX_AGE_MS  = 30000;
static DashMotion     s_motion        = {};
static uint8_t        s_cond          = DASH_GROUP_NONE;
static uint32_t       s_cond_since    = 0;

#ifndef SIMULATOR_BUILD
static SemaphoreHandle_t s_lock = nullptr;
void dash_engine_lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
void dash_engine_unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }
static uint32_t now_ms(void)  { return (uint32_t)(esp_timer_get_time() / 1000ULL); }
#else
void dash_engine_lock(void)   {}
void dash_engine_unlock(void) {}
static uint32_t now_ms(void)  { return lv_tick_get(); }
#endif

static uint32_t tile_hash(const DashTile *t)
{
    // FNV-1a over the fields that change what the value means
    uint32_t h = 2166136261u;
    auto mix = [&h](const char *s, size_t n) {
        for (size_t i = 0; i < n && s[i]; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
        h ^= 0xff; h *= 16777619u;
    };
    mix(t->path, sizeof(t->path));
    mix(t->sk_units, sizeof(t->sk_units));
    mix(t->unit, sizeof(t->unit));
    h ^= t->angle; h *= 16777619u;
    return h;
}

static void reset_state(DashTileState *st, const DashTile *t)
{
    memset(st, 0, sizeof(*st));
    snprintf(st->path, sizeof(st->path), "%s", t->path);
    st->cfg_hash = tile_hash(t);
}

static bool sub_has(const char *path)
{
    for (int i = 0; i < s_sub_count; i++) if (strcmp(s_subs[i], path) == 0) return true;
    return false;
}

static bool cfg_uses(const DashConfig *c, const char *path)
{
    if (c->settings.auto_switch && strcmp(path, SOG_PATH) == 0) return true;
    for (int p = 0; p < c->page_count; p++) {
        int n = dash_layout_tile_count(c->pages[p].layout);
        for (int i = 0; i < n; i++) {
            const char *tp = c->pages[p].tiles[i].path;
            if (strcmp(tp, path) == 0) return true;
            const char *const *deps;
            int nd = dash_graphic_deps(tp, &deps);
            for (int k = 0; k < nd; k++) if (strcmp(deps[k], path) == 0) return true;
        }
    }
    return false;
}

static void sub_add_path(const char *path)
{
    if (!path[0] || dash_is_virtual(path) || sub_has(path) || s_sub_count >= STORE_MAX_ENTRIES) return;
    if (signalk_client_subscribe(path, SUB_PERIOD_MS)) {
        snprintf(s_subs[s_sub_count++], 96, "%s", path);
    }
}

static void sync_subscriptions(void)
{
    // Release paths no longer used
    for (int i = 0; i < s_sub_count; ) {
        if (!cfg_uses(s_cfg, s_subs[i])) {
            signalk_client_unsubscribe(s_subs[i]);
            memcpy(s_subs[i], s_subs[s_sub_count - 1], sizeof(s_subs[i]));
            s_sub_count--;
        } else {
            i++;
        }
    }
    // Add new paths (graphic tiles subscribe the Signal K paths they draw)
    for (int p = 0; p < s_cfg->page_count; p++) {
        int n = dash_layout_tile_count(s_cfg->pages[p].layout);
        for (int i = 0; i < n; i++) {
            const char *path = s_cfg->pages[p].tiles[i].path;
            sub_add_path(path);
            const char *const *deps;
            int nd = dash_graphic_deps(path, &deps);
            for (int k = 0; k < nd; k++) sub_add_path(deps[k]);
        }
    }
    if (s_cfg->settings.auto_switch && !sub_has(SOG_PATH) && s_sub_count < STORE_MAX_ENTRIES &&
        signalk_client_subscribe(SOG_PATH, SUB_PERIOD_MS)) {
        snprintf(s_subs[s_sub_count++], 96, "%s", SOG_PATH);
    }
    ESP_LOGI(TAG, "Dashboard subscriptions: %d paths", s_sub_count);
}

static void reload_config(void)
{
    dash_config_get(s_cfg);
    s_rev = dash_config_revision();

    dash_engine_lock();
    for (int p = 0; p < DASH_MAX_PAGES; p++) {
        for (int i = 0; i < DASH_MAX_TILES; i++) {
            DashTileState *st = &s_state[p * DASH_MAX_TILES + i];
            const DashTile *t = &s_cfg->pages[p].tiles[i];
            bool active = p < s_cfg->page_count && i < dash_layout_tile_count(s_cfg->pages[p].layout);
            if (!active) {
                if (st->path[0]) memset(st, 0, sizeof(*st));
                continue;
            }
            if (st->cfg_hash != tile_hash(t)) reset_state(st, t);
        }
    }
    dash_engine_unlock();

    sync_subscriptions();
}

static bool catalog_has(const char *path) { return sk_catalog_has(path); }

static void check_first_run_defaults(void)
{
    if (s_defaults_checked) return;
    if (!dash_config_is_default()) { s_defaults_checked = true; return; }
    if (!signalk_client_is_connected()) return;

    int st = sk_catalog_state();
    if (st == SK_CATALOG_EMPTY) {
        sk_catalog_refresh(false);
        return;
    }
    if (st != SK_CATALOG_READY) return;
    s_defaults_checked = true;
    if (sk_catalog_count() == 0) return;

    DashConfig *tmp = (DashConfig *)malloc(sizeof(DashConfig));
    if (!tmp) return;
    dash_config_defaults_filtered(tmp, catalog_has);
    tmp->settings = s_cfg->settings;
    bool any = false;
    for (int p = 0; p < tmp->page_count && !any; p++) {
        any = tmp->pages[p].tiles[0].path[0] != '\0';
    }
    if (any) {
        ESP_LOGI(TAG, "First run: created %d default pages from available data", tmp->page_count);
        dash_config_set(tmp);
    }
    free(tmp);
}

static const float s_alpha[4] = { 1.0f, 0.5f, 0.25f, 0.1f };
static const uint32_t s_trend_ms[5] = { 0, 1000, 5000, 15000, 60000 };

static void motion_tick(uint32_t now)
{
    const DashSettings &cs = s_cfg->settings;
    DashMotion m = s_motion;
    m.enabled = cs.auto_switch;
    if (!cs.auto_switch) {
        m.state = m.pending = DASH_GROUP_NONE;
        m.sog_valid = false;
        m.pending_s = m.need_s = 0;
        s_cond = DASH_GROUP_NONE;
    } else {
        const char *p = SOG_PATH;
        PathValue v;
        bool ok = signalk_client_get_values(&p, 1, &v) && v.valid && now - v.last_update_ms < SOG_MAX_AGE_MS;
        if (ok) {
            float kn = fabsf(v.value) * 1.943844f;
            // ~2 s smoothing so a single GPS spike doesn't restart the timers
            m.sog_kn = m.sog_valid ? m.sog_kn + 0.12f * (kn - m.sog_kn) : kn;
        }
        m.sog_valid = ok;
        // Strictly below -> anchor, strictly above -> sail; exactly at the threshold counts as neither
        uint8_t cond = !ok ? DASH_GROUP_NONE
                     : m.sog_kn < cs.auto_kn ? DASH_GROUP_ANCHOR
                     : m.sog_kn > cs.auto_kn ? DASH_GROUP_SAIL : DASH_GROUP_NONE;
        if (cond != s_cond) { s_cond = cond; s_cond_since = now; }
        m.pending   = cond != m.state ? cond : DASH_GROUP_NONE;
        m.need_s    = cond == DASH_GROUP_ANCHOR ? cs.anchor_s : cond == DASH_GROUP_SAIL ? cs.sail_s : 0;
        m.pending_s = cond ? (now - s_cond_since) / 1000u : 0;
        if (m.pending && m.pending_s >= m.need_s) {
            ESP_LOGI(TAG, "Motion: %s (SOG %.1f kn for %u s)", dash_group_id(cond), m.sog_kn, (unsigned)m.pending_s);
            m.state   = cond;
            m.pending = DASH_GROUP_NONE;
            m.seq++;
        }
    }
    dash_engine_lock();
    s_motion = m;
    dash_engine_unlock();
}

void dash_engine_motion(DashMotion *out)
{
    dash_engine_lock();
    *out = s_motion;
    dash_engine_unlock();
}

static void tick(lv_timer_t *)
{
    if (dash_config_revision() != s_rev) reload_config();
    check_first_run_defaults();
    tides_tick();
    motion_tick(now_ms());

    static const char *paths[TOTAL_TILES];
    static PathValue   vals[TOTAL_TILES];
    int n = 0;
    for (int p = 0; p < s_cfg->page_count; p++) {
        for (int i = 0; i < DASH_MAX_TILES; i++) {
            paths[p * DASH_MAX_TILES + i] = s_cfg->pages[p].tiles[i].path;
        }
        n = (p + 1) * DASH_MAX_TILES;
    }
    if (n == 0) return;
    if (!signalk_client_get_values(paths, n, vals)) {
        for (int i = 0; i < n; i++) vals[i] = PathValue{};
    }

    uint32_t now = now_ms();
    for (int i = 0; i < n; i++) {
        float v;
        if (dash_is_virtual(paths[i]) &&
            (day_night_virtual_value(paths[i], &v) || tides_virtual_value(paths[i], &v))) {
            vals[i].value = v;
            vals[i].valid = true;
            vals[i].last_update_ms = now;
        }
    }
    uint32_t stale_ms = (uint32_t)s_cfg->settings.stale_s * 1000u;
    bool beep = false;

    dash_engine_lock();
    for (int p = 0; p < s_cfg->page_count; p++) {
        int count = dash_layout_tile_count(s_cfg->pages[p].layout);
        for (int i = 0; i < count; i++) {
            int idx = p * DASH_MAX_TILES + i;
            const DashTile *t = &s_cfg->pages[p].tiles[i];
            DashTileState *st = &s_state[idx];
            const PathValue &pv = vals[idx];
            if (!t->path[0]) continue;

            if (!pv.valid) {
                st->stale = true;
            } else {
                st->stale = (now - pv.last_update_ms) > stale_ms;
                float a = s_alpha[t->smooth & 3];
                if (dash_is_angle(t)) {
                    float rad = strcmp(t->sk_units, "deg") == 0 ? pv.value * 0.0174532925f : pv.value;
                    float cx = cosf(rad), sy = sinf(rad);
                    if (!st->has_value) { st->sx = cx; st->sy = sy; }
                    else { st->sx += a * (cx - st->sx); st->sy += a * (sy - st->sy); }
                    float sm = atan2f(st->sy, st->sx);
                    st->value_si = strcmp(t->sk_units, "deg") == 0 ? sm * 57.2957795f : sm;
                } else {
                    if (!st->has_value || dash_is_clock(t)) st->value_si = pv.value;
                    else st->value_si += a * (pv.value - st->value_si);
                }
                st->has_value = true;
            }

            if (!st->has_value || st->stale) {
                st->alarm = DASH_ALARM_NONE;
                st->acked = false;
                continue;
            }

            DashFormatted f;
            dash_format(t, st->value_si, &f);
            float d = f.display;

            if (!st->minmax_valid) {
                st->min_disp = st->max_disp = d;
                st->minmax_valid = true;
            } else {
                if (d < st->min_disp) st->min_disp = d;
                if (d > st->max_disp) st->max_disp = d;
            }

            uint32_t iv = s_trend_ms[t->trend % 5];
            if (iv && (int32_t)(now - st->next_sample_ms) >= 0) {
                st->hist[st->hist_head] = d;
                st->hist_head = (st->hist_head + 1) % DASH_TREND_POINTS;
                if (st->hist_count < DASH_TREND_POINTS) st->hist_count++;
                st->next_sample_ms = now + iv;
            }

            uint8_t alarm = DASH_ALARM_NONE;
            if (t->alarm_lo_en && d < t->alarm_lo) alarm = DASH_ALARM_LOW;
            if (t->alarm_hi_en && d > t->alarm_hi) alarm = DASH_ALARM_HIGH;
            if (alarm == DASH_ALARM_NONE) {
                st->acked = false;
            } else if (alarm != st->alarm) {
                st->acked = false;
                st->last_beep_ms = 0;
            }
            st->alarm = alarm;
            if (alarm && !st->acked && t->alarm_beep &&
                (st->last_beep_ms == 0 || now - st->last_beep_ms >= (uint32_t)BEEP_INTERVAL_MS)) {
                st->last_beep_ms = now;
                beep = true;
            }
        }
    }
    dash_engine_unlock();

    if (beep && s_beep_cb) s_beep_cb();
}

void dash_engine_init(dash_beep_cb_t beep_cb)
{
    if (s_cfg) return;
    s_beep_cb = beep_cb;
#ifndef SIMULATOR_BUILD
    s_lock = xSemaphoreCreateMutex();
#endif
    s_cfg   = (DashConfig *)calloc(1, sizeof(DashConfig));
    s_state = (DashTileState *)calloc(TOTAL_TILES, sizeof(DashTileState));
    s_subs  = (char (*)[96])calloc(STORE_MAX_ENTRIES, 96);
    reload_config();
    tides_init();
    lv_timer_create(tick, TICK_MS, nullptr);
}

const DashTileState *dash_engine_state(int page, int slot)
{
    if (!s_state || page < 0 || page >= DASH_MAX_PAGES || slot < 0 || slot >= DASH_MAX_TILES) {
        return nullptr;
    }
    return &s_state[page * DASH_MAX_TILES + slot];
}

const DashConfig *dash_engine_config(void) { return s_cfg; }

void dash_engine_reset_minmax(int page, int slot)
{
    DashTileState *st = (DashTileState *)dash_engine_state(page, slot);
    if (!st) return;
    dash_engine_lock();
    st->minmax_valid = false;
    dash_engine_unlock();
}

void dash_engine_ack(int page, int slot)
{
    DashTileState *st = (DashTileState *)dash_engine_state(page, slot);
    if (!st) return;
    dash_engine_lock();
    if (st->alarm) st->acked = true;
    dash_engine_unlock();
}

bool dash_engine_page_alarm(int page)
{
    if (!s_cfg || page < 0 || page >= s_cfg->page_count) return false;
    int n = dash_layout_tile_count(s_cfg->pages[page].layout);
    for (int i = 0; i < n; i++) {
        const DashTileState *st = dash_engine_state(page, i);
        if (st && st->alarm && !st->acked) return true;
    }
    return false;
}

