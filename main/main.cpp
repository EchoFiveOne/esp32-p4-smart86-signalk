/*
 * SignalK Instrument Panel - Waveshare ESP32-P4-WIFI6-Touch-LCD-4B
 * 4" 720x720 IPS MIPI-DSI display, GT911 touch, panel controller auto-detected
 *
 * Architecture:
 *   - Board Manager detects and initializes the LCD controller
 *   - Waveshare BSP handles I2C, GT911 touch, and backlight
 *   - esp-ui (esp-brookesia v0.2.x) provides phone launcher shell
 *   - SignalKApp is the instrument panel app installed in the launcher
 *   - SignalKClient manages WebSocket connection in a FreeRTOS task
 *   - WiFi via ESP32-C6 coprocessor (esp-hosted/wifi_remote)
 *   - mDNS discovers SignalK servers on the local network
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "mdns.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include "bsp/esp32_p4_wifi6_touch_lcd_4b.h"
#include "esp_board_manager.h"
#include "dev_display_lcd.h"
#include "esp_lcd_panel_ops.h"
#include "bsp/touch.h"
#include "esp_lvgl_port.h"
#include "esp_ui.hpp"
#include "systems/phone/stylesheet/dark/phone_stylesheet.h"
#include "autopilot_app.hpp"
#include "wifi_settings_app.hpp"
#include "settings_app.hpp"
#include "unit_config.hpp"
#include "signalk_client.hpp"
#include "signalk_auth.hpp"
#include "wifi_manager.hpp"
#include "audio_feedback.hpp"
#include "instruments_app.hpp"
#include "dash_model.hpp"
#include "dash_engine.hpp"
#include "dash_web.hpp"
#include "day_night.hpp"
#include "sk_logo_alpha.h"

static const char *TAG = "main";

static lv_disp_t *start_display(void)
{
    ESP_ERROR_CHECK(esp_board_manager_init_device_by_name("display_lcd"));

    void *display_handle = nullptr;
    ESP_ERROR_CHECK(esp_board_manager_get_device_handle("display_lcd", &display_handle));
    auto *lcd_handles = static_cast<dev_display_lcd_handles_t *>(display_handle);

    void *display_config = nullptr;
    ESP_ERROR_CHECK(esp_board_manager_get_device_config("display_lcd", &display_config));
    auto *lcd_cfg = static_cast<dev_display_lcd_config_t *>(display_config);

    if (lcd_handles == nullptr || lcd_handles->panel_handle == nullptr ||
        lcd_handles->io_handle == nullptr || lcd_cfg == nullptr) {
        ESP_LOGE(TAG, "Board Manager returned incomplete LCD configuration");
        return nullptr;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_handles->panel_handle, true));
    ESP_ERROR_CHECK(bsp_display_brightness_init());

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    // Instruments app saves its config (cJSON + LittleFS) from LVGL event handlers
    lvgl_cfg.task_stack = 12288;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    lvgl_port_display_cfg_t display_cfg = {};
    display_cfg.io_handle = lcd_handles->io_handle;
    display_cfg.panel_handle = lcd_handles->panel_handle;
    // Draw into internal SRAM (much faster than PSRAM for blending); 50 lines = 72 KB, 15 strips per frame.
    // buff_dma forces internal RAM: MALLOC_CAP_DEFAULT would put an allocation this large in PSRAM.
    display_cfg.buffer_size = BSP_LCD_H_RES * 50;
    display_cfg.double_buffer = false;
    display_cfg.hres = lcd_cfg->lcd_width;
    display_cfg.vres = lcd_cfg->lcd_height;
    display_cfg.rotation.swap_xy = lcd_cfg->swap_xy;
    display_cfg.rotation.mirror_x = lcd_cfg->mirror_x;
    display_cfg.rotation.mirror_y = lcd_cfg->mirror_y;
    display_cfg.flags.buff_dma = true;
    display_cfg.flags.buff_spiram = false;

    lvgl_port_display_dsi_cfg_t dsi_cfg = {};
    dsi_cfg.flags.avoid_tearing = false;

    lv_disp_t *disp = lvgl_port_add_disp_dsi(&display_cfg, &dsi_cfg);
    if (disp == nullptr) {
        ESP_LOGE(TAG, "Failed to add LCD to LVGL");
        return nullptr;
    }

    ESP_ERROR_CHECK(bsp_display_backlight_on());
    ESP_LOGI(TAG, "Display ready: %dx%d", lcd_cfg->lcd_width, lcd_cfg->lcd_height);
    return disp;
}

static lv_indev_t *start_touch(lv_disp_t *disp)
{
    esp_lcd_touch_handle_t touch_handle = nullptr;
    ESP_ERROR_CHECK(bsp_touch_new(nullptr, &touch_handle));

    lvgl_port_touch_cfg_t touch_cfg = {};
    touch_cfg.disp = disp;
    touch_cfg.handle = touch_handle;
    return lvgl_port_add_touch(&touch_cfg);
}

// Forward declaration — defined below WiFi section
static const char *discover_signalk_uri(void);

// ---------------------------------------------------------------------------
// Launcher status overlay — SK logo + connection text on the home screen
// Uses LV_IMG_CF_ALPHA_8BIT so the logo is fully transparent where alpha=0,
// blending naturally with whatever the launcher background color is.
// ---------------------------------------------------------------------------

static lv_obj_t *s_status_img   = nullptr;
static lv_obj_t *s_status_label = nullptr;
static ESP_UI_Phone *s_phone    = nullptr;

static const lv_color_t CLR_GREEN = LV_COLOR_MAKE(0x00, 0xc8, 0x53);
static const lv_color_t CLR_RED   = LV_COLOR_MAKE(0xe0, 0x40, 0x40);
static const lv_color_t CLR_AMBER = LV_COLOR_MAKE(0xff, 0xa0, 0x40);
static const lv_color_t CLR_LABEL = LV_COLOR_MAKE(0xcc, 0xcc, 0xcc);

/* LVGL image descriptor pointing to the alpha map data */
static const lv_img_dsc_t sk_logo_dsc = {
    .header = {
        .cf          = LV_IMG_CF_ALPHA_8BIT,
        .always_zero = 0,
        .reserved    = 0,
        .w           = SK_LOGO_WIDTH,
        .h           = SK_LOGO_HEIGHT,
    },
    .data_size = SK_LOGO_WIDTH * SK_LOGO_HEIGHT,
    .data      = (const uint8_t *)sk_logo_alpha,
};


