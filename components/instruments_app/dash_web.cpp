#include "dash_web.hpp"
#include "dash_model.hpp"
#include "dash_engine.hpp"
#include "sk_catalog.hpp"
#include "signalk_client.hpp"
#include "day_night.hpp"
#include "tides.hpp"
#include "unit_config.hpp"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "cJSON.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "dash_web";

extern const char editor_html_start[] asm("_binary_dash_editor_html_start");
extern const char editor_html_end[]   asm("_binary_dash_editor_html_end");

static httpd_handle_t s_server = nullptr;
static const size_t MAX_BODY = 128 * 1024;

bool dash_web_url(char *out, size_t len)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) return false;
    snprintf(out, len, "http://" IPSTR "/", IP2STR(&ip.ip));
    return true;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *txt = root ? cJSON_PrintUnformatted(root) : nullptr;
    cJSON_Delete(root);
    if (!txt) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);
    cJSON_free(txt);
    return err;
}

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, editor_html_start, editor_html_end - editor_html_start);
}

static esp_err_t h_config_get(httpd_req_t *req)
{
    DashConfig *cfg = (DashConfig *)malloc(sizeof(DashConfig));
    if (!cfg) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    dash_config_get(cfg);
    cJSON *root = dash_config_to_json(cfg);
    cJSON_AddNumberToObject(root, "revision", dash_config_revision());
    free(cfg);
    return send_json(req, root);
}

static esp_err_t h_config_put(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
    }
    char *buf = (char *)heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        got += r;
    }
    buf[got] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    DashConfig *cfg = (DashConfig *)malloc(sizeof(DashConfig));
    bool ok = root && cfg && dash_config_from_json(root, cfg);
    cJSON_Delete(root);
    if (!ok) {
        free(cfg);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid dashboard JSON");
    }
    dash_config_set(cfg);
    free(cfg);
    ESP_LOGI(TAG, "Configuration updated from web editor");
    return h_config_get(req);
}

static esp_err_t h_reset(httpd_req_t *req)
{
    dash_config_reset();
    return h_config_get(req);
}

static esp_err_t h_paths(httpd_req_t *req)
{
    char q[32];
    bool force = false;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[8];
        force = httpd_query_key_value(q, "refresh", v, sizeof(v)) == ESP_OK && v[0] == '1';
    }
    sk_catalog_refresh(force);
    return send_json(req, sk_catalog_to_json());
}

static esp_err_t h_units(httpd_req_t *req)
{
    return send_json(req, dash_units_to_json());
}

