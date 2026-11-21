#include "signalk_client.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "lvgl.h"
#include "signalk_auth.hpp"

static const char *TAG = "signalk";

// ---------------------------------------------------------------------------
// Observer — single path watch with LVGL thread notification
// ---------------------------------------------------------------------------

static char s_watch_path[96]          = {};
static signalk_value_cb_t s_watch_cb  = nullptr;
static void *s_watch_user_data        = nullptr;

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

static InstrumentStore           s_data  = {};
static signalk_conn_state_t     s_state = SK_STATE_WIFI_CONNECTING;
static esp_websocket_client_handle_t s_ws = nullptr;

static int  s_disconnect_count    = 0;
static bool s_needs_rediscovery   = false;
static const int MAX_RETRY_BEFORE_REDISCOVERY = 3;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// ---------------------------------------------------------------------------
// Initial subscriptions — paths needed by autopilot and wind rose apps
// ---------------------------------------------------------------------------

struct SubEntry {
    const char *path;
    int period_ms;
};

static const SubEntry s_initial_subs[] = {
    { "navigation.speedThroughWater",                1000 },
    { "navigation.headingMagnetic",                  1000 },
    { "navigation.headingTrue",                      1000 },
    { "environment.wind.speedApparent",               500 },
    { "environment.wind.angleApparent",               500 },
    { "environment.wind.speedTrue",                   500 },
    { "environment.wind.angleTrueWater",              500 },
    { "environment.depth.belowTransducer",           2000 },
    { "steering.rudderAngle",                         500 },
    { "navigation.courseRhumbline.crossTrackError",  2000 },
};

static const int s_initial_sub_count = sizeof(s_initial_subs) / sizeof(s_initial_subs[0]);

// ---------------------------------------------------------------------------
// Subscription registry — reference-counted, protected by s_data.mutex.
// Initial paths are permanent. Every entry is re-sent on (re)connect.
// ---------------------------------------------------------------------------

struct SubRecord {
    char path[96];
    int  period_ms;
    int  refs;
    bool permanent;
};

static SubRecord s_subs[STORE_MAX_ENTRIES];
static int       s_sub_count = 0;

static SubRecord *sub_find(const char *path)
{
    for (int i = 0; i < s_sub_count; i++) {
        if (strcmp(s_subs[i].path, path) == 0) return &s_subs[i];
    }
    return nullptr;
}

static SubRecord *sub_add(const char *path, int period_ms, bool permanent)
{
    if (s_sub_count >= STORE_MAX_ENTRIES) return nullptr;
    SubRecord *r = &s_subs[s_sub_count++];
    strncpy(r->path, path, sizeof(r->path) - 1);
    r->path[sizeof(r->path) - 1] = '\0';
    r->period_ms = period_ms;
    r->refs      = 0;
    r->permanent = permanent;
    return r;
}

static void sub_remove(SubRecord *r)
{
    int idx = (int)(r - s_subs);
    if (idx < s_sub_count - 1) s_subs[idx] = s_subs[s_sub_count - 1];
    s_sub_count--;
}

static void ensure_store_slot(const char *path)
{
    if (store_find_mut(s_data, path)) return;
    if (s_data.count >= STORE_MAX_ENTRIES) return;
    PathValue *pv = &s_data.entries[s_data.count++];
    strncpy(pv->path, path, sizeof(pv->path) - 1);
    pv->path[sizeof(pv->path) - 1] = '\0';
    pv->valid = false;
    pv->last_update_ms = 0;
}

// ---------------------------------------------------------------------------
// Subscribe / unsubscribe helpers
// ---------------------------------------------------------------------------

