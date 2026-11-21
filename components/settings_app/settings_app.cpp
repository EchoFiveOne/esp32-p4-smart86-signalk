#include "settings_app.hpp"
#include "day_night.hpp"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "settings";

LV_IMG_DECLARE(settings_gear_icon);

/* Categories displayed in settings, in order */
const unit_category_t SettingsApp::s_visible_cats[SETTINGS_VISIBLE_CATS] = {
    UNIT_CAT_TIMEZONE,
    UNIT_CAT_SPEED,
    UNIT_CAT_DEPTH,
    UNIT_CAT_TEMPERATURE,
    UNIT_CAT_DISTANCE,
    UNIT_CAT_PRESSURE,
    UNIT_CAT_VOLUME,
    UNIT_CAT_STARTUP,
};

/* ── Constructor ─────────────────────────────────────────────────────── */

SettingsApp::SettingsApp()
    : ESP_UI_PhoneApp("Settings",
                      &settings_gear_icon,
                      true, false, false)
{
}

SettingsApp::~SettingsApp()
{
    ESP_LOGD(TAG, "~SettingsApp");
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

bool SettingsApp::run()
{
    ESP_LOGI(TAG, "SettingsApp::run()");
    _showing_picker = false;
    build_ui();
    show_main_view();
    _status_timer = lv_timer_create(on_status_timer, 1000, this);
    return true;
}

bool SettingsApp::close()
{
    // Screen objects and timers created in run() are deleted by esp-ui
    _status_timer = nullptr;
    _main_view = _picker_view = _picker_title = _picker_list = nullptr;
    _display_view = _display_value = _dn_mode = _dn_day = _dn_day_lbl = nullptr;
    _dn_night = _dn_night_lbl = _dn_red = _dn_switch = _dn_status = nullptr;
    for (auto &l : _row_value_lbl) l = nullptr;
    _showing_picker = false;
    return true;
}

bool SettingsApp::back()
{
    if (_showing_picker) {
        show_main_view();
        return true;
    }
    return notifyCoreClosed();
}

/* ── UI Construction ─────────────────────────────────────────────────── */

void SettingsApp::build_ui()
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ===== MAIN VIEW ===== */
    _main_view = lv_obj_create(scr);
    lv_obj_set_size(_main_view, 720, 720);
    lv_obj_align(_main_view, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(_main_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_main_view, 0, 0);
    lv_obj_set_style_pad_all(_main_view, 0, 0);

    lv_obj_t *title = lv_label_create(_main_view);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    /* Settings rows */
    for (int i = 0; i < SETTINGS_VISIBLE_CATS; i++) {
        unit_category_t cat = s_visible_cats[i];
        int y = 70 + i * 68;

        lv_obj_t *row = lv_obj_create(_main_view);
        lv_obj_set_size(row, 680, 58);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x252540), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void *)(intptr_t)cat);
        lv_obj_add_event_cb(row, on_row_click, LV_EVENT_CLICKED, this);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, unit_category_name(cat));
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 16, 0);

        _row_value_lbl[i] = lv_label_create(row);
        lv_label_set_text(_row_value_lbl[i], unit_option_name(cat));
        lv_obj_set_style_text_font(_row_value_lbl[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(_row_value_lbl[i], lv_color_hex(0x888888), 0);
        lv_obj_align(_row_value_lbl[i], LV_ALIGN_RIGHT_MID, -40, 0);

        lv_obj_t *chevron = lv_label_create(row);
        lv_label_set_text(chevron, ">");
        lv_obj_set_style_text_font(chevron, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(chevron, lv_color_hex(0x555555), 0);
        lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -12, 0);
    }

    /* Panel display row (brightness, day/night) */
    {
        int y = 70 + SETTINGS_VISIBLE_CATS * 68;
        lv_obj_t *row = lv_obj_create(_main_view);
        lv_obj_set_size(row, 680, 58);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x252540), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_display_row_click, LV_EVENT_CLICKED, this);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Display");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 16, 0);

        _display_value = lv_label_create(row);
        lv_label_set_text(_display_value, "");
        lv_obj_set_style_text_font(_display_value, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(_display_value, lv_color_hex(0x888888), 0);
        lv_obj_align(_display_value, LV_ALIGN_RIGHT_MID, -40, 0);

        lv_obj_t *chevron = lv_label_create(row);
        lv_label_set_text(chevron, ">");
        lv_obj_set_style_text_font(chevron, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(chevron, lv_color_hex(0x555555), 0);
        lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -12, 0);
    }

    build_display_view();

    /* ===== PICKER VIEW ===== */
    _picker_view = lv_obj_create(scr);
    lv_obj_set_size(_picker_view, 720, 720);
    lv_obj_align(_picker_view, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(_picker_view, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(_picker_view, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_picker_view, 0, 0);
    lv_obj_set_style_pad_all(_picker_view, 0, 0);
    lv_obj_clear_flag(_picker_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_picker_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back_btn = lv_btn_create(_picker_view);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 10, 12);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x3498db), 0);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_add_event_cb(back_btn, on_picker_back, LV_EVENT_CLICKED, this);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(back_lbl);

    _picker_title = lv_label_create(_picker_view);
    lv_label_set_text(_picker_title, "");
    lv_obj_set_style_text_font(_picker_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_picker_title, lv_color_white(), 0);
    lv_obj_align(_picker_title, LV_ALIGN_TOP_MID, 0, 18);

    _picker_list = lv_list_create(_picker_view);
    lv_obj_set_size(_picker_list, 700, 650);
    lv_obj_align(_picker_list, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(_picker_list, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(_picker_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_picker_list, 0, 0);
    lv_obj_set_style_pad_all(_picker_list, 4, 0);
}

/* ── View switching ──────────────────────────────────────────────────── */

void SettingsApp::show_main_view()
{
    _showing_picker = false;
    update_row_labels();
    update_display_status();
    lv_obj_add_flag(_picker_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_display_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_main_view, LV_OBJ_FLAG_HIDDEN);
}

void SettingsApp::show_picker(unit_category_t cat)
{
    _picker_category = cat;
    _showing_picker = true;

    lv_label_set_text(_picker_title, unit_category_name(cat));
    populate_picker();

    lv_obj_add_flag(_main_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_picker_view, LV_OBJ_FLAG_HIDDEN);
}

void SettingsApp::populate_picker()
{
    lv_obj_clean(_picker_list);

    int count = unit_option_count(_picker_category);
    int current = unit_config_get(_picker_category);

    for (int i = 0; i < count; i++) {
        char label[64];
        if (i == current) {
            snprintf(label, sizeof(label), LV_SYMBOL_OK "  %s", unit_option_name_at(_picker_category, i));
        } else {
            snprintf(label, sizeof(label), "    %s", unit_option_name_at(_picker_category, i));
        }

        lv_obj_t *btn = lv_list_add_btn(_picker_list, NULL, label);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x252540), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), lv_color_white(), 0);
        lv_obj_set_style_text_font(lv_obj_get_child(btn, 0), &lv_font_montserrat_24, 0);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, on_picker_select, LV_EVENT_CLICKED, this);
    }
}