static esp_err_t h_values(httpd_req_t *req)
{
    DashConfig *cfg = (DashConfig *)malloc(sizeof(DashConfig));
    if (!cfg) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    dash_config_get(cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", signalk_client_is_connected());
    cJSON_AddNumberToObject(root, "revision", dash_config_revision());
    DashMotion m;
    dash_engine_motion(&m);
    if (m.enabled) {
        cJSON *jm = cJSON_AddObjectToObject(root, "motion");
        cJSON_AddStringToObject(jm, "state", dash_group_id(m.state));
        if (m.sog_valid) cJSON_AddNumberToObject(jm, "sog", roundf(m.sog_kn * 10.0f) / 10.0);
        if (m.pending) {
            cJSON_AddStringToObject(jm, "pending", dash_group_id(m.pending));
            cJSON_AddNumberToObject(jm, "pendingS", m.pending_s);
            cJSON_AddNumberToObject(jm, "needS", m.need_s);
        }
    }
    cJSON *pages = cJSON_AddArrayToObject(root, "pages");

    dash_engine_lock();
    for (int p = 0; p < cfg->page_count; p++) {
        cJSON *jp = cJSON_CreateArray();
        int n = dash_layout_tile_count(cfg->pages[p].layout);
        for (int i = 0; i < n; i++) {
            const DashTile *t = &cfg->pages[p].tiles[i];
            const DashTileState *st = dash_engine_state(p, i);
            cJSON *jt = cJSON_CreateObject();
            bool valid = t->path[0] && st && st->has_value && !st->stale &&
                         strcmp(st->path, t->path) == 0;
            DashFormatted f;
            dash_format(t, valid ? st->value_si : 0.0f, &f);
            cJSON_AddStringToObject(jt, "v", valid ? f.value : (t->path[0] ? "---" : ""));
            cJSON_AddStringToObject(jt, "u", f.unit);
            cJSON_AddNumberToObject(jt, "side", valid ? f.side : 0);
            cJSON_AddBoolToObject(jt, "ok", valid);
            if (valid && st->alarm) {
                cJSON_AddStringToObject(jt, "alarm", st->alarm == DASH_ALARM_LOW ? "low" : "high");
                cJSON_AddBoolToObject(jt, "acked", st->acked);
            }
            if (valid && st->minmax_valid) {
                cJSON_AddNumberToObject(jt, "min", st->min_disp);
                cJSON_AddNumberToObject(jt, "max", st->max_disp);
            }
            cJSON_AddItemToArray(jp, jt);
        }
        cJSON_AddItemToArray(pages, jp);
    }
    dash_engine_unlock();
    free(cfg);
    return send_json(req, root);
}

// --- Panel display (all apps): brightness, day/night, red filter ------------------
static const char *s_dn_modes[DN_MODE_COUNT] = { "auto", "day", "night" };

static esp_err_t h_display_get(httpd_req_t *req)
{
    DayNightSettings s;
    DayNightStatus st;
    day_night_get_settings(&s);
    day_night_get_status(&st);
    cJSON *root = cJSON_CreateObject();
    cJSON *js = cJSON_AddObjectToObject(root, "settings");
    cJSON_AddStringToObject(js, "mode", s_dn_modes[s.mode % DN_MODE_COUNT]);
    cJSON_AddNumberToObject(js, "day", s.day_pct);
    cJSON_AddNumberToObject(js, "night", s.night_pct);
    cJSON_AddBoolToObject(js, "red", s.red_filter);
    cJSON_AddNumberToObject(js, "switch", s.switch_elev);
    cJSON_AddStringToObject(js, "timeZone", unit_option_name(UNIT_CAT_TIMEZONE));
    cJSON *jz = cJSON_AddArrayToObject(js, "timeZones");
    for (int i = 0; i < unit_option_count(UNIT_CAT_TIMEZONE); i++) {
        cJSON_AddItemToArray(jz, cJSON_CreateString(unit_option_name_at(UNIT_CAT_TIMEZONE, i)));
    }
    cJSON *jt = cJSON_AddObjectToObject(root, "status");
    cJSON_AddBoolToObject(jt, "night", st.night);
    cJSON_AddNumberToObject(jt, "level", st.level);
    cJSON_AddStringToObject(jt, "timeSource",
                            st.time_source == 1 ? "ntp" : (st.time_source == 2 ? "signalk" : "none"));
    if (st.pos_valid) {
        cJSON_AddNumberToObject(jt, "lat", st.lat);
        cJSON_AddNumberToObject(jt, "lon", st.lon);
        cJSON_AddBoolToObject(jt, "posLive", st.pos_live);
    }
    if (st.sun_valid) {
        cJSON_AddNumberToObject(jt, "sunElevation", st.elevation);
        if (st.sunrise_s >= 0) cJSON_AddNumberToObject(jt, "sunrise", st.sunrise_s);
        if (st.sunset_s >= 0)  cJSON_AddNumberToObject(jt, "sunset", st.sunset_s);
    }
    cJSON_AddStringToObject(jt, "tzName", st.tz_name);
    cJSON_AddStringToObject(jt, "tz", st.tz_posix);
    float lt;
    if (day_night_virtual_value("panel.time.local", &lt)) cJSON_AddNumberToObject(jt, "localTime", lt);
    return send_json(req, root);
}

static esp_err_t h_display_put(httpd_req_t *req)
{
    char buf[256];
    if (req->content_len == 0 || req->content_len >= sizeof(buf)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    buf[got] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }
    DayNightSettings s;
    day_night_get_settings(&s);
    cJSON *j = cJSON_GetObjectItem(root, "mode");
    if (cJSON_IsString(j)) {
        for (int i = 0; i < DN_MODE_COUNT; i++) {
            if (strcmp(j->valuestring, s_dn_modes[i]) == 0) s.mode = (uint8_t)i;
        }
    }
    j = cJSON_GetObjectItem(root, "day");
    if (cJSON_IsNumber(j)) s.day_pct = (uint8_t)j->valueint;
    j = cJSON_GetObjectItem(root, "night");
    if (cJSON_IsNumber(j)) s.night_pct = (uint8_t)j->valueint;
    j = cJSON_GetObjectItem(root, "red");
    if (cJSON_IsBool(j)) s.red_filter = cJSON_IsTrue(j);
    j = cJSON_GetObjectItem(root, "switch");
    if (cJSON_IsNumber(j)) s.switch_elev = (int8_t)j->valueint;
    j = cJSON_GetObjectItem(root, "timeZone");
    if (cJSON_IsString(j)) {
        for (int i = 0; i < unit_option_count(UNIT_CAT_TIMEZONE); i++) {
            if (strcmp(j->valuestring, unit_option_name_at(UNIT_CAT_TIMEZONE, i)) == 0) {
                if (i != unit_config_get(UNIT_CAT_TIMEZONE)) unit_config_set(UNIT_CAT_TIMEZONE, i);
                break;
            }
        }
    }
    cJSON_Delete(root);
    day_night_set_settings(&s, true);
    ESP_LOGI(TAG, "Display settings updated from web editor");
    return h_display_get(req);
}

// --- Tides (LINZ table upload, port override, transducer offset) ---------------------
static esp_err_t h_tides_get(httpd_req_t *req)
{
    char q[32];
    bool ports = false;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[8];
        ports = httpd_query_key_value(q, "ports", v, sizeof(v)) == ESP_OK && v[0] == '1';
    }
    return send_json(req, tides_status_json(ports));
}