static void update_status_overlay(signalk_conn_state_t state)
{
    lv_color_t icon_color;
    const char *text;

    switch (state) {
        case SK_STATE_WIFI_CONNECTING:
            icon_color = CLR_AMBER;
            text = "Connecting WiFi...";
            break;
        case SK_STATE_MDNS_SEARCHING:
            icon_color = CLR_AMBER;
            text = "Searching for SignalK...";
            break;
        case SK_STATE_WS_CONNECTING:
            icon_color = CLR_AMBER;
            text = "Connecting...";
            break;
        case SK_STATE_CONNECTED:
            icon_color = CLR_GREEN;
            text = "Connected";
            break;
        case SK_STATE_DISCONNECTED:
        default:
            icon_color = CLR_RED;
            text = "Disconnected";
            break;
    }

    if (s_status_img) {
        lv_obj_set_style_img_recolor(s_status_img, icon_color, 0);
        lv_obj_set_style_img_recolor_opa(s_status_img, LV_OPA_COVER, 0);
    }
    if (s_status_label) {
        lv_label_set_text(s_status_label, text);
    }
}

// ---------------------------------------------------------------------------
// Status bar — WiFi icon + clock
// ---------------------------------------------------------------------------

static void update_status_bar_wifi(void)
{
    if (!s_phone) return;

    int state = 0;  // 0=closed, 1-3=signal levels
    if (wifi_manager_get_state() == WIFI_MGR_CONNECTED) {
        // Get RSSI for connected network from scan results
        WifiScanResult results[WIFI_MGR_MAX_SCAN];
        int count = wifi_manager_scan_get_results(results, WIFI_MGR_MAX_SCAN);
        const char *ssid = wifi_manager_get_ssid();
        int rssi = -100;
        for (int i = 0; i < count; i++) {
            if (strcmp(results[i].ssid, ssid) == 0) {
                rssi = results[i].rssi;
                break;
            }
        }
        // Map RSSI to 3 levels
        if (rssi > -50)       state = 3;  // excellent
        else if (rssi > -70)  state = 2;  // good
        else                  state = 1;  // weak
    }

    s_phone->getHome().getStatusBar()->setWifiIconState(state);
}

static lv_obj_t *s_clock_period = nullptr;