static void send_subscribe_list(esp_websocket_client_handle_t client,
                                const SubEntry *subs, int count)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "context", "vessels.self");
    cJSON *arr = cJSON_AddArrayToObject(root, "subscribe");

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "path",   subs[i].path);
        cJSON_AddNumberToObject(item, "period", subs[i].period_ms);
        cJSON_AddItemToArray(arr, item);
    }

    char *msg = cJSON_PrintUnformatted(root);
    if (msg) {
        esp_websocket_client_send_text(client, msg, strlen(msg), pdMS_TO_TICKS(2000));
        ESP_LOGI(TAG, "Subscribed to %d paths", count);
        free(msg);
    }
    cJSON_Delete(root);
}

static void send_subscribe_one(esp_websocket_client_handle_t client,
                               const char *path, int period_ms)
{
    SubEntry e = { path, period_ms };
    send_subscribe_list(client, &e, 1);
}

// signalk-server only accepts a wildcard unsubscribe (anything else closes the
// stream), so drop everything and re-send the remaining registered paths.
static void send_subscribe(esp_websocket_client_handle_t client);

static void send_unsubscribe_one(esp_websocket_client_handle_t client,
                                 const char *path)
{
    static const char msg[] = "{\"context\":\"*\",\"unsubscribe\":[{\"path\":\"*\"}]}";
    esp_websocket_client_send_text(client, msg, strlen(msg), pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Unsubscribed from: %s (resubscribing remaining paths)", path);
    send_subscribe(client);
}

// Called on WS connect — (re)subscribe to every registered path
static void send_subscribe(esp_websocket_client_handle_t client)
{
    // Heap buffers: this runs from both the WS task and the LVGL task
    SubEntry *list = (SubEntry *)malloc(sizeof(SubEntry) * STORE_MAX_ENTRIES);
    char (*paths)[96] = (char (*)[96])malloc(96 * STORE_MAX_ENTRIES);
    if (!list || !paths) {
        free(list);
        free(paths);
        ESP_LOGE(TAG, "Out of memory building subscription list");
        return;
    }
    int n = 0;

    xSemaphoreTake(s_data.mutex, portMAX_DELAY);
    for (int i = 0; i < s_sub_count; i++) {
        memcpy(paths[n], s_subs[i].path, sizeof(paths[n]));
        list[n].path      = paths[n];
        list[n].period_ms = s_subs[i].period_ms;
        n++;
    }
    xSemaphoreGive(s_data.mutex);

    // Send in batches to keep each WebSocket frame small
    const int BATCH = 16;
    for (int i = 0; i < n; i += BATCH) {
        send_subscribe_list(client, &list[i], (n - i) < BATCH ? (n - i) : BATCH);
    }
    free(list);
    free(paths);
}

// ---------------------------------------------------------------------------
// Time from the data stream (GPS navigation.datetime or update timestamps)
// ---------------------------------------------------------------------------

static int64_t  s_time_epoch   = 0;    // UTC seconds at s_time_rx_ms
static uint32_t s_time_rx_ms   = 0;
static int      s_time_src     = 0;    // 1 = navigation.datetime, 2 = update timestamp
static uint32_t s_datetime_ms  = 0;    // last valid navigation.datetime

static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

// Parse "YYYY-MM-DDTHH:MM:SS[.fff]Z"; rejects years before 2024 (e.g. the
// simulator's stale recorded GPS date).
static bool parse_iso_utc(const char *s, int64_t *out)
{
    int y, mo, d, h, mi;
    double sec;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &sec) != 6) return false;
    if (y < 2024 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec >= 61) return false;
    size_t n = strlen(s);
    if (n && s[n - 1] != 'Z' && !strstr(s + 10, "+00:00")) return false;
    *out = days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + (int64_t)sec;
    return true;
}

static void note_time(int64_t epoch, int src, uint32_t now)
{
    if (src == 2 && s_datetime_ms && now - s_datetime_ms < 60000) return;
    s_time_epoch = epoch;
    s_time_rx_ms = now;
    s_time_src   = src;
    if (src == 1) s_datetime_ms = now;
}