static esp_err_t h_tides_put(httpd_req_t *req)
{
    if (req->content_len < 32) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
    remove(tides_upload_path());
    if (req->content_len > tides_upload_max()) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not enough free storage for this file");
    }
    FILE *f = fopen(tides_upload_path(), "wb");
    char *buf = (char *)malloc(8192);
    if (!f || !buf) {
        if (f) fclose(f);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create file");
    }
    size_t got = 0;
    int timeouts = 0;
    bool ok = true;
    while (got < req->content_len) {
        size_t want = req->content_len - got;
        int r = httpd_req_recv(req, buf, want > 8192 ? 8192 : want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (r <= 0 || fwrite(buf, 1, r, f) != (size_t)r) { ok = false; break; }
        timeouts = 0;
        got += r;
    }
    fclose(f);
    free(buf);
    if (!ok) {
        remove(tides_upload_path());
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
    }
    char err[64];
    if (!tides_install_upload(err, sizeof(err))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
    }
    return send_json(req, tides_status_json(false));
}

static esp_err_t h_tides_settings(httpd_req_t *req)
{
    char buf[256];
    if (req->content_len == 0 || req->content_len >= sizeof(buf)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    buf[got] = '\0';
    cJSON *root = cJSON_Parse(buf);
    char err[64] = "Invalid JSON";
    bool ok = cJSON_IsObject(root) && tides_apply_settings(root, err, sizeof(err));
    cJSON_Delete(root);
    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
    return send_json(req, tides_status_json(false));
}

#pragma pack(push, 1)
struct BmpHeader {
    uint16_t type;
    uint32_t file_size;
    uint32_t reserved;
    uint32_t offset;
    uint32_t hdr_size;
    int32_t  width;
    int32_t  height;
    uint16_t planes;
    uint16_t bpp;
    uint32_t compression;
    uint32_t image_size;
    int32_t  xppm, yppm;
    uint32_t colors, important;
    uint32_t mask_r, mask_g, mask_b;
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Display performance counters (updated in the LVGL task, read under its lock)
// ---------------------------------------------------------------------------

struct PerfStats {
    uint32_t frames, frame_ms, frame_max, slow, px;
    uint32_t gfx, gfx_ms, gfx_max;
    int64_t  since_us;
};
static PerfStats s_perf;

static void perf_monitor_cb(lv_disp_drv_t *drv, uint32_t ms, uint32_t px)
{
    (void)drv;
    s_perf.frames++;
    s_perf.frame_ms += ms;
    s_perf.px += px;
    if (ms > s_perf.frame_max) s_perf.frame_max = ms;
    if (ms > 33) s_perf.slow++;
}

void dash_web_perf_note_gfx(uint32_t ms)
{
    s_perf.gfx++;
    s_perf.gfx_ms += ms;
    if (ms > s_perf.gfx_max) s_perf.gfx_max = ms;
}

static esp_err_t h_perf(httpd_req_t *req)
{
    if (!lvgl_port_lock(2000)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Display busy");
    }
    PerfStats p = s_perf;
    s_perf = {};
    s_perf.since_us = esp_timer_get_time();
    lvgl_port_unlock();

    char out[512];
    snprintf(out, sizeof(out),
             "{\"window_s\":%.1f,\"frames\":%lu,\"frame_avg_ms\":%.1f,\"frame_max_ms\":%lu,"
             "\"frames_over_33ms\":%lu,\"avg_px\":%lu,\"tile_renders\":%lu,\"tile_render_avg_ms\":%.1f,"
             "\"tile_render_max_ms\":%lu,\"free_internal\":%lu,\"largest_internal\":%lu,\"free_psram\":%lu}",
             (esp_timer_get_time() - p.since_us) / 1e6, (unsigned long)p.frames,
             p.frames ? (double)p.frame_ms / p.frames : 0.0, (unsigned long)p.frame_max,
             (unsigned long)p.slow, (unsigned long)(p.frames ? p.px / p.frames : 0), (unsigned long)p.gfx,
             p.gfx ? (double)p.gfx_ms / p.gfx : 0.0, (unsigned long)p.gfx_max,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t h_screenshot(httpd_req_t *req)
{
    if (!lvgl_port_lock(2000)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Display busy");
    }
    lv_obj_t *scr = lv_scr_act();
    uint32_t size = lv_snapshot_buf_size_needed(scr, LV_IMG_CF_TRUE_COLOR);
    uint8_t *buf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    lv_img_dsc_t dsc = {};
    lv_res_t res = buf ? lv_snapshot_take_to_buf(scr, LV_IMG_CF_TRUE_COLOR, &dsc, buf, size)
                       : LV_RES_INV;
    if (res == LV_RES_OK) day_night_filter_pixels((uint16_t *)buf, (size_t)dsc.header.w * dsc.header.h);
    lvgl_port_unlock();
    if (res != LV_RES_OK) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Snapshot failed");
    }

    int w = dsc.header.w, h = dsc.header.h;
    int row = w * 2;
    int pad = (4 - row % 4) % 4;
    BmpHeader hdr = {};
    hdr.type = 0x4D42;
    hdr.offset = sizeof(BmpHeader);
    hdr.image_size = (row + pad) * h;
    hdr.file_size = hdr.offset + hdr.image_size;
    hdr.hdr_size = 40;
    hdr.width = w;
    hdr.height = -h;   // top-down
    hdr.planes = 1;
    hdr.bpp = 16;
    hdr.compression = 3;   // BI_BITFIELDS
    hdr.mask_r = 0xF800;
    hdr.mask_g = 0x07E0;
    hdr.mask_b = 0x001F;

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)&hdr, sizeof(hdr));
    static const uint8_t zeros[4] = {};
    for (int y = 0; y < h && err == ESP_OK; y++) {
        err = httpd_resp_send_chunk(req, (const char *)buf + y * row, row);
        if (err == ESP_OK && pad) err = httpd_resp_send_chunk(req, (const char *)zeros, pad);
    }
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, nullptr, 0);
    free(buf);
    return err;
}