// esp-ui's clock has hour, ":", minute and an AM/PM label; find the latter
static lv_obj_t *find_period_label(lv_obj_t *obj)
{
    uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(obj, i);
        if (lv_obj_check_type(c, &lv_label_class)) {
            const char *t = lv_label_get_text(c);
            if (t && (strcmp(t, " AM ") == 0 || strcmp(t, " PM ") == 0)) return c;
        }
        lv_obj_t *f = find_period_label(c);
        if (f) return f;
    }
    return nullptr;
}

static void update_status_bar_clock(void)
{
    if (!s_phone) return;

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // Skip if time hasn't been synced yet (year < 2024)
    if (timeinfo.tm_year < (2024 - 1900)) return;

    // 24-hour clock: hour 0..23, AM/PM label hidden
    ESP_UI_StatusBar *bar = s_phone->getHome().getStatusBar();
    bar->setClock(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_hour >= 12);
    if (!s_clock_period) {
        s_clock_period = find_period_label(lv_layer_top());
        if (!s_clock_period) s_clock_period = find_period_label(lv_layer_sys());
        if (!s_clock_period) s_clock_period = find_period_label(s_phone->getHome().getMainScreen());
        if (s_clock_period) lv_obj_add_flag(s_clock_period, LV_OBJ_FLAG_HIDDEN);
    }
}

static void init_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP...");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb([](struct timeval *) { day_night_note_sntp_sync(); });
    esp_sntp_init();
    /* Timezone already applied by unit_config_init() at boot */
}

static bool s_client_has_token = false;

static void start_client(const char *uri)
{
    s_client_has_token = signalk_auth_has_token();
    signalk_client_start(uri);
}

/* Signal K with security on closes unauthenticated streams, so ask the
 * server for a device token (approved once in the admin UI) and reconnect
 * with it once granted. */
static void maintain_auth(void)
{
    static uint32_t s_next_request_ms = 0;
    if (!signalk_auth_get_base_url() || wifi_manager_get_state() != WIFI_MGR_CONNECTED) return;

    if (signalk_auth_has_token()) {
        if (!s_client_has_token && signalk_client_get_state() != SK_STATE_MDNS_SEARCHING) {
            ESP_LOGI(TAG, "Device token available - reconnecting SignalK with it");
            signalk_client_stop();   // state DISCONNECTED: discovery below restarts it
        }
        return;
    }

    signalk_auth_state_t st = signalk_auth_get_state();
    if (st == SK_AUTH_REQUESTING || st == SK_AUTH_DENIED) return;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_next_request_ms && (int32_t)(now_ms - s_next_request_ms) < 0) return;
    s_next_request_ms = now_ms + (st == SK_AUTH_ERROR ? 300000 : 60000);
    ESP_LOGI(TAG, "No SignalK device token - requesting access (approve in SignalK admin)");
    signalk_auth_request_access();
}

static void status_timer_cb(lv_timer_t *t)
{
    maintain_auth();

    // Check if max retries reached and client needs re-discovery
    if (signalk_client_needs_rediscovery()) {
        ESP_LOGI(TAG, "Tearing down old client and re-discovering via mDNS...");
        signalk_client_stop();
        const char *uri = discover_signalk_uri();
        if (uri) {
            signalk_auth_set_base_url(uri);
            start_client(uri);
        } else {
            ESP_LOGW(TAG, "mDNS re-discovery failed — will retry next cycle");
            signalk_client_set_state(SK_STATE_DISCONNECTED);
        }
    }

    /* Watchdog: if never reaching CONNECTED for a minute in any other state
     * (e.g. a client stuck after a server restart), force a fresh discovery. */
    static uint32_t s_not_connected_since = 0;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (signalk_client_is_connected() || wifi_manager_get_state() != WIFI_MGR_CONNECTED) {
        s_not_connected_since = now_ms;
    } else if (now_ms - s_not_connected_since > 60000) {
        ESP_LOGW(TAG, "SignalK not connected for 60 s - restarting client");
        s_not_connected_since = now_ms;
        signalk_client_stop();
    }

    /* Also try initial discovery if WiFi is connected but SignalK never found */
    if (signalk_client_get_state() == SK_STATE_DISCONNECTED &&
        wifi_manager_get_state() == WIFI_MGR_CONNECTED &&
        !signalk_client_is_connected()) {
        const char *uri = discover_signalk_uri();
        if (uri) {
            // Replace any existing client; start() is a no-op while one exists
            signalk_client_stop();
            signalk_auth_init(uri);
            start_client(uri);
        }
    }

    static signalk_conn_state_t prev = (signalk_conn_state_t)-1;
    signalk_conn_state_t cur = signalk_client_get_state();
    if (cur != prev) {
        prev = cur;
        update_status_overlay(cur);
    }

    // Update status bar WiFi icon and clock
    update_status_bar_wifi();
    update_status_bar_clock();
}