bool signalk_client_get_time(int64_t *utc_s, uint32_t *age_ms, int *source)
{
    if (!s_data.mutex) return false;
    xSemaphoreTake(s_data.mutex, portMAX_DELAY);
    bool ok = s_time_src != 0;
    if (ok) {
        uint32_t age = now_ms() - s_time_rx_ms;
        if (utc_s)  *utc_s  = s_time_epoch + age / 1000;
        if (age_ms) *age_ms = age;
        if (source) *source = s_time_src;
    }
    xSemaphoreGive(s_data.mutex);
    return ok;
}

// ---------------------------------------------------------------------------
// Delta message parser
// ---------------------------------------------------------------------------

static void parse_delta(const char *json_str, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json_str, len);
    if (!root) return;

    cJSON *updates = cJSON_GetObjectItem(root, "updates");
    if (!cJSON_IsArray(updates)) {
        cJSON_Delete(root);
        return;
    }

    uint32_t now = now_ms();
    bool notify_watcher = false;

    xSemaphoreTake(s_data.mutex, portMAX_DELAY);

    cJSON *update;
    cJSON_ArrayForEach(update, updates) {
        cJSON *values = cJSON_GetObjectItem(update, "values");
        if (!cJSON_IsArray(values)) continue;

        int64_t ts;
        cJSON *ts_j = cJSON_GetObjectItem(update, "timestamp");
        if (cJSON_IsString(ts_j) && parse_iso_utc(ts_j->valuestring, &ts)) note_time(ts, 2, now);

        cJSON *val_obj;
        cJSON_ArrayForEach(val_obj, values) {
            cJSON *path_j  = cJSON_GetObjectItem(val_obj, "path");
            cJSON *value_j = cJSON_GetObjectItem(val_obj, "value");

            if (!cJSON_IsString(path_j)) continue;

            // Object/string values: position is split into .latitude/.longitude
            // (degrees) and GPS datetime feeds the time source.
            if (cJSON_IsObject(value_j) && strcmp(path_j->valuestring, "navigation.position") == 0) {
                cJSON *la = cJSON_GetObjectItem(value_j, "latitude");
                cJSON *lo = cJSON_GetObjectItem(value_j, "longitude");
                if (cJSON_IsNumber(la) && cJSON_IsNumber(lo) && store_find_mut(s_data, "navigation.position") &&
                    fabs(la->valuedouble) <= 90 && fabs(lo->valuedouble) <= 180) {
                    store_set(s_data, "navigation.position", 1.0f, now);
                    store_set(s_data, "navigation.position.latitude",  (float)la->valuedouble, now);
                    store_set(s_data, "navigation.position.longitude", (float)lo->valuedouble, now);
                }
                continue;
            }
            if (cJSON_IsString(value_j) && strcmp(path_j->valuestring, "navigation.datetime") == 0) {
                int64_t t;
                if (parse_iso_utc(value_j->valuestring, &t)) note_time(t, 1, now);
                continue;
            }
            if (!cJSON_IsNumber(value_j)) continue;

            const char *path = path_j->valuestring;
            float value      = (float)value_j->valuedouble;

            if (isnan(value) || isinf(value)) continue;

            // Store any subscribed path — no routing needed
            if (store_find_mut(s_data, path)) {
                store_set(s_data, path, value, now);

                // Check if this is the watched path
                if (s_watch_cb && s_watch_path[0] &&
                    strcmp(path, s_watch_path) == 0) {
                    notify_watcher = true;
                }
            }
        }
    }

    // ── Derive true wind from apparent wind + boat speed when server
    //    doesn't provide it directly.
    const PathValue *tws = store_find(s_data, "environment.wind.speedTrue");
    if (!tws || !tws->valid) {
        const PathValue *aws = store_find(s_data, "environment.wind.speedApparent");
        const PathValue *awa = store_find(s_data, "environment.wind.angleApparent");
        const PathValue *stw = store_find(s_data, "navigation.speedThroughWater");

        if (aws && aws->valid && awa && awa->valid && stw && stw->valid) {
            float a = aws->value, w = awa->value, s = stw->value;
            float tws_v = sqrtf(a * a + s * s - 2.0f * a * s * cosf(w));
            float twa_v = atan2f(a * sinf(w), a * cosf(w) - s);
            store_set(s_data, "environment.wind.speedTrue",      tws_v, now);
            store_set(s_data, "environment.wind.angleTrueWater", twa_v, now);
        }
    }

    xSemaphoreGive(s_data.mutex);

    // Notify watcher OUTSIDE mutex to avoid deadlock with LVGL locks
    if (notify_watcher && s_watch_cb) {
        ESP_LOGD(TAG, "Notifying watcher for: %s", s_watch_path);
        lv_async_call((lv_async_cb_t)s_watch_cb, s_watch_user_data);
    }

    cJSON_Delete(root);
}