void SettingsApp::update_row_labels()
{
    for (int i = 0; i < SETTINGS_VISIBLE_CATS; i++) {
        if (_row_value_lbl[i]) {
            lv_label_set_text(_row_value_lbl[i], unit_option_name(s_visible_cats[i]));
        }
    }
}

/* ── Event Callbacks ─────────────────────────────────────────────────── */

void SettingsApp::on_row_click(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    auto *app = (SettingsApp *)lv_event_get_user_data(e);
    unit_category_t cat = (unit_category_t)(intptr_t)lv_obj_get_user_data(row);
    app->show_picker(cat);
}

void SettingsApp::on_picker_select(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    auto *app = (SettingsApp *)lv_event_get_user_data(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);

    unit_config_set(app->_picker_category, idx);
    ESP_LOGI(TAG, "Set %s = %s", unit_category_name(app->_picker_category),
             unit_option_name(app->_picker_category));

    app->show_main_view();
}

void SettingsApp::on_picker_back(lv_event_t *e)
{
    auto *app = (SettingsApp *)lv_event_get_user_data(e);
    app->show_main_view();
}

/* ── Display page ────────────────────────────────────────────────────── */

static lv_obj_t *dn_section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    return l;
}

static lv_obj_t *dn_buttons(lv_obj_t *parent, const char **map, int checked, lv_event_cb_t cb, void *app)
{
    lv_obj_t *m = lv_btnmatrix_create(parent);
    lv_btnmatrix_set_map(m, map);
    lv_btnmatrix_set_btn_ctrl_all(m, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(m, true);
    lv_btnmatrix_set_btn_ctrl(m, checked, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_set_size(m, 660, 70);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_24, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, lv_color_hex(0x252540), LV_PART_ITEMS);
    lv_obj_set_style_text_color(m, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, lv_color_hex(0x3498db), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(m, cb, LV_EVENT_VALUE_CHANGED, app);
    return m;
}

static lv_obj_t *dn_slider(lv_obj_t *parent, int min, int max, int val, lv_obj_t **lbl,
                           lv_event_cb_t cb, void *app)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 660, 60);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *s = lv_slider_create(row);
    lv_obj_set_size(s, 520, 24);
    lv_obj_align(s, LV_ALIGN_LEFT_MID, 16, 0);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, app);
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, app);
    *lbl = lv_label_create(row);
    lv_obj_set_style_text_font(*lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(*lbl, lv_color_white(), 0);
    lv_obj_align(*lbl, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_label_set_text_fmt(*lbl, "%d %%", val);
    return s;
}

static const char *s_mode_map[]   = { "Auto (sun)", "Day", "Night", "" };
static const char *s_switch_map[] = { "Sunset", "Civil -6", "Nautical -12", "" };
static const int8_t s_switch_vals[] = { 0, -6, -12 };

void SettingsApp::build_display_view()
{
    lv_obj_t *scr = lv_scr_act();
    DayNightSettings s;
    day_night_get_settings(&s);
    _loading_display = true;

    _display_view = lv_obj_create(scr);
    lv_obj_set_size(_display_view, 720, 720);
    lv_obj_align(_display_view, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(_display_view, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(_display_view, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_display_view, 0, 0);
    lv_obj_set_style_radius(_display_view, 0, 0);
    lv_obj_set_style_pad_all(_display_view, 0, 0);
    lv_obj_add_flag(_display_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back_btn = lv_btn_create(_display_view);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 10, 12);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x3498db), 0);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_add_event_cb(back_btn, on_picker_back, LV_EVENT_CLICKED, this);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(back_lbl);

    lv_obj_t *title = lv_label_create(_display_view);
    lv_label_set_text(title, "Display (all apps)");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_t *body = lv_obj_create(_display_view);
    lv_obj_set_size(body, 720, 650);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_hor(body, 30, 0);
    lv_obj_set_style_pad_ver(body, 6, 0);
    lv_obj_set_style_pad_row(body, 12, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    dn_section(body, "Day / night mode");
    _dn_mode = dn_buttons(body, s_mode_map, s.mode % DN_MODE_COUNT, on_display_changed, this);

    dn_section(body, "Day brightness");
    _dn_day = dn_slider(body, 5, 100, s.day_pct, &_dn_day_lbl, on_display_changed, this);
    dn_section(body, "Night brightness");
    _dn_night = dn_slider(body, 1, 100, s.night_pct, &_dn_night_lbl, on_display_changed, this);

    lv_obj_t *red_row = lv_obj_create(body);
    lv_obj_set_size(red_row, 660, 56);
    lv_obj_set_style_bg_opa(red_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(red_row, 0, 0);
    lv_obj_set_style_pad_all(red_row, 0, 0);
    lv_obj_clear_flag(red_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *rl = dn_section(red_row, "Red night filter");
    lv_obj_align(rl, LV_ALIGN_LEFT_MID, 0, 0);
    _dn_red = lv_switch_create(red_row);
    lv_obj_set_size(_dn_red, 90, 46);
    lv_obj_align(_dn_red, LV_ALIGN_RIGHT_MID, -4, 0);
    if (s.red_filter) lv_obj_add_state(_dn_red, LV_STATE_CHECKED);
    lv_obj_add_event_cb(_dn_red, on_display_changed, LV_EVENT_VALUE_CHANGED, this);

    dn_section(body, "Night starts at sun elevation");
    int sw = 0;
    for (int i = 0; i < 3; i++) if (s_switch_vals[i] == s.switch_elev) sw = i;
    _dn_switch = dn_buttons(body, s_switch_map, sw, on_display_changed, this);

    _dn_status = lv_label_create(body);
    lv_obj_set_width(_dn_status, 660);
    lv_label_set_long_mode(_dn_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_dn_status, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_dn_status, lv_color_hex(0xaab4c8), 0);
    lv_label_set_text(_dn_status, "");

    _loading_display = false;
}

void SettingsApp::show_display_view()
{
    _showing_picker = true;   // Back returns to the main list
    update_display_status();
    lv_obj_add_flag(_main_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_picker_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_display_view, LV_OBJ_FLAG_HIDDEN);
}

static void fmt_hm(char *out, size_t len, int s)
{
    if (s < 0) snprintf(out, len, "--:--");
    else snprintf(out, len, "%02d:%02d", s / 3600, (s / 60) % 60);
}

void SettingsApp::update_display_status()
{
    DayNightSettings s;
    DayNightStatus st;
    day_night_get_settings(&s);
    day_night_get_status(&st);

    if (_display_value) {
        static const char *modes[] = { "Auto", "Day", "Night" };
        lv_label_set_text_fmt(_display_value, "%s - %s now, %d%%", modes[s.mode % DN_MODE_COUNT],
                              st.night ? "night" : "day", st.level);
    }
    if (!_dn_status || lv_obj_has_flag(_display_view, LV_OBJ_FLAG_HIDDEN)) return;

    char buf[400];
    int n = snprintf(buf, sizeof(buf), "Now: %s, brightness %d%%\n", st.night ? "NIGHT" : "DAY", st.level);
    if (st.sun_valid) {
        char rise[8], set[8];
        fmt_hm(rise, sizeof(rise), st.sunrise_s);
        fmt_hm(set, sizeof(set), st.sunset_s);
        n += snprintf(buf + n, sizeof(buf) - n, "Sun %.1f deg, sunrise %s, sunset %s\n", st.elevation, rise, set);
    } else {
        n += snprintf(buf + n, sizeof(buf) - n, "Sun: needs time and position\n");
    }
    if (st.time_valid) {
        time_t now = time(nullptr);
        struct tm lt;
        localtime_r(&now, &lt);
        n += snprintf(buf + n, sizeof(buf) - n, "Local time %02d:%02d (%s), clock from %s\n",
                      lt.tm_hour, lt.tm_min, st.tz_name[0] ? st.tz_name : "manual zone",
                      st.time_source == 1 ? "NTP" : "Signal K");
    } else {
        n += snprintf(buf + n, sizeof(buf) - n, "Clock not set (no NTP or Signal K time)\n");
    }
    if (st.pos_valid) {
        snprintf(buf + n, sizeof(buf) - n, "Position %.3f, %.3f%s", st.lat, st.lon,
                 st.pos_live ? "" : " (last known)");
    } else {
        snprintf(buf + n, sizeof(buf) - n, "No position yet (navigation.position)");
    }
    lv_label_set_text(_dn_status, buf);
}

void SettingsApp::on_display_row_click(lv_event_t *e)
{
    ((SettingsApp *)lv_event_get_user_data(e))->show_display_view();
}

void SettingsApp::on_display_changed(lv_event_t *e)
{
    auto *app = (SettingsApp *)lv_event_get_user_data(e);
    if (!app || app->_loading_display || !app->_display_view) return;
    DayNightSettings s;
    day_night_get_settings(&s);
    s.mode = (uint8_t)lv_btnmatrix_get_selected_btn(app->_dn_mode);
    if (s.mode >= DN_MODE_COUNT) s.mode = DN_MODE_AUTO;
    s.day_pct = (uint8_t)lv_slider_get_value(app->_dn_day);
    s.night_pct = (uint8_t)lv_slider_get_value(app->_dn_night);
    s.red_filter = lv_obj_has_state(app->_dn_red, LV_STATE_CHECKED);
    uint16_t sw = lv_btnmatrix_get_selected_btn(app->_dn_switch);
    if (sw < 3) s.switch_elev = s_switch_vals[sw];
    lv_label_set_text_fmt(app->_dn_day_lbl, "%d %%", s.day_pct);
    lv_label_set_text_fmt(app->_dn_night_lbl, "%d %%", s.night_pct);
    // Save only on release so dragging a slider does not hammer NVS
    lv_obj_t *target = lv_event_get_target(e);
    bool dragging = (target == app->_dn_day || target == app->_dn_night) &&
                    lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED && lv_slider_is_dragged(target);
    day_night_set_settings(&s, !dragging);
    app->update_display_status();
}

void SettingsApp::on_status_timer(lv_timer_t *t)
{
    auto *app = (SettingsApp *)t->user_data;
    if (app && app->_display_view) app->update_display_status();
}