// ---------------------------------------------------------------------------
// mDNS discovery
// ---------------------------------------------------------------------------

static char s_discovered_uri[128];
static uint32_t s_last_ip;      // last server that answered (network byte order), 0 = none
static uint16_t s_last_port;

static const char *set_uri(uint32_t ip, uint16_t port)
{
    esp_ip4_addr_t a = { .addr = ip };
    snprintf(s_discovered_uri, sizeof(s_discovered_uri),
             "ws://" IPSTR ":%u/signalk/v1/stream?subscribe=none", IP2STR(&a), port);
    s_last_ip = ip;
    s_last_port = port;
    return s_discovered_uri;
}

static bool tcp_probe(uint32_t ip, uint16_t port, int timeout_ms)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return false;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = ip;
    bool ok = connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0;
    if (!ok && errno == EINPROGRESS) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        if (select(s + 1, NULL, &w, NULL, &tv) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
            ok = err == 0;
        }
    }
    close(s);
    return ok;
}

/* Used when mDNS finds nothing (e.g. the server doesn't advertise on a hotspot
 * interface): try the last server that worked, the WiFi gateway (a Pi / Venus OS
 * hotspot is usually the server itself) and CONFIG_SIGNALK_WS_URI, on port 3000. */
static const char *fallback_signalk_uri(void)
{
    struct { uint32_t ip; uint16_t port; const char *why; } cand[3] = {};
    int n = 0;
    if (s_last_ip) cand[n++] = { s_last_ip, s_last_port, "last server" };

    esp_netif_ip_info_t info = {};
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.gw.addr) {
        cand[n++] = { info.gw.addr, 3000, "WiFi gateway" };
    }

    unsigned b[4], port = 3000;
    if (sscanf(CONFIG_SIGNALK_WS_URI, "ws://%u.%u.%u.%u:%u", &b[0], &b[1], &b[2], &b[3], &port) >= 4) {
        cand[n++] = { ESP_IP4TOADDR(b[0], b[1], b[2], b[3]), (uint16_t)port, "configured URI" };
    }

    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++) dup |= cand[j].ip == cand[i].ip && cand[j].port == cand[i].port;
        if (dup) continue;
        esp_ip4_addr_t a = { .addr = cand[i].ip };
        if (tcp_probe(cand[i].ip, cand[i].port, 700)) {
            ESP_LOGI(TAG, "SignalK fallback (%s): " IPSTR ":%u", cand[i].why, IP2STR(&a), cand[i].port);
            return set_uri(cand[i].ip, cand[i].port);
        }
        ESP_LOGW(TAG, "SignalK fallback (%s) " IPSTR ":%u not reachable", cand[i].why, IP2STR(&a), cand[i].port);
    }
    return NULL;
}

static const char *discover_signalk_uri(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return fallback_signalk_uri();
    }

    ESP_LOGI(TAG, "mDNS: searching for _signalk-ws._tcp ...");
    signalk_client_set_state(SK_STATE_MDNS_SEARCHING);

    mdns_result_t *results = NULL;
    err = mdns_query_ptr("_signalk-ws", "_tcp",
                          CONFIG_SIGNALK_MDNS_TIMEOUT_MS, 5, &results);
    if (err != ESP_OK || results == NULL) {
        ESP_LOGW(TAG, "mDNS: no SignalK server found on network");
        mdns_free();
        return fallback_signalk_uri();
    }

    // A multi-homed server may advertise an address from another interface,
    // so use the first IPv4 address that actually accepts a connection.
    for (mdns_result_t *r = results; r != NULL; r = r->next) {
        for (mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
            if (a->addr.type != ESP_IPADDR_TYPE_V4) continue;
            esp_ip4_addr_t ip = { .addr = a->addr.u_addr.ip4.addr };
            if (!tcp_probe(ip.addr, r->port, 700)) {
                ESP_LOGW(TAG, "mDNS: " IPSTR ":%u (host=%s) not reachable", IP2STR(&ip),
                         r->port, r->hostname ? r->hostname : "?");
                continue;
            }
            set_uri(ip.addr, r->port);
            ESP_LOGI(TAG, "mDNS discovered SignalK: %s (host=%s)",
                     s_discovered_uri, r->hostname ? r->hostname : "?");
            mdns_query_results_free(results);
            return s_discovered_uri;
        }
    }

    ESP_LOGW(TAG, "mDNS: no reachable IPv4 address advertised");
    mdns_query_results_free(results);
    return fallback_signalk_uri();
}

