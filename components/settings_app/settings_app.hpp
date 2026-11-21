#pragma once

#include "esp_ui.hpp"
#include "lvgl.h"
#include "unit_config.hpp"

/* Number of categories shown in settings */
#define SETTINGS_VISIBLE_CATS 8  /* TZ, Speed, Depth, Temp, Distance, Pressure, Volume, Startup app */

class SettingsApp : public ESP_UI_PhoneApp {
public:
    SettingsApp();
    ~SettingsApp() override;

    bool run()    override;
    bool back()   override;
    bool close()  override;
    bool pause()  override { return true; }
    bool resume() override { return true; }

protected:
    void build_ui();
    void show_main_view();
    void show_picker(unit_category_t cat);
    void populate_picker();
    void update_row_labels();
    void build_display_view();
    void show_display_view();
    void update_display_status();

    static void on_display_row_click(lv_event_t *e);
    static void on_display_changed(lv_event_t *e);
    static void on_status_timer(lv_timer_t *t);
    static void on_row_click(lv_event_t *e);
    static void on_picker_select(lv_event_t *e);
    static void on_picker_back(lv_event_t *e);

    lv_obj_t *_main_view       = nullptr;
    lv_obj_t *_picker_view     = nullptr;
    lv_obj_t *_picker_title    = nullptr;
    lv_obj_t *_picker_list     = nullptr;

    /* Display page (panel-wide brightness and day/night) */
    lv_obj_t *_display_view    = nullptr;
    lv_obj_t *_display_value   = nullptr;   // summary on the main row
    lv_obj_t *_dn_mode         = nullptr;
    lv_obj_t *_dn_day          = nullptr;
    lv_obj_t *_dn_day_lbl      = nullptr;
    lv_obj_t *_dn_night        = nullptr;
    lv_obj_t *_dn_night_lbl    = nullptr;
    lv_obj_t *_dn_red          = nullptr;
    lv_obj_t *_dn_switch       = nullptr;
    lv_obj_t *_dn_status       = nullptr;
    lv_timer_t *_status_timer  = nullptr;
    bool _loading_display      = false;
    /* One value label per settings row */
    lv_obj_t *_row_value_lbl[SETTINGS_VISIBLE_CATS] = {};

    /* Which categories to show, in order */
    static const unit_category_t s_visible_cats[SETTINGS_VISIBLE_CATS];

    unit_category_t _picker_category = UNIT_CAT_TIMEZONE;
    bool _showing_picker = false;

private:
};