void dash_web_start(void)
{
    if (s_server) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    // Browsers open several parallel connections on page load; LWIP_MAX_SOCKETS is sized for this
    cfg.max_open_sockets = 10;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web editor server");
        s_server = nullptr;
        return;
    }
    const httpd_uri_t uris[] = {
        { "/",                   HTTP_GET,  h_index,      nullptr },
        { "/index.html",         HTTP_GET,  h_index,      nullptr },
        { "/api/config",         HTTP_GET,  h_config_get, nullptr },
        { "/api/config",         HTTP_PUT,  h_config_put, nullptr },
        { "/api/config",         HTTP_POST, h_config_put, nullptr },
        { "/api/reset",          HTTP_POST, h_reset,      nullptr },
        { "/api/paths",          HTTP_GET,  h_paths,      nullptr },
        { "/api/units",          HTTP_GET,  h_units,      nullptr },
        { "/api/values",         HTTP_GET,  h_values,     nullptr },
        { "/api/screenshot.bmp", HTTP_GET,  h_screenshot, nullptr },
        { "/api/perf",           HTTP_GET,  h_perf,       nullptr },
        { "/api/display",        HTTP_GET,  h_display_get, nullptr },
        { "/api/display",        HTTP_PUT,  h_display_put, nullptr },
        { "/api/tides",          HTTP_GET,  h_tides_get,  nullptr },
        { "/api/tides",          HTTP_PUT,  h_tides_put,  nullptr },
        { "/api/tides/settings", HTTP_PUT,  h_tides_settings, nullptr },
    };
    for (const auto &u : uris) httpd_register_uri_handler(s_server, &u);
    if (lvgl_port_lock(2000)) {
        lv_disp_t *disp = lv_disp_get_default();
        if (disp && disp->driver) disp->driver->monitor_cb = perf_monitor_cb;
        s_perf.since_us = esp_timer_get_time();
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "Dashboard web editor listening on port %d", cfg.server_port);
}