// ---------------------------------------------------------------------------
// WiFi — delegated to wifi_manager component
// ---------------------------------------------------------------------------

/* Venus OS hotspots (ConnMan with --nodnsproxy) hand out no DNS server, which
 * breaks SNTP. Fall back to the gateway, which can run a DNS forwarder. */
static void ensure_dns_server(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return;
    esp_netif_dns_info_t dns = {};
    if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
        dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0) {
        return;
    }
    esp_netif_ip_info_t info = {};
    if (esp_netif_get_ip_info(sta, &info) != ESP_OK || info.gw.addr == 0) return;
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = info.gw.addr;
    if (esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        ESP_LOGI(TAG, "DHCP gave no DNS server; using gateway " IPSTR, IP2STR(&info.gw));
    }
}

static void on_wifi_got_ip(void)
{
    ensure_dns_server();

    // Always stop + re-discover on WiFi connect (IP may have changed)
    signalk_client_stop();
    const char *uri = discover_signalk_uri();
    if (!uri) {
        ESP_LOGW(TAG, "No SignalK server found — will retry via status timer");
        signalk_client_set_state(SK_STATE_DISCONNECTED);
        return;
    }
    signalk_auth_init(uri);
    start_client(uri);

    // Start SNTP time sync (safe to call multiple times — checks if already running)
    if (!esp_sntp_enabled()) {
        init_sntp();
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "SignalK Instrument Panel starting");

    // --- BSP I2C, auto-detected display, and GT911 touch ---
    ESP_ERROR_CHECK(bsp_i2c_init());

    lv_disp_t *disp = start_display();
    ESP_ERROR_CHECK(disp == nullptr ? ESP_FAIL : ESP_OK);
    lv_indev_t *touch_indev = start_touch(disp);
    ESP_ERROR_CHECK(touch_indev == nullptr ? ESP_FAIL : ESP_OK);

    // --- Audio feedback ---
    audio_feedback_init();

    // --- WiFi (wifi_manager handles init, events, NVS) ---
    signalk_client_set_state(SK_STATE_WIFI_CONNECTING);
    wifi_manager_init(on_wifi_got_ip);

    // --- Unit config (timezone, speed, depth, temp, distance — from NVS) ---
    // Must be after wifi_manager_init() which calls nvs_flash_init()
    unit_config_init();

    // --- Instruments dashboard configuration (LittleFS + NVS) ---
    dash_config_init();

    // Connect to best saved network in range, or stay disconnected
    if (!wifi_manager_connect_saved()) {
        ESP_LOGI(TAG, "No saved networks in range — use WiFi app to configure");
    }

    // --- esp-ui phone shell ---
    bsp_display_lock(0);

    ESP_LOGI(TAG, "LVGL disp size: %dx%d", lv_disp_get_hor_res(disp), lv_disp_get_ver_res(disp));

    ESP_UI_Phone *phone = new ESP_UI_Phone(disp);
    ESP_UI_CHECK_NULL_EXIT(phone, "Create phone failed");
    s_phone = phone;

    // Workaround: esp-ui v0.2.1 bug — calibrateResolutionSize resolves percent→pixels
    // in a local copy, so calibrateStylesheet receives screen_size with raw height=0.
    // Fix: add stylesheet with explicit pixel dimensions before begin().
    ESP_UI_PhoneStylesheet_t stylesheet = ESP_UI_PHONE_DEFAULT_DARK_STYLESHEET();
    stylesheet.core.screen_size = ESP_UI_STYLE_SIZE_RECT(BSP_LCD_H_RES, BSP_LCD_V_RES);
    ESP_UI_CHECK_FALSE_EXIT(phone->addStylesheet(stylesheet), "Add stylesheet failed");
    ESP_UI_CHECK_FALSE_EXIT(phone->activateStylesheet(stylesheet), "Activate stylesheet failed");

    ESP_UI_CHECK_FALSE_EXIT(phone->setTouchDevice(touch_indev),
                            "Set touch device failed");
    ESP_UI_CHECK_FALSE_EXIT(phone->begin(), "Phone begin failed");

    // --- Hide battery icon (no battery on this board) ---
    phone->getHome().getStatusBar()->hideBatteryIcon();

    // --- Status overlay on launcher home screen ---
    lv_obj_t *home_scr = phone->getHome().getMainScreen();

    s_status_img = lv_img_create(home_scr);
    lv_img_set_src(s_status_img, &sk_logo_dsc);
    lv_obj_align(s_status_img, LV_ALIGN_BOTTOM_MID, 0, -120);

    s_status_label = lv_label_create(home_scr);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_label, CLR_LABEL, 0);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_status_label, 400);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -70);

    // Force initial amber state — WiFi connect may have already fired
    update_status_overlay(signalk_client_get_state());

    // Timer to poll state changes (500ms for responsive feedback)
    lv_timer_create(status_timer_cb, 500, NULL);

    // --- Install Settings app (first in launcher) ---
    SettingsApp *settings_app = new SettingsApp();
    ESP_UI_CHECK_NULL_EXIT(settings_app, "Create Settings app failed");
    int settings_id = phone->installApp(settings_app);
    ESP_UI_CHECK_FALSE_EXIT((settings_id >= 0), "Install Settings app failed");
    ESP_LOGI(TAG, "Settings app installed in launcher");

    // --- Install Instruments dashboard app ---
    // The engine runs from boot so trends, min/max and alarms keep updating
    dash_engine_init(audio_feedback_invalid);

    // --- Panel-wide brightness, day/night (sun) mode, red filter, auto time zone ---
    {
        DashConfig *dc = (DashConfig *)malloc(sizeof(DashConfig));
        int seed = 100;
        if (dc) { dash_config_get(dc); seed = dc->settings.brightness; free(dc); }
        day_night_init(disp, [](int percent) { bsp_display_brightness_set(percent); }, seed);
    }

    InstrumentsApp *inst_app = new InstrumentsApp();
    ESP_UI_CHECK_NULL_EXIT(inst_app, "Create Instruments app failed");
    int inst_id = phone->installApp(inst_app);
    ESP_UI_CHECK_FALSE_EXIT((inst_id >= 0), "Install Instruments app failed");
    ESP_LOGI(TAG, "Instruments app installed in launcher");

    // --- Install Autopilot app ---
    AutopilotApp *ap_app = new AutopilotApp();
    ESP_UI_CHECK_NULL_EXIT(ap_app, "Create Autopilot app failed");
    int ap_id = phone->installApp(ap_app);
    ESP_UI_CHECK_FALSE_EXIT((ap_id >= 0), "Install Autopilot app failed");

    ESP_LOGI(TAG, "Autopilot app installed in launcher");

    // --- Install WiFi Settings app ---
    WifiSettingsApp *wifi_app = new WifiSettingsApp();
    ESP_UI_CHECK_NULL_EXIT(wifi_app, "Create WiFi Settings app failed");
    int wifi_id = phone->installApp(wifi_app);
    ESP_UI_CHECK_FALSE_EXIT((wifi_id >= 0), "Install WiFi Settings app failed");

    ESP_LOGI(TAG, "WiFi Settings app installed in launcher");

    // --- Open the app chosen in Settings > Startup App ---
    {
        int startup_id = -1;
        switch (unit_config_get(UNIT_CAT_STARTUP)) {
        case STARTUP_INSTRUMENTS: startup_id = inst_id;     break;
        case STARTUP_AUTOPILOT:   startup_id = ap_id;       break;
        case STARTUP_WIFI:        startup_id = wifi_id;     break;
        case STARTUP_SETTINGS:    startup_id = settings_id; break;
        default: break;
        }
        if (startup_id >= 0) {
            // Deferred so the launcher finishes its first layout before the app opens
            lv_timer_create([](lv_timer_t *tm) {
                int id = (int)(intptr_t)tm->user_data;
                ESP_UI_CoreAppEventData_t ev = { id, ESP_UI_CORE_APP_EVENT_TYPE_START, nullptr };
                if (!s_phone->sendAppEvent(&ev)) {
                    ESP_LOGW(TAG, "Failed to open startup app %d", id);
                }
                lv_timer_del(tm);
            }, 300, (void *)(intptr_t)startup_id);
            ESP_LOGI(TAG, "Startup app: %s", unit_option_name(UNIT_CAT_STARTUP));
        }
    }

    bsp_display_unlock();

    // Web editor for the Instruments dashboard (http://<panel-ip>/)
    dash_web_start();

    // Main task: monitor heap (debug), everything else is event-driven
    while (1) {
        ESP_LOGD(TAG, "Free heap: %lu SRAM, %lu PSRAM",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
