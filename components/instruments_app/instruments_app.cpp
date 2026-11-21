#include "instruments_app.hpp"
#include "dash_engine.hpp"
#include "dash_web.hpp"
#include "sk_catalog.hpp"
#include "day_night.hpp"
#include "tides.hpp"
#include "signalk_client.hpp"
#include "audio_feedback.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

static const char *TAG = "instruments";

LV_IMG_DECLARE(instruments_icon);

LV_FONT_DECLARE(lv_font_montserrat_360);
LV_FONT_DECLARE(lv_font_montserrat_310);
LV_FONT_DECLARE(lv_font_montserrat_120);   // 210 px
LV_FONT_DECLARE(lv_font_inst_176);
LV_FONT_DECLARE(lv_font_inst_144);
LV_FONT_DECLARE(lv_font_inst_116);
LV_FONT_DECLARE(lv_font_inst_92);
LV_FONT_DECLARE(lv_font_inst_72);
LV_FONT_DECLARE(lv_font_inst_56);
LV_FONT_DECLARE(lv_font_inst_44);
LV_FONT_DECLARE(lv_font_inst_label_36);   // Montserrat Bold, ASCII + degree
LV_FONT_DECLARE(lv_font_inst_label_30);

static const lv_font_t *const s_value_fonts[] = {
    &lv_font_montserrat_360, &lv_font_montserrat_310, &lv_font_montserrat_120,
    &lv_font_inst_176, &lv_font_inst_144, &lv_font_inst_116, &lv_font_inst_92,
    &lv_font_inst_72, &lv_font_inst_56, &lv_font_inst_44,
};
static const int VALUE_FONT_COUNT = sizeof(s_value_fonts) / sizeof(s_value_fonts[0]);

static const int HEADER_H   = 48;
static const int OV_HEAD_H  = 64;
static const int KB_H       = 300;
static const int TILE_GAP   = 6;
static const int TILE_TOP   = 50;     // label row height inside a tile
static const int MINMAX_H   = 30;
static const uint32_t CYCLE_IDLE_MS = 30000;
static const uint32_t SWITCH_IDLE_MS = 5000;   // anchor/sail jump waits for 5 s without touch

static const uint8_t s_cycle_opts[] = { 0, 5, 10, 15, 30, 60 };
static const uint8_t s_stale_opts[] = { 3, 5, 10, 20, 30, 60 };

enum { ACT_ADD = 1, ACT_DUP, ACT_DELETE, ACT_LEFT, ACT_RIGHT, ACT_RESET };

// ---------------------------------------------------------------------------
// Themes
// ---------------------------------------------------------------------------

struct Theme {
    uint32_t bg, tile, label, value, dim, accent, port, stbd, alarm, trend;
};

static const Theme s_themes[DASH_THEME_COUNT] = {
    // Dark
    { 0x000000, 0x11171d, 0xc8d4e0, 0xffffff, 0x3c4650, 0x3aa0ff, 0xff4040, 0x30d060, 0xd01818, 0x24527a },
    // Day
    { 0xc8d0d8, 0xffffff, 0x1e2830, 0x000000, 0xa8b2bc, 0x0060c0, 0xd00000, 0x008a20, 0xe02020, 0xa8c8e8 },
    // Night (red)
    { 0x000000, 0x0a0000, 0x9a2020, 0xc01818, 0x3a0808, 0xa01010, 0xc01818, 0x806010, 0x600000, 0x3a0808 },
    // Auto: resolved in theme_of()
    { 0x000000, 0x11171d, 0xc8d4e0, 0xffffff, 0x3c4650, 0x3aa0ff, 0xff4040, 0x30d060, 0xd01818, 0x24527a },
};

static const Theme &theme_of(const DashConfig *c);

// Copy `src` into `out`, trimming characters and appending "..." until it fits max_w
static void fit_single_line(const char *src, const lv_font_t *font, int max_w, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s", src);
    lv_point_t sz;
    lv_txt_get_size(&sz, out, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x <= max_w) return;
    size_t n = strlen(out);
    while (n > 0) {
        n--;
        while (n > 0 && out[n - 1] == ' ') n--;
        if (n + 4 > out_len) continue;
        memcpy(out + n, "...", 4);
        lv_txt_get_size(&sz, out, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= max_w) return;
    }
    snprintf(out, out_len, "...");
}

static const Theme &theme_of(const DashConfig *c)
{
    int t = c ? c->settings.theme % DASH_THEME_COUNT : 0;
    if (t == DASH_THEME_AUTO) t = day_night_is_night() ? DASH_THEME_DARK : DASH_THEME_DAY;
    return s_themes[t];
}

static uint32_t now_ms(void) { return lv_tick_get(); }

// Replace the degree sign (not in the UI fonts) for dropdown text
static void ascii_unit(const char *in, char *out, size_t len)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < len; ) {
        if ((uint8_t)in[i] == 0xC2 && (uint8_t)in[i + 1] == 0xB0) {
            const char *rep = isalpha((unsigned char)in[i + 2]) ? "deg " : "deg";
            for (const char *r = rep; *r && j + 1 < len; r++) out[j++] = *r;
            i += 2;
        } else {
            out[j++] = in[i++];
        }
    }
    out[j] = '\0';
}