// ---------------------------------------------------------------------------
// WebSocket event handler
// ---------------------------------------------------------------------------

static void ws_event_handler(void *handler_args, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to SignalK");
            s_state = SK_STATE_CONNECTED;
            s_disconnect_count = 0;
            send_subscribe(s_ws);
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            s_disconnect_count++;
            ESP_LOGW(TAG, "Disconnected from SignalK (attempt %d/%d)",
                     s_disconnect_count, MAX_RETRY_BEFORE_REDISCOVERY);
            if (s_disconnect_count >= MAX_RETRY_BEFORE_REDISCOVERY) {
                ESP_LOGW(TAG, "Max retries reached — requesting mDNS re-discovery");
                s_disconnect_count = 0;
                s_needs_rediscovery = true;
                s_state = SK_STATE_MDNS_SEARCHING;
            } else {
                s_state = SK_STATE_DISCONNECTED;
            }
            break;

        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 0x01 && data->data_len > 0) {
                parse_delta(data->data_ptr, data->data_len);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            s_state = SK_STATE_DISCONNECTED;
            break;

        case WEBSOCKET_EVENT_CLOSED:
            // Server-initiated close (e.g. server restart)
            ESP_LOGW(TAG, "SignalK closed the connection");
            s_state = SK_STATE_DISCONNECTED;
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void signalk_client_start(const char *uri)
{
    if (s_ws) {
        ESP_LOGW(TAG, "Client already started");
        return;
    }

    // Init shared data mutex
    if (!s_data.mutex) {
        s_data.mutex = xSemaphoreCreateMutex();
    }

    // Register permanent initial subscriptions and pre-populate their store
    // slots (valid=false until data arrives)
    xSemaphoreTake(s_data.mutex, portMAX_DELAY);
    for (int i = 0; i < s_initial_sub_count; i++) {
        SubRecord *r = sub_find(s_initial_subs[i].path);
        if (!r) r = sub_add(s_initial_subs[i].path, s_initial_subs[i].period_ms, true);
        if (r) r->permanent = true;
        ensure_store_slot(s_initial_subs[i].path);
    }
    xSemaphoreGive(s_data.mutex);

    s_state = SK_STATE_WS_CONNECTING;

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri                        = uri;
    ws_cfg.buffer_size                = 4096;
    ws_cfg.task_stack                 = 8192;
    ws_cfg.reconnect_timeout_ms       = CONFIG_SIGNALK_RECONNECT_MS;
    ws_cfg.disable_auto_reconnect     = false;
    // Without this the client task exits for good when the server closes
    ws_cfg.enable_close_reconnect     = true;
    ws_cfg.skip_cert_common_name_check = true;

    /* Signal K with security enabled drops unauthenticated streams */
    static char s_auth_header[600];
    const char *token = signalk_auth_get_token();
    if (token) {
        snprintf(s_auth_header, sizeof(s_auth_header), "Authorization: Bearer %s\r\n", token);
        ws_cfg.headers = s_auth_header;
    }

    s_ws = esp_websocket_client_init(&ws_cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        s_state = SK_STATE_DISCONNECTED;
        return;
    }

    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY,
                                   ws_event_handler, NULL);
    esp_websocket_client_start(s_ws);

    ESP_LOGI(TAG, "SignalK client started → %s (%s)", uri, token ? "with token" : "no token");
}

