#pragma once

#include "esp_ui.hpp"
#include "dash_model.hpp"

// ---------------------------------------------------------------------------
// InstrumentsApp — configurable multi-page numeric instrument display
// (in the style of Raymarine i70 / B&G Triton2).
//
//  - Swipe left/right between pages, each page has its own grid layout
//  - Long-press a tile to choose the data path, label, units, alarms ...
//  - Tap a tile to acknowledge an alarm or toggle the min/max line
//  - Gear button: page layout, add/remove pages, theme, lock (brightness is in Settings > Display)
//  - The same configuration is editable from a browser (dash_web)
// ---------------------------------------------------------------------------

class InstrumentsApp : public ESP_UI_PhoneApp {
public:
    InstrumentsApp();
    ~InstrumentsApp();

protected:
    bool run()    override;
    bool back()   override;
    bool close()  override;
    bool pause()  override;
    bool resume() override;

private:
    struct TileUi {
        lv_obj_t        *cont;
        lv_obj_t        *name;
        lv_obj_t        *unit;
        lv_obj_t        *value;
        lv_obj_t        *minmax;
        lv_obj_t        *chart;
        lv_chart_series_t *series;
        lv_obj_t        *gfx;          // custom-drawn tile (tide curve, sun, solunar, wind rose)
        uint32_t         gfx_sig;      // content signature wanted now
        uint32_t         cache_sig;    // signature rendered into the cache
        uint32_t         cache_ms;     // when the cache was rendered
        uint8_t         *cache_buf;    // pre-rendered RGB565 image (PSRAM)
        lv_img_dsc_t     cache_dsc;
        bool             cache_ok;
        bool             cache_fail;   // allocation failed: draw live
        int16_t          w, h;
        int16_t          value_top, value_h;
        char             pattern[16];
        char             text[16];
        int8_t           font_idx;
        uint16_t         hist_sig;
        uint32_t         color;
        bool             show_minmax;
        bool             flash;
    };

    struct Overlay {
        lv_obj_t *root;
        lv_obj_t *body;
        lv_obj_t *kb;
    };

    // Model
    DashConfig *_cfg  = nullptr;   // rendered configuration
    DashConfig *_edit = nullptr;   // working copy for the page menu
    uint32_t    _rev  = 0;
    bool        _night = false;   // day_night state the tiles were built with
    int         _page = 0;
    bool        _auto_page_change = false;
    uint32_t    _last_cycle_ms = 0;
    uint32_t    _tick = 0;
    uint32_t    _motion_seq = 0;   // last anchor/sail change acted on

    // Widgets
    lv_obj_t   *_screen   = nullptr;
    lv_obj_t   *_root     = nullptr;
    lv_obj_t   *_title    = nullptr;
    lv_obj_t   *_dots     = nullptr;
    lv_obj_t   *_conn     = nullptr;
    lv_obj_t   *_gear     = nullptr;
    lv_obj_t   *_tv       = nullptr;
    lv_obj_t   *_pages[DASH_MAX_PAGES] = {};
    TileUi      _tiles[DASH_MAX_PAGES][DASH_MAX_TILES] = {};
    lv_timer_t *_timer    = nullptr;
    int16_t     _area_w = 0, _area_h = 0, _area_x = 0, _area_y = 0;

    // Tile editor
    Overlay     _ed = {};
    int         _ed_page = -1, _ed_slot = -1;
    DashTile    _ed_tile = {};
    lv_obj_t   *_ed_path = nullptr, *_ed_label = nullptr, *_ed_unit = nullptr;
    lv_obj_t   *_ed_dec = nullptr, *_ed_angle = nullptr, *_ed_smooth = nullptr;
    lv_obj_t   *_ed_trend = nullptr, *_ed_lo_sw = nullptr, *_ed_lo = nullptr;
    lv_obj_t   *_ed_hi_sw = nullptr, *_ed_hi = nullptr, *_ed_beep = nullptr;

    // Path picker
    Overlay     _pk = {};
    lv_obj_t   *_pk_search = nullptr, *_pk_list = nullptr, *_pk_status = nullptr;
    uint32_t    _pk_gen = 0;
    char        _pk_filter[64] = {};
    bool        _pk_dirty = false;

    // Page menu
    Overlay     _mn = {};
    lv_obj_t   *_mn_name = nullptr, *_mn_layout = nullptr, *_mn_theme = nullptr;
    lv_obj_t   *_mn_group = nullptr, *_mn_auto = nullptr;
    lv_obj_t   *_mn_cycle = nullptr, *_mn_stale = nullptr;
    lv_obj_t   *_mn_start = nullptr, *_mn_lock = nullptr;
    int         _confirm = 0;
    lv_obj_t   *_mbox = nullptr;

    // Build / refresh
    void build();
    void destroy_pages();
    void build_page(int p);
    void update_header();
    void update_tile(int p, int i, bool visible);
    bool render_gfx_cache(int p, int i);
    void refresh_gfx_caches();
    void free_gfx_caches();
    void fit_value(TileUi &u, const char *text);
    void update_chart(TileUi &u, const DashTile &t, const struct DashTileState *st);
    void apply_theme_to_tile(TileUi &u, bool alarm_bg);
    void go_to_page(int p, bool anim);
    bool overlay_open() const;

    // Overlays
    Overlay make_overlay(const char *title, const char *ok_text,
                         lv_event_cb_t ok_cb, lv_event_cb_t cancel_cb);
    void    close_overlay(Overlay &o);
    lv_obj_t *add_row(lv_obj_t *body, const char *label);
    lv_obj_t *add_dropdown(lv_obj_t *row, const char *options, int sel);
    lv_obj_t *add_textarea(Overlay &o, lv_obj_t *row, const char *text, int width, bool numeric);
    lv_obj_t *add_switch(lv_obj_t *row, bool on);
    lv_obj_t *add_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, uint32_t color);

    void open_editor(int page, int slot, const DashTile *tile);
    void read_editor();
    void save_editor();
    void open_picker();
    void fill_picker();
    void choose_path(const char *path);
    void open_menu();
    void read_menu();
    void apply_config(DashConfig *cfg, int goto_page);
    void page_action(int action);
    void confirm(const char *text, int action);

    // Event handlers
    static void on_timer(lv_timer_t *t);
    static void on_tv_changed(lv_event_t *e);
    static void on_tile_event(lv_event_t *e);
    static void on_gfx_draw(lv_event_t *e);
    static void on_gear(lv_event_t *e);
    static void on_ta_clicked(lv_event_t *e);
    static void on_kb_event(lv_event_t *e);
    static void on_ed_save(lv_event_t *e);
    static void on_ed_cancel(lv_event_t *e);
    static void on_ed_path(lv_event_t *e);
    static void on_ed_clear(lv_event_t *e);
    static void on_ed_reset_minmax(lv_event_t *e);
    static void on_pk_cancel(lv_event_t *e);
    static void on_pk_search(lv_event_t *e);
    static void on_pk_item(lv_event_t *e);
    static void on_pk_typed(lv_event_t *e);
    static void on_mn_done(lv_event_t *e);
    static void on_mn_cancel(lv_event_t *e);
    static void on_mn_action(lv_event_t *e);
    static void on_confirm(lv_event_t *e);
};