static void no_style_box(lv_obj_t *o)
{
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

// ---------------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------------

InstrumentsApp::InstrumentsApp()
    : ESP_UI_PhoneApp("Instruments", &instruments_icon, true, false, false)
{
}

InstrumentsApp::~InstrumentsApp()
{
    free(_cfg);
    free(_edit);
}

bool InstrumentsApp::run()
{
    if (!_cfg) _cfg = (DashConfig *)calloc(1, sizeof(DashConfig));
    if (!_cfg) return false;

    _screen = lv_scr_act();
    lv_area_t vis = getVisualArea();
    _area_x = vis.x1;
    _area_y = vis.y1;
    _area_w = vis.x2 - vis.x1 + 1;
    _area_h = vis.y2 - vis.y1 + 1;

    dash_config_get(_cfg);
    _rev = dash_config_revision();
    int start = _cfg->settings.start_page >= 0 ? _cfg->settings.start_page : dash_last_page_get();
    _page = (start >= 0 && start < _cfg->page_count) ? start : 0;
    _last_cycle_ms = now_ms();

    build();
    sk_catalog_refresh(false);

    _timer = lv_timer_create(on_timer, 250, this);
    return true;
}

bool InstrumentsApp::back()
{
    if (_mbox) {
        lv_msgbox_close(_mbox);
        _mbox = nullptr;
        return true;
    }
    if (_pk.root)  { close_overlay(_pk); return true; }
    if (_ed.root)  { close_overlay(_ed); return true; }
    if (_mn.root)  { on_mn_cancel(nullptr); return true; }
    return notifyCoreClosed();
}

bool InstrumentsApp::close()
{
    if (_mbox) {
        lv_msgbox_close(_mbox);
        _mbox = nullptr;
    }
    free(_edit);
    _edit = nullptr;
    free_gfx_caches();
    // The default screen and timers created in run() are deleted by esp-ui
    _screen = _root = _title = _dots = _conn = _gear = _tv = nullptr;
    memset(_pages, 0, sizeof(_pages));
    memset(_tiles, 0, sizeof(_tiles));
    _ed = {};
    _pk = {};
    _mn = {};
    _timer = nullptr;
    return true;
}

bool InstrumentsApp::pause()
{
    if (_timer) lv_timer_pause(_timer);
    return true;
}

bool InstrumentsApp::resume()
{
    if (_timer) lv_timer_resume(_timer);
    return true;
}

bool InstrumentsApp::overlay_open() const
{
    return _ed.root || _pk.root || _mn.root || _mbox;
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

void InstrumentsApp::destroy_pages()
{
    if (_root) lv_obj_del(_root);
    free_gfx_caches();
    _root = _title = _dots = _conn = _gear = _tv = nullptr;
    memset(_pages, 0, sizeof(_pages));
    memset(_tiles, 0, sizeof(_tiles));
}

void InstrumentsApp::build()
{
    destroy_pages();
    const Theme &th = theme_of(_cfg);

    lv_obj_set_style_bg_color(_screen, lv_color_hex(th.bg), 0);
    lv_obj_set_style_bg_opa(_screen, LV_OPA_COVER, 0);

    _root = lv_obj_create(_screen);
    no_style_box(_root);
    lv_obj_set_pos(_root, _area_x, _area_y);
    lv_obj_set_size(_root, _area_w, _area_h);
    lv_obj_set_style_bg_color(_root, lv_color_hex(th.bg), 0);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, 0);
    lv_obj_move_background(_root);

    // Header: page name | page dots | connection + gear
    _title = lv_label_create(_root);
    lv_obj_set_style_text_font(_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_title, lv_color_hex(th.label), 0);
    lv_label_set_long_mode(_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_title, _area_w / 3);
    lv_obj_align(_title, LV_ALIGN_TOP_LEFT, 14, (HEADER_H - 28) / 2);

    _dots = lv_obj_create(_root);
    no_style_box(_dots);
    lv_obj_set_size(_dots, LV_SIZE_CONTENT, 16);
    lv_obj_set_flex_flow(_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(_dots, 10, 0);
    lv_obj_align(_dots, LV_ALIGN_TOP_MID, 0, (HEADER_H - 16) / 2);
    for (int p = 0; p < _cfg->page_count; p++) {
        lv_obj_t *d = lv_obj_create(_dots);
        lv_obj_set_size(d, 12, 12);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(th.dim), 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
    }

    _gear = lv_btn_create(_root);
    lv_obj_set_size(_gear, 64, HEADER_H - 4);
    lv_obj_align(_gear, LV_ALIGN_TOP_RIGHT, -4, 2);
    lv_obj_set_style_bg_opa(_gear, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(_gear, lv_color_hex(th.dim), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(_gear, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(_gear, 0, 0);
    // Larger touch target than the icon (moved to the foreground after the page view)
    lv_obj_set_ext_click_area(_gear, 24);
    lv_obj_t *gl = lv_label_create(_gear);
    lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(gl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(gl, lv_color_hex(th.label), 0);
    lv_obj_center(gl);
    lv_obj_add_event_cb(_gear, on_gear, LV_EVENT_CLICKED, this);

    _conn = lv_obj_create(_root);
    lv_obj_set_size(_conn, 14, 14);
    lv_obj_set_style_radius(_conn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(_conn, 0, 0);
    lv_obj_clear_flag(_conn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(_conn, LV_ALIGN_TOP_RIGHT, -76, (HEADER_H - 14) / 2);

    // Pages
    _tv = lv_tileview_create(_root);
    lv_obj_set_pos(_tv, 0, HEADER_H);
    lv_obj_set_size(_tv, _area_w, _area_h - HEADER_H);
    lv_obj_set_style_bg_opa(_tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(_tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(_tv, on_tv_changed, LV_EVENT_VALUE_CHANGED, this);

    for (int p = 0; p < _cfg->page_count; p++) {
        lv_dir_t dir = LV_DIR_NONE;
        if (p > 0) dir = (lv_dir_t)(dir | LV_DIR_LEFT);
        if (p < _cfg->page_count - 1) dir = (lv_dir_t)(dir | LV_DIR_RIGHT);
        _pages[p] = lv_tileview_add_tile(_tv, p, 0, dir);
        lv_obj_clear_flag(_pages[p], LV_OBJ_FLAG_SCROLLABLE);
        build_page(p);
    }

    if (_page >= _cfg->page_count) _page = _cfg->page_count - 1;
    if (_page < 0) _page = 0;
    lv_obj_set_tile_id(_tv, _page, 0, LV_ANIM_OFF);
    // The gear's extended touch area reaches into the first tile row
    lv_obj_move_foreground(_gear);

    for (int p = 0; p < _cfg->page_count; p++) {
        int n = dash_layout_tile_count(_cfg->pages[p].layout);
        for (int i = 0; i < n; i++) update_tile(p, i, p == _page);
    }
    update_header();
    refresh_gfx_caches();
}

void InstrumentsApp::build_page(int p)
{
    const Theme &th = theme_of(_cfg);
    const DashPage &pg = _cfg->pages[p];
    const DashCell *cells = dash_layout_cells(pg.layout);
    int n = dash_layout_tile_count(pg.layout);
    int W = _area_w, H = _area_h - HEADER_H;

    for (int i = 0; i < n; i++) {
        TileUi &u = _tiles[p][i];
        const DashTile &t = pg.tiles[i];
        const DashCell &c = cells[i];
        int x0 = c.x * W / DASH_GRID, x1 = (c.x + c.w) * W / DASH_GRID;
        int y0 = c.y * H / DASH_GRID, y1 = (c.y + c.h) * H / DASH_GRID;
        u.w = x1 - x0 - TILE_GAP;
        u.h = y1 - y0 - TILE_GAP;
        u.font_idx = -1;
        u.color = 0xFFFFFFFF;

        u.cont = lv_obj_create(_pages[p]);
        lv_obj_set_pos(u.cont, x0 + TILE_GAP / 2, y0 + TILE_GAP / 2);
        lv_obj_set_size(u.cont, u.w, u.h);
        lv_obj_set_style_pad_all(u.cont, 0, 0);
        lv_obj_set_style_border_width(u.cont, 0, 0);
        lv_obj_set_style_radius(u.cont, 8, 0);
        lv_obj_set_style_bg_color(u.cont, lv_color_hex(th.tile), 0);
        lv_obj_set_style_bg_opa(u.cont, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(u.cont, 0, 0);
        lv_obj_clear_flag(u.cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(u.cont, (void *)(intptr_t)(p * DASH_MAX_TILES + i));
        lv_obj_add_event_cb(u.cont, on_tile_event, LV_EVENT_SHORT_CLICKED, this);
        lv_obj_add_event_cb(u.cont, on_tile_event, LV_EVENT_LONG_PRESSED, this);
        bool graphic = t.path[0] && dash_is_graphic(t.path);

        // Trend sparkline sits behind the value
        if (t.path[0] && t.trend && !graphic) {
            u.chart = lv_chart_create(u.cont);
            int ch = u.h * 45 / 100;
            lv_obj_set_size(u.chart, u.w - 8, ch);
            lv_obj_align(u.chart, LV_ALIGN_BOTTOM_MID, 0, -4);
            lv_obj_set_style_bg_opa(u.chart, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(u.chart, 0, 0);
            lv_obj_set_style_pad_all(u.chart, 0, 0);
            lv_obj_set_style_size(u.chart, 0, LV_PART_INDICATOR);
            lv_obj_set_style_line_width(u.chart, 3, LV_PART_ITEMS);
            lv_obj_clear_flag(u.chart, LV_OBJ_FLAG_CLICKABLE);
            lv_chart_set_type(u.chart, LV_CHART_TYPE_LINE);
            lv_chart_set_div_line_count(u.chart, 0, 0);
            lv_chart_set_point_count(u.chart, DASH_TREND_POINTS);
            lv_chart_set_range(u.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
            u.series = lv_chart_add_series(u.chart, lv_color_hex(th.trend), LV_CHART_AXIS_PRIMARY_Y);
            lv_chart_set_all_value(u.chart, u.series, LV_CHART_POINT_NONE);
            u.hist_sig = 0xFFFF;
        }

        // Bold title in whatever width the unit label leaves free: 36 px, then
        // 30 px, then 30 px with an ellipsis (never below 30 px).
        const char *title = t.path[0] ? t.label : "";
        int unit_w = 0;
        if (t.path[0] && !graphic) {
            DashFormatted uf;
            dash_format(&t, 0.0f, &uf);
            lv_point_t us;
            const char *probe = dash_effective_angle_style(&t) == DASH_ANGLE_PS ? "S" : uf.unit;
            lv_txt_get_size(&us, probe, &lv_font_inst_label_36, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (us.x > 0) unit_w = us.x + 12;
        }
        int name_w = u.w - 20 - unit_w;
        if (name_w < 40) name_w = 40;
        const lv_font_t *name_font = &lv_font_inst_label_36;
        lv_point_t ts;
        lv_txt_get_size(&ts, title, name_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (ts.x > name_w) name_font = &lv_font_inst_label_30;
        char title_fit[64];
        fit_single_line(title, name_font, name_w, title_fit, sizeof(title_fit));

        u.name = lv_label_create(u.cont);
        lv_obj_set_style_text_font(u.name, name_font, 0);
        lv_obj_set_style_text_color(u.name, lv_color_hex(th.label), 0);
        lv_label_set_long_mode(u.name, LV_LABEL_LONG_CLIP);
        lv_obj_set_size(u.name, name_w, lv_font_get_line_height(name_font));
        lv_obj_align(u.name, LV_ALIGN_TOP_LEFT, 10, name_font == &lv_font_inst_label_30 ? 9 : 6);
        lv_label_set_text(u.name, title_fit);

        u.unit = lv_label_create(u.cont);
        lv_obj_set_style_text_font(u.unit, &lv_font_inst_label_36, 0);
        lv_obj_set_style_text_color(u.unit, lv_color_hex(th.label), 0);
        lv_obj_align(u.unit, LV_ALIGN_TOP_RIGHT, -10, 6);
        lv_label_set_text(u.unit, "");

        u.value = lv_label_create(u.cont);
        lv_obj_set_style_text_color(u.value, lv_color_hex(th.value), 0);
        lv_obj_set_style_text_align(u.value, LV_TEXT_ALIGN_CENTER, 0);
        if (t.path[0]) {
            lv_label_set_text(u.value, "");
        } else {
            lv_obj_set_style_text_font(u.value, &lv_font_montserrat_24, 0);
            lv_obj_set_style_text_color(u.value, lv_color_hex(th.dim), 0);
            lv_label_set_long_mode(u.value, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(u.value, u.w - 16);
            lv_label_set_text(u.value, _cfg->settings.locked ? "Empty"
                                       : u.w < 300 ? "Hold to assign" : "Hold to assign data");
            lv_obj_center(u.value);
        }

        u.minmax = lv_label_create(u.cont);
        lv_obj_set_style_text_font(u.minmax, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(u.minmax, lv_color_hex(th.label), 0);
        lv_obj_align(u.minmax, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_label_set_text(u.minmax, "");
        lv_obj_add_flag(u.minmax, LV_OBJ_FLAG_HIDDEN);

        if (graphic) {
            lv_obj_add_flag(u.value, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(u.unit, LV_OBJ_FLAG_HIDDEN);
            u.gfx = lv_obj_create(u.cont);
            no_style_box(u.gfx);
            lv_obj_set_style_pad_all(u.gfx, 0, 0);
            lv_obj_set_size(u.gfx, u.w, u.h);
            lv_obj_set_pos(u.gfx, 0, 0);
            lv_obj_clear_flag(u.gfx, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(u.gfx, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_user_data(u.gfx, (void *)(intptr_t)(p * DASH_MAX_TILES + i));
            lv_obj_add_event_cb(u.gfx, on_gfx_draw, LV_EVENT_DRAW_MAIN, this);
            // Behind the title so a pre-rendered image can't cover it
            lv_obj_move_to_index(u.gfx, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

void InstrumentsApp::update_header()
{
    if (!_root) return;
    const Theme &th = theme_of(_cfg);
    const char *name = _cfg->pages[_page].name;
    if (strcmp(lv_label_get_text(_title), name) != 0) lv_label_set_text(_title, name);

    for (int p = 0; p < _cfg->page_count; p++) {
        lv_obj_t *d = lv_obj_get_child(_dots, p);
        if (!d) break;
        uint32_t c = th.dim;
        if (dash_engine_page_alarm(p) && (_tick / 2) % 2 == 0) c = th.alarm;
        else if (p == _page) c = th.accent;
        lv_obj_set_style_bg_color(d, lv_color_hex(c), 0);
    }
    bool conn = signalk_client_is_connected();
    lv_obj_set_style_bg_color(_conn, lv_color_hex(conn ? th.stbd : th.port), 0);
}

void InstrumentsApp::fit_value(TileUi &u, const char *text)
{
    char pattern[16];
    size_t j = 0;
    for (const char *c = text; *c && j + 1 < sizeof(pattern); c++) {
        pattern[j++] = isdigit((unsigned char)*c) ? '8' : *c;
    }
    pattern[j] = '\0';

    int top = TILE_TOP;
    int bottom = u.h - (u.show_minmax ? MINMAX_H + 6 : 8);
    // Keep the digits above the sparkline (chart is bottom aligned, see build())
    if (u.chart) {
        int chart_top = u.h - u.h * 45 / 100 - 4;
        if (bottom > chart_top - 4) bottom = chart_top - 4;
    }
    int avail_h = bottom - top;
    int avail_w = u.w - 16;
    if (u.value_top == top && u.value_h == avail_h && u.font_idx >= 0 &&
        strcmp(pattern, u.pattern) == 0) {
        return;
    }
    snprintf(u.pattern, sizeof(u.pattern), "%s", pattern);
    u.value_top = top;
    u.value_h = avail_h;

    int idx = VALUE_FONT_COUNT - 1;
    lv_font_glyph_dsc_t g;
    for (int i = 0; i < VALUE_FONT_COUNT; i++) {
        const lv_font_t *f = s_value_fonts[i];
        lv_point_t sz;
        lv_txt_get_size(&sz, pattern, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (!lv_font_get_glyph_dsc(f, &g, '8', 0)) continue;
        // The largest fonts carry digits only (no ':' or '/'); skip them for HH:MM and DD/MM
        if ((strchr(pattern, ':') && !lv_font_get_glyph_dsc(f, &g, ':', 0)) ||
            (strchr(pattern, '/') && !lv_font_get_glyph_dsc(f, &g, '/', 0))) continue;
        lv_font_get_glyph_dsc(f, &g, '8', 0);
        if (sz.x <= avail_w && g.box_h <= avail_h) { idx = i; break; }
    }
    const lv_font_t *f = s_value_fonts[idx];
    u.font_idx = idx;
    lv_obj_set_style_text_font(u.value, f, 0);

    // Centre the digit glyphs (not the full line box) in the value area
    lv_font_get_glyph_dsc(f, &g, '8', 0);
    int baseline = f->line_height - f->base_line;
    int digit_top = baseline - g.ofs_y - g.box_h;
    int y = top + (avail_h - g.box_h) / 2 - digit_top;
    lv_obj_align(u.value, LV_ALIGN_TOP_MID, 0, y);
}

void InstrumentsApp::update_chart(TileUi &u, const DashTile &t, const DashTileState *st)
{
    if (!u.chart || !u.series || !st) return;
    uint16_t sig = (uint16_t)(st->hist_head | (st->hist_count << 8));
    if (sig == u.hist_sig) return;
    u.hist_sig = sig;

    int n = st->hist_count;
    float lo = 0, hi = 0;
    for (int k = 0; k < n; k++) {
        int idx = (st->hist_head - n + k + DASH_TREND_POINTS) % DASH_TREND_POINTS;
        float v = st->hist[idx];
        if (k == 0 || v < lo) lo = v;
        if (k == 0 || v > hi) hi = v;
    }
    float span = hi - lo;
    float min_span = fabsf(hi) * 0.02f + 0.1f;
    if (span < min_span) {
        float mid = (hi + lo) / 2;
        lo = mid - min_span / 2;
        span = min_span;
    }
    lv_coord_t *ys = lv_chart_get_y_array(u.chart, u.series);
    int offset = DASH_TREND_POINTS - n;
    for (int k = 0; k < DASH_TREND_POINTS; k++) {
        if (k < offset) { ys[k] = LV_CHART_POINT_NONE; continue; }
        int idx = (st->hist_head - n + (k - offset) + DASH_TREND_POINTS) % DASH_TREND_POINTS;
        ys[k] = (lv_coord_t)(50 + 900 * (st->hist[idx] - lo) / span);
    }
    lv_chart_refresh(u.chart);
    (void)t;
}

// ---------------------------------------------------------------------------
// Wind rose data: sampled every timer tick and smoothed (angles as vectors)
// ---------------------------------------------------------------------------

enum { W_AWA = 0, W_TWA, W_HDG, W_AWS, W_TWS, W_COUNT };

struct WindNow {
    bool  ok[W_COUNT];
    float x[3], y[3];      // smoothed unit vectors for AWA, TWA, heading
    float spd[2];          // smoothed AWS, TWS (m/s)
    uint32_t sig;
};
static WindNow s_wind;
static const char *const s_wind_paths[] = {
    "environment.wind.angleApparent", "environment.wind.angleTrueWater",
    "navigation.headingMagnetic", "navigation.headingTrue",
    "environment.wind.speedApparent", "environment.wind.speedTrue",
};

static float wind_deg(int k) { return atan2f(s_wind.y[k], s_wind.x[k]) * 57.29578f; }

static void wind_sample(int stale_s)
{
    PathValue v[6];
    if (!signalk_client_get_values(s_wind_paths, 6, v)) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t max_age = (uint32_t)(stale_s > 0 ? stale_s : 10) * 1000u;
    auto fresh = [&](int j) { return v[j].valid && now - v[j].last_update_ms <= max_age; };
    const float A = 0.35f;   // per 250 ms tick

    const int ang_src[3] = { 0, 1, fresh(2) ? 2 : 3 };
    for (int k = 0; k < 3; k++) {
        int j = ang_src[k];
        bool ok = fresh(j);
        if (ok) {
            float nx = cosf(v[j].value), ny = sinf(v[j].value);
            if (!s_wind.ok[k]) { s_wind.x[k] = nx; s_wind.y[k] = ny; }
            else { s_wind.x[k] += A * (nx - s_wind.x[k]); s_wind.y[k] += A * (ny - s_wind.y[k]); }
        }
        s_wind.ok[k] = ok;
    }
    for (int k = 0; k < 2; k++) {
        int j = 4 + k;
        bool ok = fresh(j);
        if (ok) s_wind.spd[k] = s_wind.ok[W_AWS + k] ? s_wind.spd[k] + A * (v[j].value - s_wind.spd[k]) : v[j].value;
        s_wind.ok[W_AWS + k] = ok;
    }

    uint32_t h = 2166136261u;
    auto mix = [&](int32_t q) { h = (h ^ (uint32_t)q) * 16777619u; };
    for (int k = 0; k < 3; k++) mix(s_wind.ok[k] ? (int32_t)lroundf(wind_deg(k)) : 9999);
    for (int k = 0; k < 2; k++) mix(s_wind.ok[W_AWS + k] ? (int32_t)lroundf(s_wind.spd[k] * 10.0f) : -1);
    s_wind.sig = h;
}

static bool is_wind_rose(const DashTile &t) { return strcmp(t.path, "panel.wind.rose") == 0; }

static int decimals_of(const char *v)
{
    const char *dot = strchr(v, '.');
    if (!dot) return 0;
    int n = 0;
    for (const char *c = dot + 1; isdigit((unsigned char)*c); c++) n++;
    return n;
}

void InstrumentsApp::update_tile(int p, int i, bool visible)
{
    TileUi &u = _tiles[p][i];
    const DashTile &t = _cfg->pages[p].tiles[i];
    if (!u.cont || !t.path[0]) return;
    if (u.gfx) {
        // Graphic tiles are pre-rendered by refresh_gfx_caches(); the wind rose follows
        // the (smoothed) wind, the others only change slowly (every 15 s)
        uint32_t sig = is_wind_rose(t) ? s_wind.sig : (uint32_t)(time(nullptr) / 15);
        if (u.cache_fail && visible && sig != u.gfx_sig) lv_obj_invalidate(u.gfx);
        u.gfx_sig = sig;
        return;
    }
    const Theme &th = theme_of(_cfg);

    const DashTileState *st = dash_engine_state(p, i);
    bool valid = st && st->has_value && !st->stale && strcmp(st->path, t.path) == 0;

    DashFormatted f;
    if (valid) {
        dash_format(&t, st->value_si, &f);
    } else {
        dash_format(&t, 0.0f, &f);
        snprintf(f.value, sizeof(f.value), "---");
        if (dash_effective_angle_style(&t) == DASH_ANGLE_PS) f.unit[0] = '\0';
        f.side = 0;
    }

    if (strcmp(lv_label_get_text(u.unit), f.unit) != 0) lv_label_set_text(u.unit, f.unit);
    if (strcmp(u.text, f.value) != 0) {
        snprintf(u.text, sizeof(u.text), "%s", f.value);
        lv_label_set_text(u.value, f.value);
    }
    fit_value(u, f.value);

    bool alarm = valid && st->alarm;
    bool flash = alarm && !st->acked && (_tick / 2) % 2 == 0;
    uint32_t vc = th.value;
    if (!valid)               vc = th.dim;
    else if (alarm && !flash) vc = th.alarm;
    else if (flash)           vc = 0xffffff;
    else if (f.side < 0)      vc = th.port;
    else if (f.side > 0)      vc = th.stbd;
    if (vc != u.color) {
        u.color = vc;
        lv_obj_set_style_text_color(u.value, lv_color_hex(vc), 0);
    }
    if (flash != u.flash) {
        u.flash = flash;
        lv_obj_set_style_bg_color(u.cont, lv_color_hex(flash ? th.alarm : th.tile), 0);
        lv_obj_set_style_text_color(u.name, lv_color_hex(flash ? 0xffffff : th.label), 0);
        lv_obj_set_style_text_color(u.unit, lv_color_hex(flash ? 0xffffff : th.label), 0);
    }

    if (u.show_minmax) {
        char mm[48];
        if (st && st->minmax_valid) {
            int dec = decimals_of(f.value);
            snprintf(mm, sizeof(mm), "Lo %.*f   Hi %.*f", dec, st->min_disp, dec, st->max_disp);
        } else {
            snprintf(mm, sizeof(mm), "Lo ---   Hi ---");
        }
        if (strcmp(lv_label_get_text(u.minmax), mm) != 0) lv_label_set_text(u.minmax, mm);
    }

    if (visible && t.trend) update_chart(u, t, st);
}

void InstrumentsApp::go_to_page(int p, bool anim)
{
    if (!_tv || p < 0 || p >= _cfg->page_count) return;
    _page = p;
    lv_obj_set_tile_id(_tv, p, 0, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    update_header();
}

void InstrumentsApp::on_timer(lv_timer_t *timer)
{
    InstrumentsApp *app = (InstrumentsApp *)timer->user_data;
    if (!app->_root) return;
    app->_tick++;

    // Configuration changed (web editor, first-run defaults ...)
    if (dash_config_revision() != app->_rev && !app->overlay_open()) {
        dash_config_get(app->_cfg);
        app->_rev = dash_config_revision();
        app->build();
        return;
    }
    // Auto theme follows dawn/dusk
    bool night = day_night_is_night();
    bool auto_theme = app->_cfg->settings.theme == DASH_THEME_AUTO;
    if (night != app->_night && !(auto_theme && app->overlay_open())) {
        app->_night = night;
        if (auto_theme) {
            app->build();
            return;
        }
    }

    wind_sample(app->_cfg->settings.stale_s);
    for (int p = 0; p < app->_cfg->page_count; p++) {
        int n = dash_layout_tile_count(app->_cfg->pages[p].layout);
        for (int i = 0; i < n; i++) app->update_tile(p, i, p == app->_page);
    }
    app->refresh_gfx_caches();
    app->update_header();

    if (app->_pk.root && (app->_pk_dirty || sk_catalog_generation() != app->_pk_gen ||
                          (sk_catalog_state() == SK_CATALOG_LOADING && app->_tick % 4 == 0))) {
        app->fill_picker();
    }

    // Automatic anchor / sail switching (deferred while the user is interacting)
    const DashSettings &s = app->_cfg->settings;
    uint32_t now = now_ms();
    DashMotion motion = {};
    if (s.auto_switch) dash_engine_motion(&motion);
    if (motion.enabled && motion.seq != app->_motion_seq && !app->overlay_open() &&
        lv_disp_get_inactive_time(NULL) >= SWITCH_IDLE_MS) {
        app->_motion_seq = motion.seq;
        int target = dash_group_first_page(app->_cfg, motion.state);
        if (target >= 0 && app->_cfg->pages[app->_page].group != motion.state) {
            app->go_to_page(target, true);
            app->_last_cycle_ms = now;
        }
    }

    // Automatic page cycling (paused while the user is interacting)
    if (s.cycle_s && app->_cfg->page_count > 1 && !app->overlay_open() &&
        lv_disp_get_inactive_time(NULL) >= CYCLE_IDLE_MS) {
        if (now - app->_last_cycle_ms >= (uint32_t)s.cycle_s * 1000u) {
            app->_last_cycle_ms = now;
            int n = app->_cfg->page_count, next = (app->_page + 1) % n;
            // Stay within the active group if it has pages
            if (s.cycle_group && motion.enabled && dash_group_first_page(app->_cfg, motion.state) >= 0) {
                for (int k = 1; k <= n; k++) {
                    int q = (app->_page + k) % n;
                    if (app->_cfg->pages[q].group == motion.state) { next = q; break; }
                }
            }
            if (next != app->_page) app->go_to_page(next, true);
        }
    } else {
        app->_last_cycle_ms = now;
    }
}

void InstrumentsApp::on_tv_changed(lv_event_t *e)
{
    InstrumentsApp *app = (InstrumentsApp *)lv_event_get_user_data(e);
    lv_obj_t *act = lv_tileview_get_tile_act(app->_tv);
    for (int p = 0; p < app->_cfg->page_count; p++) {
        if (app->_pages[p] == act) {
            app->_page = p;
            break;
        }
    }
    // Remember the page only when the user swiped (not for auto cycling)
    if (lv_disp_get_inactive_time(NULL) < 2000) dash_last_page_set(app->_page);
    app->update_header();
}

// ---------------------------------------------------------------------------
// Custom-drawn tiles: tide curve and solunar
// ---------------------------------------------------------------------------

struct Gfx {
    lv_draw_ctx_t *dc;
    lv_area_t      a;
};

static void g_rect(Gfx &g, int x1, int y1, int x2, int y2, uint32_t col,
                   lv_opa_t opa = LV_OPA_COVER, int radius = 0)
{
    if (x2 < x1 || y2 < y1) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(col);
    d.bg_opa = opa;
    d.radius = radius;
    lv_area_t r = { (lv_coord_t)(g.a.x1 + x1), (lv_coord_t)(g.a.y1 + y1),
                    (lv_coord_t)(g.a.x1 + x2), (lv_coord_t)(g.a.y1 + y2) };
    lv_draw_rect(g.dc, &d, &r);
}

static void g_line(Gfx &g, int x1, int y1, int x2, int y2, uint32_t col, int w,
                   lv_opa_t opa = LV_OPA_COVER)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(col);
    d.width = w;
    d.opa = opa;
    d.round_start = d.round_end = 1;
    lv_point_t p1 = { (lv_coord_t)(g.a.x1 + x1), (lv_coord_t)(g.a.y1 + y1) };
    lv_point_t p2 = { (lv_coord_t)(g.a.x1 + x2), (lv_coord_t)(g.a.y1 + y2) };
    lv_draw_line(g.dc, &d, &p1, &p2);
}

static int text_w(const char *txt, const lv_font_t *f)
{
    lv_point_t sz;
    lv_txt_get_size(&sz, txt, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

// x is the left edge, centre or right edge depending on `al`
static void g_text(Gfx &g, int x, int y, const char *txt, const lv_font_t *f, uint32_t col,
                   lv_text_align_t al = LV_TEXT_ALIGN_LEFT)
{
    lv_point_t sz;
    lv_txt_get_size(&sz, txt, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int x0 = al == LV_TEXT_ALIGN_CENTER ? x - sz.x / 2 : al == LV_TEXT_ALIGN_RIGHT ? x - sz.x : x;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = lv_color_hex(col);
    d.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t r = { (lv_coord_t)(g.a.x1 + x0), (lv_coord_t)(g.a.y1 + y),
                    (lv_coord_t)(g.a.x1 + x0 + sz.x + 1), (lv_coord_t)(g.a.y1 + y + sz.y) };
    lv_draw_label(g.dc, &d, &r, txt, nullptr);
}

static void fmt_hm(time_t t, char *buf, size_t n)
{
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(buf, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

// Height in the tile's units, e.g. "2.8 m"
static void fmt_height(const DashTile &t, float h, char *buf, size_t n)
{
    DashFormatted f;
    dash_format(&t, h, &f);
    snprintf(buf, n, "%s %s", f.value, f.unit);
}

static void draw_tide_curve(Gfx &g, const Theme &th, const DashTile &t, int W, int H, int title_w)
{
    const bool big = W >= 420 && H >= 260;
    const lv_font_t *fs = big ? &lv_font_montserrat_24 : &lv_font_montserrat_16;
    const lv_font_t *fl = big ? &lv_font_montserrat_36 : &lv_font_montserrat_24;
    const int lh = lv_font_get_line_height(fs);
    char buf[128], buf2[64];

    TideInfo ti;
    tides_get_info(&ti);

    // Port name in the title row
    if (ti.port[0]) {
        if (ti.distance_m >= 0) snprintf(buf, sizeof(buf), "%s %.1f NM", ti.port, ti.distance_m / 1852.0f);
        else                    snprintf(buf, sizeof(buf), "%s", ti.port);
        int max_w = W - title_w - 34;
        if (max_w > 40) {
            fit_single_line(buf, fs, max_w, buf2, sizeof(buf2));
            g_text(g, W - 10, big ? 14 : 16, buf2, fs, th.label, LV_TEXT_ALIGN_RIGHT);
        }
    }

    const char *msg = nullptr;
    if (!ti.loaded)          msg = "No tide table\nUpload it in the web editor";
    else if (!ti.port[0])    msg = "No tide port within 50 NM\n(needs a GPS position)";
    else if (!ti.valid)      msg = "No tide data for today";
    if (msg) {
        g_text(g, W / 2, H / 2 - lh, msg, fs, th.dim, LV_TEXT_ALIGN_CENTER);
        return;
    }

    const time_t now = time(nullptr);
    if (!ti.heights) {
        char a[16], b[16];
        fmt_hm(ti.next_high.t, a, sizeof(a));
        fmt_hm(ti.next_low.t, b, sizeof(b));
        snprintf(buf, sizeof(buf), "Times only\nHW %s   LW %s", a, b);
        g_text(g, W / 2, H / 2 - lh, buf, fl, th.value, LV_TEXT_ALIGN_CENTER);
        return;
    }

    // Current height, trend and range
    int y = TILE_TOP;
    fmt_height(t, ti.height, buf, sizeof(buf));
    g_text(g, 12, y, buf, fl, th.value);
    int x = 12 + text_w(buf, fl) + 14;
    DashFormatted f;
    dash_format(&t, 1.0f, &f);
    bool feet = strcmp(f.unit, "ft") == 0;
    float rate = ti.rate_mh * (feet ? 3.28084f : 1.0f);
    snprintf(buf, sizeof(buf), "%s %.2f %s/h",
             fabsf(rate) < 0.005f ? LV_SYMBOL_MINUS : rate > 0 ? LV_SYMBOL_UP : LV_SYMBOL_DOWN,
             fabsf(rate), feet ? "ft" : "m");
    int fy = y + (lv_font_get_line_height(fl) - lh) / 2;
    g_text(g, x, fy, buf, fs, th.label);
    if (big) {
        fmt_height(t, ti.range, buf2, sizeof(buf2));
        snprintf(buf, sizeof(buf), "Range %s%s", buf2, ti.baro ? "  (baro)" : "");
        g_text(g, W - 12, fy, buf, fs, th.label, LV_TEXT_ALIGN_RIGHT);
    }

    // Plot area
    const int px0 = 10, px1 = W - 10;
    const int py0 = y + lv_font_get_line_height(fl) + 6;
    const int py1 = H - lh - 8;
    const int cy0 = py0 + lh + 6, cy1 = py1 - lh - 6;   // curve band (room for event labels)
    if (px1 - px0 < 60 || cy1 - cy0 < 20) return;

    const time_t from = now - 12 * 3600, to = now + 12 * 3600;
    float hs[256];
    int n = (px1 - px0) / 3 + 1;
    if (n > 256) n = 256;
    if (!tides_curve(from, to, n, hs)) return;
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < n; i++) {
        if (isnan(hs[i])) continue;
        lo = fminf(lo, hs[i]);
        hi = fmaxf(hi, hs[i]);
    }
    if (lo > hi) return;
    if (hi - lo < 0.5f) {
        float c = (hi + lo) / 2;
        lo = c - 0.25f;
        hi = c + 0.25f;
    }
    auto ypos = [&](float h) { return cy1 - (int)lroundf((h - lo) / (hi - lo) * (cy1 - cy0)); };
    auto xpos = [&](time_t tt) { return px0 + (int)((double)(tt - from) / (double)(to - from) * (px1 - px0)); };

    // Hour grid in local time
    const int step = big ? 3 : 6;
    struct tm lt;
    localtime_r(&from, &lt);
    lt.tm_min = lt.tm_sec = 0;
    lt.tm_hour = (lt.tm_hour / step + 1) * step;
    lt.tm_isdst = -1;
    for (time_t tt = mktime(&lt); tt < to; tt += step * 3600) {
        int gx = xpos(tt);
        g_line(g, gx, py0, gx, py1, th.dim, 1, LV_OPA_50);
        struct tm l2;
        localtime_r(&tt, &l2);
        snprintf(buf, sizeof(buf), "%02d:00", l2.tm_hour);
        if (gx - text_w(buf, fs) / 2 > px0 && gx + text_w(buf, fs) / 2 < px1) {
            g_text(g, gx, py1 + 4, buf, fs, th.label, LV_TEXT_ALIGN_CENTER);
        }
    }

    // Filled curve
    int prev_x = -1, prev_y = 0;
    for (int i = 0; i < n; i++) {
        if (isnan(hs[i])) { prev_x = -1; continue; }
        int cx = px0 + i * (px1 - px0) / (n - 1);
        int nx = i + 1 < n ? px0 + (i + 1) * (px1 - px0) / (n - 1) : cx + 1;
        int cy = ypos(hs[i]);
        g_rect(g, cx, cy, nx - 1, py1, th.trend, LV_OPA_70);
        if (prev_x >= 0) g_line(g, prev_x, prev_y, cx, cy, th.accent, 3);
        prev_x = cx;
        prev_y = cy;
    }
    g_line(g, px0, py1, px1, py1, th.dim, 2);

    // High / low water markers
    TideEvent ev[8];
    int ne = tides_events(from, to, ev, 8);
    for (int i = 0; i < ne; i++) {
        int ex = xpos(ev[i].t), ey = ypos(ev[i].h);
        g_rect(g, ex - 5, ey - 5, ex + 5, ey + 5, th.value, LV_OPA_COVER, LV_RADIUS_CIRCLE);
        char tm_s[16];
        fmt_hm(ev[i].t, tm_s, sizeof(tm_s));
        fmt_height(t, ev[i].h, buf2, sizeof(buf2));
        snprintf(buf, sizeof(buf), "%s %s", tm_s, buf2);
        int tw = text_w(buf, fs);
        int lx = ex;
        if (lx - tw / 2 < px0) lx = px0 + tw / 2;
        if (lx + tw / 2 > px1) lx = px1 - tw / 2;
        int ly = ev[i].high ? ey - lh - 6 : ey + 8;
        g_text(g, lx, ly, buf, fs, th.label, LV_TEXT_ALIGN_CENTER);
    }

    // Now
    int nx = xpos(now), ny = ypos(ti.height);
    g_line(g, nx, py0, nx, py1, th.value, 2, LV_OPA_60);
    g_rect(g, nx - 8, ny - 8, nx + 8, ny + 8, th.value, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    g_rect(g, nx - 4, ny - 4, nx + 4, ny + 4, th.accent, LV_OPA_COVER, LV_RADIUS_CIRCLE);
}

// Sky colours per SkyState (night .. day) and the sun colour, per theme
static void sun_palette(const Theme &th, uint32_t sky[6], uint32_t *sun)
{
    bool day_theme = th.tile >= 0xe00000;
    bool red = th.value == 0xc01818;
    static const uint32_t s_dark[6] = { 0x11171d, 0x14233a, 0x1b3458, 0x2a4c7c, 0x6a5630, 0x3a6aa4 };
    static const uint32_t s_day[6]  = { 0x56657a, 0x7686a0, 0x98abc6, 0xbccfe6, 0xf2deb0, 0xdcecfc };
    static const uint32_t s_red[6]  = { 0x0a0000, 0x160303, 0x220505, 0x320707, 0x4a0c0c, 0x420a0a };
    const uint32_t *src = red ? s_red : day_theme ? s_day : s_dark;
    for (int i = 0; i < 6; i++) sky[i] = src[i];
    if (!red && !day_theme) sky[0] = th.tile;
    *sun = red ? th.value : day_theme ? 0xd08000 : 0xffc030;
}

// Samples of f over [from, from + 24 h], cached per minute and position
struct CurveCache { float v[400]; time_t from; int n; double lat, lon; };

static bool cached_curve(CurveCache &c, float (*f)(double, double, time_t), double lat, double lon,
                         time_t from, int n, float *out)
{
    if (n < 2 || n > 400) return false;
    if (c.from != from || c.n != n || c.lat != lat || c.lon != lon) {
        for (int i = 0; i < n; i++) c.v[i] = f(lat, lon, from + (time_t)((int64_t)i * 86400 / (n - 1)));
        c.from = from;
        c.n = n;
        c.lat = lat;
        c.lon = lon;
    }
    memcpy(out, c.v, sizeof(float) * n);
    return true;
}

// Background bands by sun sky state; es = sun elevation samples spanning px0..px1
static void draw_sky_bands(Gfx &g, const uint32_t sky[6], const float *es, int n, int px0, int px1, int py0, int py1)
{
    auto xcol = [&](int i) { return px0 + i * (px1 - px0) / (n - 1); };
    int run = 0, rs = day_night_sky_state(es[0]);
    for (int i = 1; i <= n; i++) {
        int st = i < n ? day_night_sky_state(es[i]) : -1;
        if (st == rs) continue;
        g_rect(g, xcol(run), py0, i < n ? xcol(i) - 1 : px1, py1, sky[rs]);
        run = i;
        rs = st;
    }
}

// Local-time hour grid with labels below py1; the step widens until labels fit
static void draw_hour_grid(Gfx &g, const Theme &th, time_t from, time_t to, int px0, int px1, int py0, int py1,
                           const lv_font_t *fs)
{
    char buf[16];
    const int lab_w = text_w("00:00", fs) + 14;
    int step = 3;
    while (step < 12 && (px1 - px0) * step / 24 < lab_w) step *= 2;
    struct tm lt;
    localtime_r(&from, &lt);
    lt.tm_min = lt.tm_sec = 0;
    lt.tm_hour = (lt.tm_hour / step + 1) * step;
    lt.tm_isdst = -1;
    for (time_t tt = mktime(&lt); tt < to; tt += step * 3600) {
        int gx = px0 + (int)((double)(tt - from) / (double)(to - from) * (px1 - px0));
        g_line(g, gx, py0, gx, py1, th.dim, 1, LV_OPA_50);
        struct tm l2;
        localtime_r(&tt, &l2);
        snprintf(buf, sizeof(buf), "%02d:00", l2.tm_hour);
        if (gx - text_w(buf, fs) / 2 > px0 && gx + text_w(buf, fs) / 2 < px1) {
            g_text(g, gx, py1 + 4, buf, fs, th.label, LV_TEXT_ALIGN_CENTER);
        }
    }
}

// Elevation curve: highlight colour above the horizon, grey below
template <class YPos>
static void draw_elev_curve(Gfx &g, const Theme &th, const float *es, int n, int px0, int px1,
                            const YPos &ypos, uint32_t col)
{
    auto xcol = [&](int i) { return px0 + i * (px1 - px0) / (n - 1); };
    for (int i = 1; i < n; i++) {
        bool up = es[i - 1] >= 0 && es[i] >= 0;
        g_line(g, xcol(i - 1), ypos(es[i - 1]), xcol(i), ypos(es[i]),
               up ? col : th.label, up ? 3 : 2, up ? LV_OPA_COVER : LV_OPA_60);
    }
}

// Lit part of the moon drawn row by row; the lit limb is mirrored south of the equator
static void draw_moon_disc(Gfx &g, int cx, int cy, int r, float phase, float lat, uint32_t lit, uint32_t dark)
{
    g_rect(g, cx - r, cy - r, cx + r, cy + r, dark, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    float c = cosf(2.0f * (float)M_PI * phase);
    bool waxing = phase < 0.5f;
    for (int dy = -r; dy <= r; dy++) {
        float hw = sqrtf((float)(r * r - dy * dy));
        float xl, xr;
        if (waxing) { xl = hw * c; xr = hw; }
        else        { xl = -hw;    xr = -hw * c; }
        if (lat < 0) {
            float t = xl;
            xl = -xr;
            xr = -t;
        }
        int a = cx + (int)lroundf(xl), b = cx + (int)lroundf(xr);
        if (b > a) g_rect(g, a, cy + dy, b, cy + dy, lit);
    }
}

static void draw_sun(Gfx &g, const Theme &th, int W, int H, int title_w)
{
    SunInfo s;
    day_night_sun(&s);
    const bool big = W >= 420 && H >= 260;
    const lv_font_t *fs = big ? &lv_font_montserrat_24 : &lv_font_montserrat_16;
    const lv_font_t *fl = big ? &lv_font_montserrat_36 : &lv_font_montserrat_24;
    const int lh = lv_font_get_line_height(fs), flh = lv_font_get_line_height(fl);
    char buf[96], fit[96], a[16];
    if (!s.valid) {
        g_text(g, W / 2, H / 2 - lh, "Waiting for the clock\nand a GPS position", fs, th.dim, LV_TEXT_ALIGN_CENTER);
        return;
    }
    const time_t now = time(nullptr);
    const SunDay &d = s.show_tomorrow ? s.tomorrow : s.today;
    uint32_t sky[6], sunc;
    sun_palette(th, sky, &sunc);

    // Sky state in the title row
    const int state = day_night_sky_state(s.elevation);
    int max_w = W - title_w - 34;
    if (max_w > 40) {
        fit_single_line(day_night_sky_name(state), fs, max_w, fit, sizeof(fit));
        g_text(g, W - 10, big ? 14 : 16, fit, fs, th.label, LV_TEXT_ALIGN_RIGHT);
    }

    // Elevation, trend, azimuth and day length
    int y = TILE_TOP;
    snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)lroundf(s.elevation));
    g_text(g, 12, y, buf, fl, th.value);
    int x = 12 + text_w(buf, fl) + 14;
    float later = day_night_sun_elevation_at(s.lat, s.lon, now + 300);
    snprintf(buf, sizeof(buf), "%s Az %d\xC2\xB0", later >= s.elevation ? LV_SYMBOL_UP : LV_SYMBOL_DOWN,
             (int)lroundf(s.azimuth) % 360);
    const int fy = y + (flh - lh) / 2;
    g_text(g, x, fy, buf, fs, th.label);
    x += text_w(buf, fs) + 20;
    snprintf(buf, sizeof(buf), "Day length %dh %02dm", d.day_length_s / 3600, (d.day_length_s / 60) % 60);
    if (W - 12 - x < text_w(buf, fs)) {
        snprintf(buf, sizeof(buf), "%dh %02dm", d.day_length_s / 3600, (d.day_length_s / 60) % 60);
    }
    if (W - 12 - x >= text_w(buf, fs)) g_text(g, W - 12, fy, buf, fs, th.label, LV_TEXT_ALIGN_RIGHT);

    // Layout: twilight table beside (wide, short tiles) or below the graph, or a one-line summary
    const int top = y + flh + 6;
    const int rh = lh + 4;
    const bool side = W >= 560 && H < 430;
    int plot_x1 = W - 10, plot_y1 = H - 6;
    int tx0 = 0, tw = 0, ty0 = 0, nrows = 0;
    if (side) {
        tw = W * 42 / 100;
        tx0 = W - 10 - tw;
        plot_x1 = tx0 - 16;
        ty0 = top;
        nrows = (H - 6 - ty0) / rh - 1;
    } else {
        nrows = (H - 6 - top - 110) / rh - 1;
        if (nrows >= 2) {
            tx0 = 12;
            tw = W - 22;
        }
    }
    if (nrows > 6) nrows = 6;
    const bool compact = !side && nrows < 2;
    if (compact) {
        plot_y1 = H - 6 - rh;
    } else if (!side) {
        ty0 = H - 6 - (nrows + 1) * rh;
        plot_y1 = ty0 - 8;
    }

    // Graph: sky bands, hour grid, elevation curve, now
    const int px0 = 10, px1 = plot_x1;
    const int py0 = top, py1 = plot_y1 - lh - 4;
    if (px1 - px0 >= 60 && py1 - py0 >= 30) {
        const time_t from = now / 60 * 60 - 12 * 3600, to = from + 24 * 3600;
        float es[400];
        int n = (px1 - px0) / 2 + 1;
        if (n > 400) n = 400;
        static CurveCache s_cache;
        if (cached_curve(s_cache, day_night_sun_elevation_at, s.lat, s.lon, from, n, es)) {
            float lo = 90, hi = -90;
            for (int i = 0; i < n; i++) { lo = fminf(lo, es[i]); hi = fmaxf(hi, es[i]); }
            lo = fminf(lo, -2.0f) - 2.0f;
            hi = fmaxf(hi, 2.0f) + 3.0f;
            const int cy0 = py0 + 4, cy1 = py1 - 4;
            auto ypos = [&](float e) { return cy1 - (int)lroundf((e - lo) / (hi - lo) * (cy1 - cy0)); };
            auto xpos = [&](time_t tt) { return px0 + (int)((double)(tt - from) / (double)(to - from) * (px1 - px0)); };

            draw_sky_bands(g, sky, es, n, px0, px1, py0, py1);
            draw_hour_grid(g, th, from, to, px0, px1, py0, py1, fs);

            const int hy = ypos(0);
            g_line(g, px0, hy, px1, hy, th.label, 1, LV_OPA_70);
            draw_elev_curve(g, th, es, n, px0, px1, ypos, sunc);
            const SunDay *days[2] = { &s.today, &s.tomorrow };
            for (const SunDay *sd : days) {
                for (int ev : { (int)SUN_RISE, (int)SUN_SET }) {
                    time_t t = sd->ev[ev];
                    if (t < from || t > to) continue;
                    int ex = xpos(t);
                    g_rect(g, ex - 4, hy - 4, ex + 4, hy + 4, th.value, LV_OPA_COVER, LV_RADIUS_CIRCLE);
                }
            }
            int nx = xpos(now), ny = ypos(s.elevation);
            g_line(g, nx, py0, nx, py1, th.value, 2, LV_OPA_60);
            g_rect(g, nx - 10, ny - 10, nx + 10, ny + 10, th.value, LV_OPA_COVER, LV_RADIUS_CIRCLE);
            g_rect(g, nx - 7, ny - 7, nx + 7, ny + 7, sunc, LV_OPA_COVER, LV_RADIUS_CIRCLE);
        }
    }

    // Next event of the day shown (highlighted in the table)
    time_t next_t = 0;
    for (int i = 0; i < SUN_EVENT_COUNT; i++) {
        if (d.ev[i] > now && (!next_t || d.ev[i] < next_t)) next_t = d.ev[i];
    }
    auto time_col = [&](time_t t) -> uint32_t { return !t || t < now ? th.dim : t == next_t ? th.value : th.label; };

    if (compact) {
        char b[16];
        if (d.ev[SUN_RISE]) fmt_hm(d.ev[SUN_RISE], a, sizeof(a)); else snprintf(a, sizeof(a), "--");
        if (d.ev[SUN_SET])  fmt_hm(d.ev[SUN_SET], b, sizeof(b));  else snprintf(b, sizeof(b), "--");
        snprintf(buf, sizeof(buf), "Rise %s   Set %s", a, b);
        fit_single_line(buf, fs, W - 22, fit, sizeof(fit));
        g_text(g, 12, H - 6 - rh + 2, fit, fs, th.label);
        return;
    }
    if (nrows < 1) return;

    // Twilight table: rows in time order, the least important dropped first when short of room
    static const struct { const char *name; int8_t am, pm; uint8_t prio; } s_rows[] = {
        { "Astro",    SUN_ASTRO_DAWN, SUN_ASTRO_DUSK,   3 },
        { "Nautical", SUN_NAUT_DAWN,  SUN_NAUT_DUSK,    2 },
        { "Civil",    SUN_CIVIL_DAWN, SUN_CIVIL_DUSK,   1 },
        { "Sun",      SUN_RISE,       SUN_SET,          0 },
        { "Golden",   SUN_GOLDEN_END, SUN_GOLDEN_START, 4 },
        { "Noon",     SUN_NOON,       -1,               5 },
    };
    const bool long_hdr = text_w("Evening", fs) + text_w("Nautical", fs) + 2 * text_w("00:00", fs) + 60 < tw;
    const int colw = (long_hdr ? text_w("Evening", fs) : text_w("00:00", fs)) + 16;
    const int c2 = tx0 + tw, c1 = c2 - colw;
    int ry = ty0;
    g_text(g, tx0, ry, s.show_tomorrow ? "Tomorrow" : "Today", fs, s.show_tomorrow ? th.accent : th.dim);
    g_text(g, c1, ry, long_hdr ? "Morning" : "AM", fs, th.dim, LV_TEXT_ALIGN_RIGHT);
    g_text(g, c2, ry, long_hdr ? "Evening" : "PM", fs, th.dim, LV_TEXT_ALIGN_RIGHT);
    ry += rh;
    for (const auto &r : s_rows) {
        if (r.prio >= nrows) continue;
        fit_single_line(r.name, fs, c1 - colw - tx0, fit, sizeof(fit));
        g_text(g, tx0, ry, fit, fs, th.label);
        time_t ta = d.ev[r.am];
        if (ta) fmt_hm(ta, a, sizeof(a)); else snprintf(a, sizeof(a), "--");
        if (ta && ta == next_t) {
            g_rect(g, c1 - text_w(a, fs) - 6, ry - 1, c1 + 6, ry + lh, th.accent, LV_OPA_40, 4);
        }
        g_text(g, c1, ry, a, fs, time_col(ta), LV_TEXT_ALIGN_RIGHT);
        if (r.pm >= 0) {
            time_t tb = d.ev[r.pm];
            if (tb) fmt_hm(tb, a, sizeof(a)); else snprintf(a, sizeof(a), "--");
            if (tb && tb == next_t) {
                g_rect(g, c2 - text_w(a, fs) - 6, ry - 1, c2 + 6, ry + lh, th.accent, LV_OPA_40, 4);
            }
            g_text(g, c2, ry, a, fs, time_col(tb), LV_TEXT_ALIGN_RIGHT);
        } else {
            snprintf(a, sizeof(a), "%d\xC2\xB0", (int)lroundf(d.noon_elev));
            g_text(g, c2, ry, a, fs, th.label, LV_TEXT_ALIGN_RIGHT);
        }
        ry += rh;
    }
}

static void draw_solunar(Gfx &g, const Theme &th, int W, int H, int title_w)
{
    MoonInfo m;
    moon_get(&m);
    const bool big = W >= 420 && H >= 260;
    const lv_font_t *fs = big ? &lv_font_montserrat_24 : &lv_font_montserrat_16;
    const lv_font_t *fm = big ? &lv_font_montserrat_28 : &lv_font_montserrat_24;
    const int lh = lv_font_get_line_height(fs), fmh = lv_font_get_line_height(fm);
    char buf[96], fit[96], a[16], b[16];
    if (!m.valid || !m.phase_name) {
        g_text(g, W / 2, H / 2 - lh / 2, "Waiting for the clock", fs, th.dim, LV_TEXT_ALIGN_CENTER);
        return;
    }
    const time_t now = time(nullptr);
    const bool day_theme = th.tile >= 0xe00000, red = th.value == 0xc01818;
    const uint32_t lit = red ? th.value : day_theme ? 0xd0a020 : 0xf0ead0;
    uint32_t sky[6], sunc;
    sun_palette(th, sky, &sunc);

    const SolunarPeriod *cur = nullptr;
    for (int i = 0; i < m.nper; i++) {
        const SolunarPeriod &p = m.per[i];
        if (p.start <= now && p.end > now && (!cur || p.major)) cur = &p;
    }
    const float alt = m.located ? moon_altitude_at(m.lat, m.lon, now) : 0;

    // State in the title row
    if (m.located && W - title_w - 34 > 40) {
        const char *st = cur ? (cur->major ? "Major period" : "Minor period") : alt >= 0 ? "Moon up" : "Moon down";
        fit_single_line(st, fs, W - title_w - 34, fit, sizeof(fit));
        g_text(g, W - 10, big ? 14 : 16, fit, fs, cur ? th.value : th.label, LV_TEXT_ALIGN_RIGHT);
    }

    // Header: moon disc, phase name, illumination, altitude / azimuth
    const int top = TILE_TOP;
    const int r = (fmh + lh + 2) / 2;
    draw_moon_disc(g, 12 + r, top + r, r, m.phase, m.lat, lit, th.dim);
    const int tx = 12 + 2 * r + 14, tw = W - tx - 10;
    if (tw > 40) {
        const lv_font_t *pf = text_w(m.phase_name, fm) <= tw ? fm : fs;
        fit_single_line(m.phase_name, pf, tw, fit, sizeof(fit));
        g_text(g, tx, top + (pf == fm ? 0 : fmh - lh), fit, pf, th.value);
        int il = (int)lroundf(m.illum * 100);
        if (m.located) {
            float later = moon_altitude_at(m.lat, m.lon, now + 300);
            snprintf(buf, sizeof(buf), "%d%% lit   %d\xC2\xB0 %s  Az %d\xC2\xB0", il, (int)lroundf(alt),
                     later >= alt ? LV_SYMBOL_UP : LV_SYMBOL_DOWN,
                     (int)lroundf(moon_azimuth_at(m.lat, m.lon, now)) % 360);
            if (text_w(buf, fs) > tw) {
                snprintf(buf, sizeof(buf), "%d%% lit   %d\xC2\xB0 %s", il, (int)lroundf(alt),
                         later >= alt ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
            }
        }
        if (!m.located || text_w(buf, fs) > tw) snprintf(buf, sizeof(buf), "%d%% lit", il);
        g_text(g, tx, top + fmh + 2, buf, fs, th.label);
    }
    const int gtop = top + 2 * r + 8;
    if (!m.located) {
        g_text(g, 12, gtop, "No position", fs, th.dim);
        return;
    }

    // Upcoming rise / set / transit / underfoot and the current or next solunar periods
    struct Row { const char *name; time_t t0, t1; uint8_t prio; };
    Row rows[6];
    int nr = 0;
    auto next_ev = [&](uint8_t type) -> time_t {
        for (int i = 0; i < m.nev; i++) if (m.ev[i].type == type && m.ev[i].t > now) return m.ev[i].t;
        return 0;
    };
    auto next_per = [&](bool major) -> const SolunarPeriod * {
        for (int i = 0; i < m.nper; i++) if (m.per[i].major == major && m.per[i].end > now) return &m.per[i];
        return nullptr;
    };
    const struct { const char *name; uint8_t type, prio; } evs[] = {
        { "Moonrise", MOON_RISE, 0 }, { "Moonset", MOON_SET, 1 },
        { "Overhead", MOON_TRANSIT, 4 }, { "Underfoot", MOON_UNDERFOOT, 5 },
    };
    for (const auto &e : evs) {
        time_t t = next_ev(e.type);
        if (t) rows[nr++] = { e.name, t, 0, e.prio };
    }
    for (int k = 0; k < 2; k++) {
        const SolunarPeriod *p = next_per(k == 0);
        if (p) rows[nr++] = { k == 0 ? "Major" : "Minor", p->start, p->end, (uint8_t)(2 + k) };
    }
    for (int i = 1; i < nr; i++) {
        Row r = rows[i];
        int j = i - 1;
        for (; j >= 0 && rows[j].t0 > r.t0; j--) rows[j + 1] = rows[j];
        rows[j + 1] = r;
    }

    // Layout like the sun tile: table beside (wide, short tiles) or below the graph, or a one-line summary
    const int rh = lh + 4;
    const bool side = W >= 560 && H < 430;
    int plot_x1 = W - 10, plot_y1 = H - 6;
    int tx0 = 0, tw2 = 0, ty0 = 0, nrows = 0;
    if (side) {
        tw2 = W * 42 / 100;
        tx0 = W - 10 - tw2;
        plot_x1 = tx0 - 16;
        ty0 = gtop;
        nrows = (H - 6 - ty0) / rh;
    } else {
        nrows = (H - 6 - gtop - 110) / rh;
        if (nrows >= 2) {
            tx0 = 12;
            tw2 = W - 22;
        }
    }
    if (nrows > 6) nrows = 6;
    const bool compact = !side && nrows < 2;
    if (compact) {
        plot_y1 = H - 6 - rh;
    } else if (!side) {
        ty0 = H - 6 - nrows * rh;
        plot_y1 = ty0 - 8;
    }

    // Graph: sky bands from the sun, solunar periods, hour grid, moon altitude curve, now
    const int px0 = 10, px1 = plot_x1;
    const int py0 = gtop, py1 = plot_y1 - lh - 4;
    if (px1 - px0 >= 60 && py1 - py0 >= 30) {
        const time_t from = now / 60 * 60 - 12 * 3600, to = from + 24 * 3600;
        static float ms[400], ss[400];
        static CurveCache s_moon_cache, s_sky_cache;
        int n = (px1 - px0) / 2 + 1;
        if (n > 400) n = 400;
        if (cached_curve(s_moon_cache, moon_altitude_at, m.lat, m.lon, from, n, ms) &&
            cached_curve(s_sky_cache, day_night_sun_elevation_at, m.lat, m.lon, from, n, ss)) {
            float lo = 90, hi = -90;
            for (int i = 0; i < n; i++) { lo = fminf(lo, ms[i]); hi = fmaxf(hi, ms[i]); }
            lo = fminf(lo, -2.0f) - 2.0f;
            hi = fmaxf(hi, 2.0f) + 3.0f;
            const int cy0 = py0 + 4, cy1 = py1 - 4;
            auto ypos = [&](float e) { return cy1 - (int)lroundf((e - lo) / (hi - lo) * (cy1 - cy0)); };
            auto xpos = [&](time_t tt) {
                if (tt < from) tt = from;
                if (tt > to) tt = to;
                return px0 + (int)((double)(tt - from) / (double)(to - from) * (px1 - px0));
            };

            draw_sky_bands(g, sky, ss, n, px0, px1, py0, py1);
            for (int i = 0; i < m.nper; i++) {
                const SolunarPeriod &p = m.per[i];
                if (p.end < from || p.start > to) continue;
                g_rect(g, xpos(p.start), py0, xpos(p.end), py1, lit, p.major ? LV_OPA_30 : LV_OPA_20);
            }
            draw_hour_grid(g, th, from, to, px0, px1, py0, py1, fs);

            const int hy = ypos(0);
            g_line(g, px0, hy, px1, hy, th.label, 1, LV_OPA_70);
            draw_elev_curve(g, th, ms, n, px0, px1, ypos, lit);
            for (int i = 0; i < m.nev; i++) {
                const MoonEvent &e = m.ev[i];
                if (e.t < from || e.t > to || (e.type != MOON_RISE && e.type != MOON_SET)) continue;
                int ex = xpos(e.t);
                g_rect(g, ex - 4, hy - 4, ex + 4, hy + 4, th.value, LV_OPA_COVER, LV_RADIUS_CIRCLE);
            }
            const int mr = big ? 9 : 7;
            int nx = xpos(now), ny = ypos(alt);
            g_line(g, nx, py0, nx, py1, th.value, 2, LV_OPA_60);
            g_rect(g, nx - mr - 3, ny - mr - 3, nx + mr + 3, ny + mr + 3, th.value, LV_OPA_COVER, LV_RADIUS_CIRCLE);
            draw_moon_disc(g, nx, ny, mr, m.phase, m.lat, lit, th.tile);
        }
    }

    if (compact) {
        time_t rise = next_ev(MOON_RISE), set = next_ev(MOON_SET);
        if (rise) fmt_hm(rise, a, sizeof(a)); else snprintf(a, sizeof(a), "--");
        if (set)  fmt_hm(set, b, sizeof(b));  else snprintf(b, sizeof(b), "--");
        snprintf(buf, sizeof(buf), "Rise %s   Set %s", a, b);
        fit_single_line(buf, fs, W - 22, fit, sizeof(fit));
        g_text(g, 12, H - 6 - rh + 2, fit, fs, th.label);
        return;
    }

    // Table in time order; the least important rows are dropped first when short of room
    const int c2 = tx0 + tw2;
    int ry = ty0;
    bool first = true;
    for (int i = 0; i < nr; i++) {
        const Row &row = rows[i];
        if (row.prio >= nrows) continue;
        fmt_hm(row.t0, a, sizeof(a));
        if (row.t1) {
            fmt_hm(row.t1, b, sizeof(b));
            snprintf(buf, sizeof(buf), "%s-%s", a, b);
        } else {
            snprintf(buf, sizeof(buf), "%s", a);
        }
        const bool active = row.t1 && row.t0 <= now;
        const int vw = text_w(buf, fs);
        fit_single_line(row.name, fs, tw2 - vw - 16, fit, sizeof(fit));
        g_text(g, tx0, ry, fit, fs, row.t1 && row.prio == 2 ? th.value : th.label);
        if (active) g_rect(g, c2 - vw - 6, ry - 1, c2 + 6, ry + lh, th.accent, LV_OPA_40, 4);
        g_text(g, c2, ry, buf, fs, active || first ? th.value : th.label, LV_TEXT_ALIGN_RIGHT);
        first = false;
        ry += rh;
    }
}

// ---------------------------------------------------------------------------
// Wind rose: compass card turning with the heading, close-hauled sectors and
// AWA / TWA markers relative to the bow, AWS in the centre and TWS below
// ---------------------------------------------------------------------------

static uint32_t mix_rgb(uint32_t a, uint32_t b, int pct_b)
{
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
        out |= (uint32_t)((ca * (100 - pct_b) + cb * pct_b) / 100) << sh;
    }
    return out;
}

static void g_circle(Gfx &g, int cx, int cy, int r, uint32_t col)
{
    g_rect(g, cx - r, cy - r, cx + r, cy + r, col, LV_OPA_COVER, LV_RADIUS_CIRCLE);
}

// LVGL angles: 0 = 3 o'clock, clockwise; r is the outer radius
static void g_arc(Gfx &g, int cx, int cy, int r, int w, int a0, int a1, uint32_t col)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = lv_color_hex(col);
    d.width = w;
    d.opa = LV_OPA_COVER;
    lv_point_t c = { (lv_coord_t)(g.a.x1 + cx), (lv_coord_t)(g.a.y1 + cy) };
    lv_draw_arc(g.dc, &d, &c, r, a0, a1);
}

// Triangle pointing outwards at a bow-relative angle (0 = ahead, clockwise)
static void g_marker(Gfx &g, int cx, int cy, float ang_deg, float tip_r, float h, float hw, uint32_t col)
{
    float a = (ang_deg - 90.0f) * 0.01745329f;
    float c = cosf(a), s = sinf(a), br = tip_r - h;
    float ox = g.a.x1 + cx, oy = g.a.y1 + cy;
    lv_point_t pts[3] = {
        { (lv_coord_t)lroundf(ox + tip_r * c), (lv_coord_t)lroundf(oy + tip_r * s) },
        { (lv_coord_t)lroundf(ox + br * c + hw * s), (lv_coord_t)lroundf(oy + br * s - hw * c) },
        { (lv_coord_t)lroundf(ox + br * c - hw * s), (lv_coord_t)lroundf(oy + br * s + hw * c) },
    };
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(col);
    d.bg_opa = LV_OPA_COVER;
    lv_draw_polygon(g.dc, &d, pts, 3);
}

static void fmt_wind_speed(int k, const char *path, char *val, size_t vn, char *unit, size_t un)
{
    DashTile t = {};
    snprintf(t.path, sizeof(t.path), "%s", path);
    snprintf(t.sk_units, sizeof(t.sk_units), "m/s");
    t.decimals = -1;
    DashFormatted f;
    dash_format(&t, s_wind.ok[W_AWS + k] ? s_wind.spd[k] : 0.0f, &f);
    snprintf(val, vn, "%s", s_wind.ok[W_AWS + k] ? f.value : "---");
    snprintf(unit, un, "%s", f.unit);
}

static void draw_wind(Gfx &g, const Theme &th, int W, int H)
{
    int y0 = 0, avail = H;
    int R = (W < H ? W : H) / 2 - 6;
    const bool compact = R < 110;
    if (compact && W < 2 * H) {
        // Keep the title row clear on small tiles (wide, short tiles have room beside the title)
        y0 = 34;
        avail = H - y0;
        R = (W < avail ? W : avail) / 2 - 3;
    }
    if (R < 30) return;
    const int cx = W / 2, cy = y0 + avail / 2;
    const float k = R / 280.0f;
    const int band = R * (compact ? 30 : 41) / 100;
    const int r_in = R - band;
    int sect = (int)lroundf(18 * k);
    if (sect < 4) sect = 4;

    const bool hdg_ok = s_wind.ok[W_HDG];
    const float hdg = hdg_ok ? wind_deg(W_HDG) : 0.0f;

    // Card band with the fixed port / starboard close-hauled sectors on its rim
    g_circle(g, cx, cy, R, mix_rgb(th.tile, th.label, 22));
    g_arc(g, cx, cy, R, sect, 180, 270, th.port);
    g_arc(g, cx, cy, R, sect, 270, 360, th.stbd);
    g_circle(g, cx, cy, r_in, th.tile);

    // Ticks every 5 degrees, majors every 30 (fewer on small tiles)
    const int step = compact ? 10 : 5;
    const int maj_len = compact ? band * 35 / 100 : (int)lroundf(25 * k);
    const int min_len = compact ? band * 20 / 100 : (int)lroundf(15 * k);
    for (int d = 0; d < 360; d += step) {
        bool major = d % 30 == 0;
        if (compact && !major && R < 80) continue;
        float a = ((float)d - hdg - 90.0f) * 0.01745329f;
        float c = cosf(a), s = sinf(a);
        int len = major ? maj_len : min_len;
        g_line(g, cx + (int)lroundf(R * c), cy + (int)lroundf(R * s),
               cx + (int)lroundf((R - len) * c), cy + (int)lroundf((R - len) * s),
               major ? th.value : th.label, major ? (compact ? 2 : 3) : (compact ? 1 : 2));
    }

    // Labels: N/E/S/W and bearings with a heading, otherwise bow-relative angles
    const lv_font_t *fnum = R >= 240 ? &lv_font_montserrat_28 : R >= 150 ? &lv_font_montserrat_24 : &lv_font_montserrat_16;
    const lv_font_t *fcard = R >= 240 ? &lv_font_montserrat_36 : R >= 150 ? &lv_font_montserrat_28
                           : compact && R < 80 ? &lv_font_montserrat_14 : &lv_font_montserrat_16;
    const float lr = compact ? r_in + band * 0.45f : R - 62.0f * k;
    for (int d = 0; d < 360; d += 30) {
        char txt[8];
        const lv_font_t *f = fnum;
        uint32_t col = th.label;
        if (hdg_ok && d % 90 == 0) {
            snprintf(txt, sizeof(txt), "%c", "NESW"[d / 90]);
            f = fcard;
            col = th.value;
        } else if (compact) {
            continue;
        } else {
            snprintf(txt, sizeof(txt), "%d", hdg_ok ? d : (d <= 180 ? d : 360 - d));
        }
        float a = ((float)d - hdg) * 0.01745329f;
        int lx = cx + (int)lroundf(lr * sinf(a));
        int ly = cy - (int)lroundf(lr * cosf(a));
        g_text(g, lx, ly - lv_font_get_line_height(f) / 2, txt, f, col, LV_TEXT_ALIGN_CENTER);
    }

    // Lubber line at the bow
    int lw = (int)lroundf(4 * k);
    g_line(g, cx, cy - R, cx, cy - r_in, th.accent, lw < 2 ? 2 : lw);

    // Wind markers on the rim pointing inward, outside the label ring (true under apparent)
    float th_h = 30 * k, th_w = 15 * k;
    if (th_h < 10) th_h = 10;
    if (th_w < 6) th_w = 6;
    const float tip = (float)R - th_h;
    if (s_wind.ok[W_TWA]) g_marker(g, cx, cy, wind_deg(W_TWA), tip, -th_h, th_w, th.accent);
    if (s_wind.ok[W_AWA]) g_marker(g, cx, cy, wind_deg(W_AWA), tip, -th_h, th_w, th.value);

    // Centre: AWS (largest font that fits the inner disc) and TWS
    char aws[16], unit[16], tws[16], tunit[16], buf[48];
    fmt_wind_speed(0, "environment.wind.speedApparent", aws, sizeof(aws), unit, sizeof(unit));
    fmt_wind_speed(1, "environment.wind.speedTrue", tws, sizeof(tws), tunit, sizeof(tunit));
    const int ri = r_in - 4;
    static const lv_font_t *const vfonts[] = {
        &lv_font_inst_176, &lv_font_inst_144, &lv_font_inst_116, &lv_font_inst_92, &lv_font_inst_72,
        &lv_font_inst_56, &lv_font_inst_44, &lv_font_montserrat_36, &lv_font_montserrat_28,
        &lv_font_montserrat_24, &lv_font_montserrat_16,
    };
    const float max_w = (compact ? 1.75f : 1.5f) * ri, max_h = (compact ? 1.1f : 0.8f) * ri;
    const lv_font_t *vf = vfonts[sizeof(vfonts) / sizeof(vfonts[0]) - 1];
    for (const lv_font_t *f : vfonts) {
        if (text_w(aws, f) <= max_w && lv_font_get_line_height(f) <= max_h) { vf = f; break; }
    }
    const int vh = lv_font_get_line_height(vf);
    const uint32_t vcol = s_wind.ok[W_AWS] ? th.value : th.dim;
    const lv_font_t *fs = ri >= 120 ? &lv_font_montserrat_24 : ri >= 70 ? &lv_font_montserrat_16 : &lv_font_montserrat_14;
    if (compact) {
        g_text(g, cx, cy - vh / 2, aws, vf, vcol, LV_TEXT_ALIGN_CENTER);
        return;
    }
    const int vc = cy - ri / 20;
    snprintf(buf, sizeof(buf), unit[0] ? "AWS %s" : "AWS", unit);
    g_text(g, cx, cy - ri * 62 / 100, buf, fs, th.label, LV_TEXT_ALIGN_CENTER);
    g_text(g, cx, vc - vh / 2, aws, vf, vcol, LV_TEXT_ALIGN_CENTER);
    snprintf(buf, sizeof(buf), "TWS %s%s%s", tws, tunit[0] && s_wind.ok[W_TWS] ? " " : "",
             s_wind.ok[W_TWS] ? tunit : "");
    const lv_font_t *ft = ri >= 160 ? &lv_font_montserrat_28 : fs;
    g_text(g, cx, cy + ri * 44 / 100, buf, ft, s_wind.ok[W_TWS] ? th.label : th.dim, LV_TEXT_ALIGN_CENTER);
}

// While true, on_gfx_draw renders live (into a tile cache) instead of blitting it
static bool s_rendering = false;

void InstrumentsApp::on_gfx_draw(lv_event_t *e)
{
    InstrumentsApp *app = (InstrumentsApp *)lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(obj);
    int p = idx / DASH_MAX_TILES, i = idx % DASH_MAX_TILES;
    if (!app->_cfg || p >= app->_cfg->page_count) return;
    const DashTile &t = app->_cfg->pages[p].tiles[i];
    TileUi &u = app->_tiles[p][i];
    const Theme &th = theme_of(app->_cfg);

    Gfx g;
    g.dc = lv_event_get_draw_ctx(e);
    lv_obj_get_coords(obj, &g.a);
    int W = lv_area_get_width(&g.a), H = lv_area_get_height(&g.a);

    if (!s_rendering && u.cache_ok) {
        lv_draw_img_dsc_t d;
        lv_draw_img_dsc_init(&d);
        lv_draw_img(g.dc, &d, &g.a, &u.cache_dsc);
        return;
    }
    if (s_rendering) {
        // The cached image covers the whole tile, rounded corners included
        g_rect(g, 0, 0, W - 1, H - 1, th.bg);
        g_rect(g, 0, 0, W - 1, H - 1, th.tile, LV_OPA_COVER, 8);
    }

    int title_w = u.name ? text_w(lv_label_get_text(u.name), lv_obj_get_style_text_font(u.name, 0)) : 0;
    if (strcmp(t.path, "panel.tide.curve") == 0) {
        draw_tide_curve(g, th, t, W, H, title_w);
    } else if (strcmp(t.path, "panel.sun") == 0) {
        draw_sun(g, th, W, H, title_w);
    } else if (is_wind_rose(t)) {
        draw_wind(g, th, W, H);
    } else {
        draw_solunar(g, th, W, H, title_w);
    }
}

// ---------------------------------------------------------------------------
// Graphic tile cache: each custom-drawn tile is rendered once into a PSRAM
// image, so page swipes and redraws only copy pixels
// ---------------------------------------------------------------------------

bool InstrumentsApp::render_gfx_cache(int p, int i)
{
    TileUi &u = _tiles[p][i];
    if (!u.gfx || u.cache_fail) return false;
    uint32_t size = lv_snapshot_buf_size_needed(u.gfx, LV_IMG_CF_TRUE_COLOR);
    if (!u.cache_buf) {
        u.cache_buf = size ? (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM) : nullptr;
        if (!u.cache_buf) {
            ESP_LOGW(TAG, "No memory for tile cache (%lu bytes), drawing live", (unsigned long)size);
            u.cache_fail = true;
            return false;
        }
    }
    int64_t t0 = esp_timer_get_time();
    s_rendering = true;
    lv_res_t res = lv_snapshot_take_to_buf(u.gfx, LV_IMG_CF_TRUE_COLOR, &u.cache_dsc, u.cache_buf, size);
    s_rendering = false;
    lv_img_cache_invalidate_src(&u.cache_dsc);
    dash_web_perf_note_gfx((uint32_t)((esp_timer_get_time() - t0) / 1000));
    if (res != LV_RES_OK) {
        heap_caps_free(u.cache_buf);
        u.cache_buf = nullptr;
        u.cache_ok = false;
        u.cache_fail = true;
        return false;
    }
    u.cache_ok = true;
    u.cache_sig = u.gfx_sig;
    u.cache_ms = now_ms();
    lv_obj_invalidate(u.gfx);
    return true;
}

void InstrumentsApp::refresh_gfx_caches()
{
    if (!_tv) return;
    // Never render while a swipe or page animation is running: it would stall it
    if (lv_obj_is_scrolling(_tv) || lv_disp_get_inactive_time(NULL) < 300) return;
    const int n_pages = _cfg->page_count;
    auto needs = [&](int p, int i, bool current) {
        const TileUi &u = _tiles[p][i];
        if (!u.gfx || u.cache_fail) return false;
        if (!u.cache_ok) return true;
        if (u.cache_sig == u.gfx_sig) return false;
        // Live wind is redrawn at most twice a second; neighbouring pages at most every 5 s
        if (current) return now_ms() - u.cache_ms >= 500;
        return abs(p - _page) == 1 && now_ms() - u.cache_ms >= 5000;
    };
    // The visible page is always kept current
    int n = dash_layout_tile_count(_cfg->pages[_page].layout);
    for (int i = 0; i < n; i++) {
        if (needs(_page, i, true)) render_gfx_cache(_page, i);
    }
    // Then one tile per tick elsewhere, nearest pages first
    for (int dist = 1; dist < n_pages; dist++) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            int p = _page + sgn * dist;
            if (p < 0 || p >= n_pages) continue;
            int m = dash_layout_tile_count(_cfg->pages[p].layout);
            for (int i = 0; i < m; i++) {
                if (needs(p, i, false)) {
                    render_gfx_cache(p, i);
                    return;
                }
            }
        }
    }
}

void InstrumentsApp::free_gfx_caches()
{
    for (int p = 0; p < DASH_MAX_PAGES; p++) {
        for (int i = 0; i < DASH_MAX_TILES; i++) {
            TileUi &u = _tiles[p][i];
            if (u.cache_buf) {
                lv_img_cache_invalidate_src(&u.cache_dsc);
                heap_caps_free(u.cache_buf);
            }
            u.cache_buf = nullptr;
            u.cache_ok = false;
        }
    }
}

void InstrumentsApp::on_tile_event(lv_event_t *e)
{
    InstrumentsApp *app = (InstrumentsApp *)lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(obj);
    int p = idx / DASH_MAX_TILES, i = idx % DASH_MAX_TILES;
    if (p >= app->_cfg->page_count) return;
    TileUi &u = app->_tiles[p][i];
    const DashTile &t = app->_cfg->pages[p].tiles[i];

    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        if (app->_cfg->settings.locked) {
            audio_feedback_invalid();
            return;
        }
        audio_feedback_click();
        app->open_editor(p, i, &t);
        return;
    }

    // Short click: acknowledge alarm, otherwise toggle min/max
    const DashTileState *st = dash_engine_state(p, i);
    if (st && st->alarm && !st->acked) {
        dash_engine_ack(p, i);
        audio_feedback_click();
        return;
    }
    if (!t.path[0] || u.gfx) return;
    u.show_minmax = !u.show_minmax;
    if (u.show_minmax) lv_obj_clear_flag(u.minmax, LV_OBJ_FLAG_HIDDEN);
    else               lv_obj_add_flag(u.minmax, LV_OBJ_FLAG_HIDDEN);
    u.font_idx = -1;
    app->update_tile(p, i, true);
}

// ---------------------------------------------------------------------------
// Overlay helpers
// ---------------------------------------------------------------------------

InstrumentsApp::Overlay InstrumentsApp::make_overlay(const char *title, const char *ok_text,
                                                     lv_event_cb_t ok_cb, lv_event_cb_t cancel_cb)
{
    Overlay o = {};
    o.root = lv_obj_create(_screen);
    no_style_box(o.root);
    lv_obj_set_pos(o.root, _area_x, _area_y);
    lv_obj_set_size(o.root, _area_w, _area_h);
    lv_obj_set_style_bg_color(o.root, lv_color_hex(0x0e1318), 0);
    lv_obj_set_style_bg_opa(o.root, LV_OPA_COVER, 0);

    lv_obj_t *head = lv_obj_create(o.root);
    no_style_box(head);
    lv_obj_set_size(head, _area_w, OV_HEAD_H);
    lv_obj_set_style_bg_color(head, lv_color_hex(0x1c252e), 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);

    lv_obj_t *t = lv_label_create(head);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_center(t);

    if (cancel_cb) {
        lv_obj_t *b = add_button(head, "Cancel", cancel_cb, 0x3a4652);
        lv_obj_align(b, LV_ALIGN_LEFT_MID, 8, 0);
    }
    if (ok_cb) {
        lv_obj_t *b = add_button(head, ok_text, ok_cb, 0x1f6fd0);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, -8, 0);
    }

    o.body = lv_obj_create(o.root);
    no_style_box(o.body);
    lv_obj_add_flag(o.body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(o.body, LV_DIR_VER);
    lv_obj_set_pos(o.body, 0, OV_HEAD_H);
    lv_obj_set_size(o.body, _area_w, _area_h - OV_HEAD_H);
    lv_obj_set_flex_flow(o.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(o.body, 16, 0);
    lv_obj_set_style_pad_row(o.body, 14, 0);

    o.kb = lv_keyboard_create(o.root);
    lv_obj_set_size(o.kb, _area_w, KB_H);
    lv_obj_align(o.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(o.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(o.kb, on_kb_event, LV_EVENT_READY, this);
    lv_obj_add_event_cb(o.kb, on_kb_event, LV_EVENT_CANCEL, this);
    return o;
}

void InstrumentsApp::close_overlay(Overlay &o)
{
    if (o.root) lv_obj_del(o.root);
    o = {};
    if (&o == &_ed) _ed_path = _ed_label = _ed_unit = _ed_dec = _ed_angle = _ed_smooth =
                    _ed_trend = _ed_lo_sw = _ed_lo = _ed_hi_sw = _ed_hi = _ed_beep = nullptr;
    if (&o == &_pk) _pk_search = _pk_list = _pk_status = nullptr;
    if (&o == &_mn) _mn_name = _mn_layout = _mn_theme = _mn_cycle = _mn_stale =
                    _mn_start = _mn_lock = _mn_group = _mn_auto = nullptr;
}

lv_obj_t *InstrumentsApp::add_row(lv_obj_t *body, const char *label)
{
    lv_obj_t *row = lv_obj_create(body);
    no_style_box(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    if (label) {
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, label);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xc8d2dc), 0);
    }
    return row;
}

lv_obj_t *InstrumentsApp::add_dropdown(lv_obj_t *row, const char *options, int sel)
{
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, options);
    lv_obj_set_width(dd, 340);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dd), &lv_font_montserrat_24, 0);
    if (sel >= 0 && sel < (int)lv_dropdown_get_option_cnt(dd)) lv_dropdown_set_selected(dd, sel);
    return dd;
}

lv_obj_t *InstrumentsApp::add_textarea(Overlay &o, lv_obj_t *row, const char *text, int width,
                                       bool numeric)
{
    lv_obj_t *ta = lv_textarea_create(row);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_width(ta, width);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_24, 0);
    if (numeric) {
        lv_textarea_set_accepted_chars(ta, "0123456789.-");
        lv_textarea_set_max_length(ta, 10);
        lv_obj_add_flag(ta, LV_OBJ_FLAG_USER_1);
    }
    lv_textarea_set_text(ta, text ? text : "");
    lv_obj_set_user_data(ta, &o);
    lv_obj_add_event_cb(ta, on_ta_clicked, LV_EVENT_CLICKED, this);
    return ta;
}

lv_obj_t *InstrumentsApp::add_switch(lv_obj_t *row, bool on)
{
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 80, 40);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    return sw;
}

lv_obj_t *InstrumentsApp::add_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb,
                                     uint32_t color)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_height(b, 48);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
    return b;
}

void InstrumentsApp::on_ta_clicked(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    Overlay *o = (Overlay *)lv_obj_get_user_data(ta);
    if (!o || !o->kb) return;
    lv_keyboard_set_mode(o->kb, lv_obj_has_flag(ta, LV_OBJ_FLAG_USER_1)
                                    ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(o->kb, ta);
    lv_obj_clear_flag(o->kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(o->body, lv_obj_get_height(o->root) - OV_HEAD_H - KB_H);
    lv_obj_update_layout(o->body);
    lv_obj_scroll_to_view_recursive(ta, LV_ANIM_OFF);
}

void InstrumentsApp::on_kb_event(lv_event_t *e)
{
    InstrumentsApp *app = (InstrumentsApp *)lv_event_get_user_data(e);
    lv_obj_t *kb = lv_event_get_target(e);
    Overlay *o = kb == app->_ed.kb ? &app->_ed : kb == app->_pk.kb ? &app->_pk :
                 kb == app->_mn.kb ? &app->_mn : nullptr;
    if (!o) return;
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(kb, nullptr);
    lv_obj_set_height(o->body, lv_obj_get_height(o->root) - OV_HEAD_H);
}

// ---------------------------------------------------------------------------
// Tile editor
// ---------------------------------------------------------------------------

static const char *SMOOTH_OPTS = "Off\nLow\nMedium\nHigh";
static const char *TREND_OPTS  = "Off\n1 min\n5 min\n15 min\n60 min";
static const char *DEC_OPTS    = "Auto\n0\n1\n2\n3";
static const char *ANGLE_OPTS  = "Auto\n0 - 360\n+/- 180\nPort / Stbd";

void InstrumentsApp::open_editor(int page, int slot, const DashTile *tile)
{
    if (tile != &_ed_tile) _ed_tile = *tile;
    _ed_page = page;
    _ed_slot = slot;

    char title[48];
    snprintf(title, sizeof(title), "%s - tile %d", _cfg->pages[page].name, slot + 1);
    _ed = make_overlay(title, "Save", on_ed_save, on_ed_cancel);
    lv_obj_t *body = _ed.body;

    // Data path
    lv_obj_t *row = add_row(body, "Data");
    _ed_path = add_button(row, _ed_tile.path[0] ? _ed_tile.path : "Choose...", on_ed_path, 0x2a3642);
    lv_obj_set_width(_ed_path, 470);
    lv_obj_t *pl = lv_obj_get_child(_ed_path, 0);
    lv_label_set_long_mode(pl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(pl, 440);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_16, 0);

    row = add_row(body, "Label");
    _ed_label = add_textarea(_ed, row, _ed_tile.label, 340, false);
    lv_textarea_set_max_length(_ed_label, sizeof(_ed_tile.label) - 1);

    // Units
    const DashUnitOption *opts;
    int n = dash_unit_options(_ed_tile.sk_units, &opts);
    char ubuf[256] = {};
    int usel = 0;
    for (int i = 0; i < n; i++) {
        char a[24];
        ascii_unit(opts[i].label, a, sizeof(a));
        if (i == 0 && _ed_tile.sk_units[0]) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "Auto (%s)", _ed_tile.sk_units);
            ascii_unit(tmp, a, sizeof(a));
        }
        strncat(ubuf, a, sizeof(ubuf) - strlen(ubuf) - 2);
        if (i < n - 1) strcat(ubuf, "\n");
        if (strcmp(opts[i].id, _ed_tile.unit) == 0) usel = i;
    }
    row = add_row(body, "Units");
    _ed_unit = add_dropdown(row, ubuf, usel);

    row = add_row(body, "Decimals");
    _ed_dec = add_dropdown(row, DEC_OPTS, _ed_tile.decimals + 1);

    if (dash_is_angle(&_ed_tile)) {
        row = add_row(body, "Angle style");
        _ed_angle = add_dropdown(row, ANGLE_OPTS, _ed_tile.angle);
    }

    row = add_row(body, "Smoothing");
    _ed_smooth = add_dropdown(row, SMOOTH_OPTS, _ed_tile.smooth);

    row = add_row(body, "Trend graph");
    _ed_trend = add_dropdown(row, TREND_OPTS, _ed_tile.trend);

    char num[16];
    row = add_row(body, "Low alarm");
    lv_obj_t *grp = lv_obj_create(row);
    no_style_box(grp);
    lv_obj_set_size(grp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grp, 12, 0);
    _ed_lo_sw = add_switch(grp, _ed_tile.alarm_lo_en);
    snprintf(num, sizeof(num), "%g", _ed_tile.alarm_lo);
    _ed_lo = add_textarea(_ed, grp, _ed_tile.alarm_lo_en ? num : "", 200, true);

    row = add_row(body, "High alarm");
    grp = lv_obj_create(row);
    no_style_box(grp);
    lv_obj_set_size(grp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grp, 12, 0);
    _ed_hi_sw = add_switch(grp, _ed_tile.alarm_hi_en);
    snprintf(num, sizeof(num), "%g", _ed_tile.alarm_hi);
    _ed_hi = add_textarea(_ed, grp, _ed_tile.alarm_hi_en ? num : "", 200, true);

    row = add_row(body, "Alarm beep");
    _ed_beep = add_switch(row, _ed_tile.alarm_beep);

    lv_obj_t *hint = lv_label_create(body);
    lv_label_set_text(hint, "Alarm limits are in the displayed units. Tap a tile to\n"
                            "acknowledge an alarm or show min / max.");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x7a8794), 0);

    row = add_row(body, nullptr);
    add_button(row, "Reset min/max", on_ed_reset_minmax, 0x3a4652);
    add_button(row, "Clear tile", on_ed_clear, 0x8a2020);

    if (!_ed_tile.path[0]) open_picker();
}

void InstrumentsApp::read_editor()
{
    if (!_ed.root) return;
    snprintf(_ed_tile.label, sizeof(_ed_tile.label), "%s", lv_textarea_get_text(_ed_label));
    const DashUnitOption *opts;
    int n = dash_unit_options(_ed_tile.sk_units, &opts);
    int sel = lv_dropdown_get_selected(_ed_unit);
    snprintf(_ed_tile.unit, sizeof(_ed_tile.unit), "%s", sel < n ? opts[sel].id : "");
    _ed_tile.decimals = (int8_t)lv_dropdown_get_selected(_ed_dec) - 1;
    if (_ed_angle) _ed_tile.angle = (uint8_t)lv_dropdown_get_selected(_ed_angle);
    _ed_tile.smooth = (uint8_t)lv_dropdown_get_selected(_ed_smooth);
    _ed_tile.trend  = (uint8_t)lv_dropdown_get_selected(_ed_trend);
    const char *lo = lv_textarea_get_text(_ed_lo);
    const char *hi = lv_textarea_get_text(_ed_hi);
    _ed_tile.alarm_lo_en = lv_obj_has_state(_ed_lo_sw, LV_STATE_CHECKED) && lo[0];
    _ed_tile.alarm_hi_en = lv_obj_has_state(_ed_hi_sw, LV_STATE_CHECKED) && hi[0];
    _ed_tile.alarm_lo = lo[0] ? strtof(lo, nullptr) : 0.0f;
    _ed_tile.alarm_hi = hi[0] ? strtof(hi, nullptr) : 0.0f;
    _ed_tile.alarm_beep = lv_obj_has_state(_ed_beep, LV_STATE_CHECKED);
}

void InstrumentsApp::save_editor()
{
    DashConfig *cfg = (DashConfig *)malloc(sizeof(DashConfig));
    if (!cfg) return;
    dash_config_get(cfg);
    if (_ed_page < cfg->page_count && _ed_slot < DASH_MAX_TILES) {
        cfg->pages[_ed_page].tiles[_ed_slot] = _ed_tile;
        close_overlay(_ed);
        apply_config(cfg, _ed_page);
    } else {
        close_overlay(_ed);
    }
    free(cfg);
}

void InstrumentsApp::on_ed_save(lv_event_t *e)
{
    InstrumentsApp *app = (InstrumentsApp *)lv_event_get_user_data(e);
    app->read_editor();
    if (!app->_ed_tile.label[0]) {
        const char *f = dash_friendly_label(app->_ed_tile.path);
        snprintf(app->_ed_tile.label, sizeof(app->_ed_tile.label), "%s", f ? f : "");
    }
    app->save_editor();
}

void InstrumentsApp::on_ed_cancel(lv_event_t *e)
{
    InstrumentsApp *app = (InstrumentsApp *)lv_event_get_user_data(e);
    app->close_overlay(app->_ed);
}

void InstrumentsApp::on_ed_path(lv_event_t *e)
{
    InstrumentsApp *app = (InstrumentsApp *)lv_event_get_user_data(e);
    app->open_picker();
}

void InstrumentsApp::on_ed_clear(lv_event_t *e)
{
    InstrumentsApp *app = (InstrumentsApp *)lv_event_get_user_data(e);
    dash_tile_clear(&app->_ed_tile);
    app->save_editor();
}

void InstrumentsApp::on_ed_reset_minmax(lv_event_t *e)
{
    InstrumentsApp *app = (InstrumentsApp *)lv_event_get_user_data(e);
    dash_engine_reset_minmax(app->_ed_page, app->_ed_slot);
    audio_feedback_click();
}

// ---------------------------------------------------------------------------
// Path picker
// ---------------------------------------------------------------------------

void InstrumentsApp::open_picker()
{
    sk_catalog_refresh(false);
    _pk = make_overlay("Select data", nullptr, nullptr, on_pk_cancel);
    lv_obj_t *body = _pk.body;
    lv_obj_set_style_pad_row(body, 8, 0);

    lv_obj_t *row = add_row(body, nullptr);
    _pk_search = add_textarea(_pk, row, "", 470, false);
    lv_textarea_set_placeholder_text(_pk_search, "Search or type a path");
    lv_obj_add_event_cb(_pk_search, on_pk_search, LV_EVENT_VALUE_CHANGED, this);
    add_button(row, "Use typed", on_pk_typed, 0x2a3642);

    _pk_status = lv_label_create(body);
    lv_obj_set_style_text_font(_pk_status, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_pk_status, lv_color_hex(0x7a8794), 0);

    _pk_list = lv_list_create(body);
    lv_obj_set_width(_pk_list, LV_PCT(100));
    lv_obj_set_flex_grow(_pk_list, 1);
    lv_obj_set_style_bg_color(_pk_list, lv_color_hex(0x161d24), 0);
    lv_obj_set_style_border_width(_pk_list, 0, 0);
    // Keep the list inside the body so only the list scrolls
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    _pk_filter[0] = '\0';
    fill_picker();
}

static bool contains_ci(const char *hay, const char *needle)
{
    if (!needle[0]) return true;
    size_t n = strlen(needle);
    for (const char *h = hay; *h; h++) {
        if (strncasecmp(h, needle, n) == 0) return true;
    }
    return false;
}

void InstrumentsApp::fill_picker()
{
    if (!_pk_list) return;
    _pk_dirty = false;
    _pk_gen = sk_catalog_generation();
    snprintf(_pk_filter, sizeof(_pk_filter), "%s", lv_textarea_get_text(_pk_search));

    lv_obj_clean(_pk_list);
    int st = sk_catalog_state();
    int total = sk_catalog_count();
    int shown = 0, matches = 0;
    SkPathInfo info;
    for (int i = 0; i < total; i++) {
        if (!sk_catalog_get(i, &info)) break;
        if (!contains_ci(info.path, _pk_filter) && !contains_ci(info.display_name, _pk_filter)) {
            continue;
        }
        matches++;
        if (shown >= 100) continue;
        shown++;
        lv_obj_t *btn = lv_list_add_btn(_pk_list, nullptr, info.path);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x161d24), 0);
        lv_obj_t *pl = lv_obj_get_child(btn, 0);
        lv_obj_set_style_text_font(pl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(pl, lv_color_white(), 0);
        lv_label_set_long_mode(pl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(pl, LV_PCT(100));

        char sub[96], val[24];
        DashTile tmp;
        dash_tile_clear(&tmp);
        dash_tile_set_path(&tmp, info.path, info.units, info.display_name);
        DashFormatted f;
        dash_format(&tmp, info.value, &f);
        char unit[16];
        ascii_unit(f.unit, unit, sizeof(unit));
        snprintf(val, sizeof(val), "%s", f.value);
        // Large-font degree sign is not in montserrat_16
        char *deg = strstr(val, "\xc2\xb0");
        if (deg) snprintf(deg, sizeof(val) - (deg - val), " deg");
        snprintf(sub, sizeof(sub), "%s   %s %s", tmp.label, val, unit);
        lv_obj_t *sl = lv_label_create(btn);
        lv_label_set_text(sl, sub);
        lv_obj_set_style_text_font(sl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(sl, lv_color_hex(0x8fa0b0), 0);
        lv_obj_add_event_cb(btn, on_pk_item, LV_EVENT_CLICKED, this);
    }

    char status[96];
    if (st == SK_CATALOG_LOADING && total == 0) {
        snprintf(status, sizeof(status), "Loading paths from the Signal K server...");
    } else if (st == SK_CATALOG_ERROR && total == 0) {
        snprintf(status, sizeof(status), "Server not reachable - type a path and press 'Use typed'");
    } else if (matches > shown) {
        snprintf(status, sizeof(status), "%d of %d paths shown - refine the search", shown, matches);
    } else {
        snprintf(status, sizeof(status), "%d paths", matches);
    }
    lv_label_set_text(_pk_status, status);
}

void InstrumentsApp::choose_path(const char *path)
{
    if (!path || !path[0]) return;
    char p[96];
    snprintf(p, sizeof(p), "%s", path);
    read_editor();
    SkPathInfo info;
    bool found = sk_catalog_find(p, &info);
    dash_tile_set_path(&_ed_tile, p, found ? info.units : nullptr, found ? info.display_name : nullptr);
    close_overlay(_pk);
    close_overlay(_ed);
    open_editor(_ed_page, _ed_slot, &_ed_tile);
}

void InstrumentsApp::on_pk_cancel(lv_event_t *e)
{
    InstrumentsApp *app = (InstrumentsApp *)lv_event_get_user_data(e);
    app->close_overlay(app->_pk);
}

void InstrumentsApp::on_pk_search(lv_event_t *e)
{
    InstrumentsApp *app = (InstrumentsApp *)lv_event_get_user_data(e);
    app->_pk_dirty = true;
}

void InstrumentsApp::on_pk_item(lv_event_t *e)
{
    InstrumentsApp *app = (InstrumentsApp *)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target(e);
    lv_obj_t *pl = lv_obj_get_child(btn, 0);
    if (pl) app->choose_path(lv_label_get_text(pl));
}

void InstrumentsApp::on_pk_typed(lv_event_t *e)
{
    InstrumentsApp *app = (InstrumentsApp *)lv_event_get_user_data(e);
    char p[96];
    snprintf(p, sizeof(p), "%s", lv_textarea_get_text(app->_pk_search));
    // Trim spaces
    char *s = p;
    while (*s == ' ') s++;
    for (int i = (int)strlen(s) - 1; i >= 0 && s[i] == ' '; i--) s[i] = '\0';
    if (!s[0] || strchr(s, ' ')) {
        audio_feedback_invalid();
        return;
    }
    app->choose_path(s);
}

// ---------------------------------------------------------------------------
// Page menu
// ---------------------------------------------------------------------------

static int option_index(const uint8_t *opts, int n, int v)
{
    int best = 0;
    for (int i = 0; i < n; i++) {
        if (abs(opts[i] - v) < abs(opts[best] - v)) best = i;
    }
    return best;
}

void InstrumentsApp::open_menu()
{
    free(_edit);
    _edit = (DashConfig *)malloc(sizeof(DashConfig));
    if (!_edit) return;
    dash_config_get(_edit);
    if (_page >= _edit->page_count) _page = 0;
    bool locked = _edit->settings.locked;
    DashPage &pg = _edit->pages[_page];

    _mn = make_overlay(locked ? "Display (locked)" : "Page & display", "Done", on_mn_done, on_mn_cancel);
    lv_obj_t *body = _mn.body;
    lv_obj_t *row;

    if (!locked) {
        row = add_row(body, "Page name");
        _mn_name = add_textarea(_mn, row, pg.name, 340, false);
        lv_textarea_set_max_length(_mn_name, sizeof(pg.name) - 1);

        char lbuf[512] = {};
        for (int i = 0; i < DASH_LAYOUT_COUNT; i++) {
            strcat(lbuf, dash_layout_name(i));
            if (i < DASH_LAYOUT_COUNT - 1) strcat(lbuf, "\n");
        }
        row = add_row(body, "Layout");
        _mn_layout = add_dropdown(row, lbuf, pg.layout);

        row = add_row(body, "Page group");
        _mn_group = add_dropdown(row, "None\nAnchor\nSail", pg.group);

        row = add_row(body, nullptr);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(row, 10, 0);
        struct { const char *t; int a; uint32_t c; } acts[] = {
            { LV_SYMBOL_PLUS " Add page", ACT_ADD, 0x1f6fd0 },
            { LV_SYMBOL_COPY " Duplicate", ACT_DUP, 0x2a3642 },
            { LV_SYMBOL_LEFT " Move", ACT_LEFT, 0x2a3642 },
            { "Move " LV_SYMBOL_RIGHT, ACT_RIGHT, 0x2a3642 },
            { LV_SYMBOL_TRASH " Delete", ACT_DELETE, 0x8a2020 },
        };
        for (auto &a : acts) {
            lv_obj_t *b = add_button(row, a.t, on_mn_action, a.c);
            lv_obj_set_user_data(b, (void *)(intptr_t)a.a);
        }
    }

    lv_obj_t *sep = lv_label_create(body);
    lv_label_set_text(sep, "Display");
    lv_obj_set_style_text_font(sep, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(sep, lv_color_hex(0x3aa0ff), 0);

    row = add_row(body, "Theme");
    _mn_theme = add_dropdown(row, "Dark\nDay\nNight (red)\nAuto (Day / Dark at night)", _edit->settings.theme);

    lv_obj_t *bn = lv_label_create(body);
    lv_label_set_text(bn, "Brightness and night mode: Settings > Display");
    lv_obj_set_style_text_font(bn, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(bn, lv_color_hex(0x8a96a2), 0);

    if (!locked) {
        row = add_row(body, "Auto page cycle");
        _mn_cycle = add_dropdown(row, "Off\n5 s\n10 s\n15 s\n30 s\n60 s",
                                 option_index(s_cycle_opts, 6, _edit->settings.cycle_s));

        row = add_row(body, "No-data timeout");
        _mn_stale = add_dropdown(row, "3 s\n5 s\n10 s\n20 s\n30 s\n60 s",
                                 option_index(s_stale_opts, 6, _edit->settings.stale_s));

        char sbuf[DASH_MAX_PAGES * 22 + 16] = "Last viewed";
        for (int p = 0; p < _edit->page_count; p++) {
            strcat(sbuf, "\n");
            strncat(sbuf, _edit->pages[p].name, sizeof(sbuf) - strlen(sbuf) - 1);
        }
        row = add_row(body, "Start page");
        _mn_start = add_dropdown(row, sbuf, _edit->settings.start_page + 1);

        row = add_row(body, "Auto anchor / sail");
        _mn_auto = add_switch(row, _edit->settings.auto_switch);
        char abuf[128];
        snprintf(abuf, sizeof(abuf), "Below %.1f kn for %u min: Anchor pages. Above for %u s: Sail pages.",
                 _edit->settings.auto_kn, (unsigned)(_edit->settings.anchor_s + 30) / 60,
                 (unsigned)_edit->settings.sail_s);
        lv_obj_t *al = lv_label_create(body);
        lv_label_set_text(al, abuf);
        lv_obj_set_width(al, lv_pct(100));
        lv_label_set_long_mode(al, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(al, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(al, lv_color_hex(0x8a96a2), 0);
    }

    row = add_row(body, "Lock editing");
    _mn_lock = add_switch(row, locked);

    char url[64];
    char info[160];
    if (dash_web_url(url, sizeof(url))) {
        snprintf(info, sizeof(info), "Web editor: %s\nOpen on a phone or PC on the same network.", url);
    } else {
        snprintf(info, sizeof(info), "Web editor: available when Wi-Fi is connected.");
    }
    lv_obj_t *wl = lv_label_create(body);
    lv_label_set_text(wl, info);
    lv_obj_set_style_text_font(wl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(wl, lv_color_hex(0xc8d2dc), 0);

    if (!locked) {
        row = add_row(body, nullptr);
        lv_obj_t *b = add_button(row, "Reset all pages to defaults", on_mn_action, 0x8a2020);
        lv_obj_set_user_data(b, (void *)(intptr_t)ACT_RESET);
    }
}

void InstrumentsApp::read_menu()
{
    if (!_edit || !_mn.root) return;
    DashPage &pg = _edit->pages[_page];
    if (_mn_name) snprintf(pg.name, sizeof(pg.name), "%s", lv_textarea_get_text(_mn_name));
    if (_mn_layout) pg.layout = (uint8_t)lv_dropdown_get_selected(_mn_layout);
    if (_mn_group) pg.group = (uint8_t)lv_dropdown_get_selected(_mn_group);
    DashSettings &s = _edit->settings;
    if (_mn_auto) s.auto_switch = lv_obj_has_state(_mn_auto, LV_STATE_CHECKED);
    s.theme      = (uint8_t)lv_dropdown_get_selected(_mn_theme);
    if (_mn_cycle) s.cycle_s = s_cycle_opts[lv_dropdown_get_selected(_mn_cycle)];
    if (_mn_stale) s.stale_s = s_stale_opts[lv_dropdown_get_selected(_mn_stale)];
    if (_mn_start) s.start_page = (int8_t)((int)lv_dropdown_get_selected(_mn_start) - 1);
    s.locked = lv_obj_has_state(_mn_lock, LV_STATE_CHECKED);
}

void InstrumentsApp::apply_config(DashConfig *cfg, int goto_page)
{
    dash_config_set(cfg);
    dash_config_get(_cfg);
    _rev = dash_config_revision();
    _page = goto_page;
    if (_page >= _cfg->page_count) _page = _cfg->page_count - 1;
    if (_page < 0) _page = 0;
    build();
}

void InstrumentsApp::on_mn_done(lv_event_t *e)
{
    InstrumentsApp *app = (InstrumentsApp *)lv_event_get_user_data(e);
    app->read_menu();
    if (!app->_edit[0].pages[app->_page].name[0]) {
        snprintf(app->_edit->pages[app->_page].name, sizeof(app->_edit->pages[0].name),
                 "Page %d", app->_page + 1);
    }
    DashConfig *cfg = app->_edit;
    app->_edit = nullptr;
    app->close_overlay(app->_mn);
    app->apply_config(cfg, app->_page);
    free(cfg);
}

void InstrumentsApp::on_mn_cancel(lv_event_t *e)
{
    InstrumentsApp *app = e ? (InstrumentsApp *)lv_event_get_user_data(e) : nullptr;
    if (!app) return;
    free(app->_edit);
    app->_edit = nullptr;
    app->close_overlay(app->_mn);
}

void InstrumentsApp::on_mn_action(lv_event_t *e)
{
    InstrumentsApp *app = (InstrumentsApp *)lv_event_get_user_data(e);
    int action = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (action == ACT_DELETE) {
        if (app->_edit && app->_edit->page_count <= 1) {
            audio_feedback_invalid();
            return;
        }
        app->confirm("Delete this page?", ACT_DELETE);
        return;
    }
    if (action == ACT_RESET) {
        app->confirm("Replace all pages with the default dashboard?", ACT_RESET);
        return;
    }
    app->page_action(action);
}

void InstrumentsApp::confirm(const char *text, int action)
{
    static const char *btns[] = { "Cancel", "Yes", "" };
    _confirm = action;
    _mbox = lv_msgbox_create(NULL, "Confirm", text, btns, false);
    lv_obj_set_width(_mbox, 480);
    lv_obj_set_style_text_font(_mbox, &lv_font_montserrat_24, 0);
    lv_obj_center(_mbox);
    lv_obj_add_event_cb(_mbox, on_confirm, LV_EVENT_VALUE_CHANGED, this);
}

void InstrumentsApp::on_confirm(lv_event_t *e)
{
    InstrumentsApp *app = (InstrumentsApp *)lv_event_get_user_data(e);
    lv_obj_t *mbox = lv_event_get_current_target(e);
    bool yes = lv_msgbox_get_active_btn(mbox) == 1;
    lv_msgbox_close(mbox);
    app->_mbox = nullptr;
    if (yes) app->page_action(app->_confirm);
    app->_confirm = 0;
}

void InstrumentsApp::page_action(int action)
{
    if (!_edit) return;
    if (action == ACT_RESET) {
        free(_edit);
        _edit = nullptr;
        close_overlay(_mn);
        dash_config_reset();
        dash_config_get(_cfg);
        _rev = dash_config_revision();
        _page = 0;
        build();
        return;
    }

    read_menu();
    DashConfig *c = _edit;
    int p = _page;
    int goto_page = p;
    switch (action) {
    case ACT_ADD:
    case ACT_DUP:
        if (c->page_count >= DASH_MAX_PAGES) {
            audio_feedback_invalid();
            return;
        }
        memmove(&c->pages[p + 2], &c->pages[p + 1], (c->page_count - p - 1) * sizeof(DashPage));
        if (action == ACT_ADD) {
            char name[20];
            snprintf(name, sizeof(name), "Page %d", c->page_count + 1);
            dash_page_init(&c->pages[p + 1], name, DASH_LAYOUT_2X2);
        } else {
            c->pages[p + 1] = c->pages[p];
        }
        c->page_count++;
        goto_page = p + 1;
        break;
    case ACT_DELETE:
        if (c->page_count <= 1) return;
        memmove(&c->pages[p], &c->pages[p + 1], (c->page_count - p - 1) * sizeof(DashPage));
        c->page_count--;
        goto_page = p > 0 ? p - 1 : 0;
        c->settings.start_page = -1;
        break;
    case ACT_LEFT:
    case ACT_RIGHT: {
        int q = action == ACT_LEFT ? p - 1 : p + 1;
        if (q < 0 || q >= c->page_count) {
            audio_feedback_invalid();
            return;
        }
        DashPage *tmp = (DashPage *)malloc(sizeof(DashPage));
        if (!tmp) return;
        *tmp = c->pages[p];
        c->pages[p] = c->pages[q];
        c->pages[q] = *tmp;
        free(tmp);
        goto_page = q;
        break;
    }
    default:
        return;
    }
    audio_feedback_click();
    _edit = nullptr;
    close_overlay(_mn);
    apply_config(c, goto_page);
    free(c);
}

void InstrumentsApp::on_gear(lv_event_t *e)
{
    InstrumentsApp *app = (InstrumentsApp *)lv_event_get_user_data(e);
    if (app->overlay_open()) return;
    audio_feedback_click();
    app->open_menu();
}