void signalk_client_stop(void)
{
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = nullptr;
    }
    s_state = SK_STATE_DISCONNECTED;
}

bool signalk_client_is_connected(void)
{
    return s_state == SK_STATE_CONNECTED;
}

signalk_conn_state_t signalk_client_get_state(void)
{
    return s_state;
}

void signalk_client_set_state(signalk_conn_state_t state)
{
    s_state = state;
}

bool signalk_client_needs_rediscovery(void)
{
    if (s_needs_rediscovery) {
        s_needs_rediscovery = false;
        return true;
    }
    return false;
}

bool signalk_client_get_data(InstrumentStore *dest)
{
    if (!s_data.mutex) return false;
    if (xSemaphoreTake(s_data.mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    *dest = s_data;
    dest->mutex = nullptr;
    xSemaphoreGive(s_data.mutex);
    return true;
}

bool signalk_client_get_values(const char *const *paths, int count, PathValue *out)
{
    if (!s_data.mutex) return false;
    if (xSemaphoreTake(s_data.mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    for (int i = 0; i < count; i++) {
        const PathValue *pv = (paths[i] && paths[i][0]) ? store_find(s_data, paths[i]) : nullptr;
        if (pv) {
            out[i] = *pv;
        } else {
            out[i] = PathValue{};
        }
    }
    xSemaphoreGive(s_data.mutex);
    return true;
}

static void ensure_mutex(void)
{
    // Subscriptions may be registered before the client is first started
    if (!s_data.mutex) s_data.mutex = xSemaphoreCreateMutex();
}

bool signalk_client_subscribe(const char *path, int period_ms)
{
    if (!path || !path[0]) return false;
    ensure_mutex();

    bool ok = true;
    bool send_now = false;
    xSemaphoreTake(s_data.mutex, portMAX_DELAY);
    SubRecord *r = sub_find(path);
    if (!r) {
        if (s_data.count >= STORE_MAX_ENTRIES && !store_find_mut(s_data, path)) {
            ok = false;
        } else {
            r = sub_add(path, period_ms, false);
            if (!r) ok = false;
        }
        if (ok) {
            ensure_store_slot(path);
            send_now = true;
            ESP_LOGI(TAG, "Subscribed (new): %s", path);
        }
    } else if (period_ms < r->period_ms) {
        r->period_ms = period_ms;
        send_now = true;
    }
    if (ok && r) r->refs++;
    xSemaphoreGive(s_data.mutex);

    if (!ok) {
        ESP_LOGW(TAG, "Store full, cannot subscribe: %s", path);
        return false;
    }
    if (send_now && s_ws && s_state == SK_STATE_CONNECTED) {
        send_subscribe_one(s_ws, path, period_ms);
    }
    return true;
}

void signalk_client_unsubscribe(const char *path)
{
    if (!path || !path[0] || !s_data.mutex) return;

    bool release = false;
    xSemaphoreTake(s_data.mutex, portMAX_DELAY);
    SubRecord *r = sub_find(path);
    if (r && r->refs > 0) {
        r->refs--;
        if (r->refs == 0 && !r->permanent) {
            sub_remove(r);
            store_remove(s_data, path);
            release = true;
        }
    }
    xSemaphoreGive(s_data.mutex);

    if (release && s_ws && s_state == SK_STATE_CONNECTED) {
        send_unsubscribe_one(s_ws, path);
    }
}

void signalk_client_watch(const char *path, signalk_value_cb_t cb, void *user_data)
{
    if (path) {
        strncpy(s_watch_path, path, sizeof(s_watch_path) - 1);
        s_watch_path[sizeof(s_watch_path) - 1] = '\0';
    } else {
        s_watch_path[0] = '\0';
    }
    s_watch_cb = cb;
    s_watch_user_data = user_data;
}
