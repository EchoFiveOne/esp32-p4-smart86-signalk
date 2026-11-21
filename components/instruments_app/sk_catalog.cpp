#include "sk_catalog.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef SIMULATOR_BUILD
#include "day_night.hpp"
#include "tides.hpp"
#include "signalk_auth.hpp"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

static const char *TAG = "sk_catalog";

static const int MAX_ENTRIES = 400;

static SkPathInfo        *s_entries   = nullptr;
static int                s_count     = 0;
static volatile int       s_state     = SK_CATALOG_EMPTY;
static volatile uint32_t  s_gen       = 0;

#ifndef SIMULATOR_BUILD

static SemaphoreHandle_t  s_lock      = nullptr;
static int64_t            s_fetched_us = 0;

struct HttpBuf { char *data; size_t len; size_t cap; };

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    HttpBuf *b = (HttpBuf *)evt->user_data;
    if (b && evt->event_id == HTTP_EVENT_ON_DATA) {
        if (b->len + evt->data_len < b->cap) {
            memcpy(b->data + b->len, evt->data, evt->data_len);
            b->len += evt->data_len;
            b->data[b->len] = '\0';
        }
    }
    return ESP_OK;
}

static int cmp_entry(const void *a, const void *b)
{
    return strcmp(((const SkPathInfo *)a)->path, ((const SkPathInfo *)b)->path);
}

static void walk(cJSON *obj, const char *prefix, SkPathInfo *out, int *count)
{
    cJSON *child;
    cJSON_ArrayForEach(child, obj) {
        const char *key = child->string;
        if (!key || !cJSON_IsObject(child)) continue;
        if (strcmp(key, "meta") == 0 || strcmp(key, "$source") == 0 ||
            strcmp(key, "values") == 0 || strcmp(key, "timestamp") == 0) continue;

        char path[128];
        if (prefix[0]) snprintf(path, sizeof(path), "%s.%s", prefix, key);
        else           snprintf(path, sizeof(path), "%s", key);

        cJSON *val = cJSON_GetObjectItem(child, "value");
        if (val) {
            if (!cJSON_IsNumber(val) || *count >= MAX_ENTRIES) continue;
            if (strlen(path) >= sizeof(out[0].path)) continue;
            SkPathInfo &e = out[(*count)++];
            memset(&e, 0, sizeof(e));
            memcpy(e.path, path, strlen(path) + 1);
            e.value = (float)val->valuedouble;
            cJSON *meta = cJSON_GetObjectItem(child, "meta");
            if (cJSON_IsObject(meta)) {
                cJSON *u  = cJSON_GetObjectItem(meta, "units");
                cJSON *dn = cJSON_GetObjectItem(meta, "displayName");
                if (cJSON_IsString(u))  snprintf(e.units, sizeof(e.units), "%s", u->valuestring);
                if (cJSON_IsString(dn)) snprintf(e.display_name, sizeof(e.display_name), "%s", dn->valuestring);
            }
        } else {
            walk(child, path, out, count);
        }
    }
}

// Values computed on the panel itself (clock, date, sun); listed so they can be picked
static void add_virtual(SkPathInfo *out, int *count)
{
    static const struct { const char *path; const char *name; const char *units; } s_virtual[] = {
        { "panel.time.local",    "Local time (panel clock)",  "tod" },
        { "panel.time.utc",      "UTC time (panel clock)",    "tod" },
        { "panel.date.local",    "Local date (panel clock)",  "date" },
        { "panel.wind.rose",     "Wind rose (graph tile)",    "rad" },
        { "panel.sun",                  "Sun and twilight (graph tile)", "rad" },
        { "panel.sun.sunrise",          "Sunrise (local)",           "tod" },
        { "panel.sun.sunset",           "Sunset (local)",            "tod" },
        { "panel.sun.sunrise.in",       "Time to next sunrise",      "dur" },
        { "panel.sun.sunset.in",        "Time to next sunset",       "dur" },
        { "panel.sun.noon",             "Solar noon (local)",        "tod" },
        { "panel.sun.daylength",        "Day length",                "dur" },
        { "panel.sun.dawn.civil",       "Civil dawn (-6 deg)",       "tod" },
        { "panel.sun.dusk.civil",       "Civil dusk (-6 deg)",       "tod" },
        { "panel.sun.dawn.nautical",    "Nautical dawn (-12 deg)",   "tod" },
        { "panel.sun.dusk.nautical",    "Nautical dusk (-12 deg)",   "tod" },
        { "panel.sun.dawn.astronomical","Astronomical dawn (-18 deg)","tod" },
        { "panel.sun.dusk.astronomical","Astronomical dusk (-18 deg)","tod" },
        { "panel.sun.golden.morning",   "Morning golden hour ends",  "tod" },
        { "panel.sun.golden.evening",   "Evening golden hour starts","tod" },
        { "panel.sun.elevation",        "Sun elevation",             "rad" },
        { "panel.sun.azimuth",          "Sun azimuth (true)",        "rad" },
        { "panel.tide.curve",            "Tide curve (graph tile)",        "m" },
        { "panel.tide.height",           "Tide height above datum", "m" },
        { "panel.tide.trend",            "Tide rate (rising +)",           "m/h" },
        { "panel.tide.range",            "Tide range (current tide)",      "m" },
        { "panel.tide.port.distance",    "Distance to tide port",          "m" },
        { "panel.tide.next.high.time",   "Next high water time",           "tod" },
        { "panel.tide.next.high.in",     "Time to next high water",        "dur" },
        { "panel.tide.next.high.height", "Next high water height",         "m" },
        { "panel.tide.next.high.depth",  "Depth at next high water",       "m" },
        { "panel.tide.next.low.time",    "Next low water time",            "tod" },
        { "panel.tide.next.low.in",      "Time to next low water",         "dur" },
        { "panel.tide.next.low.height",  "Next low water height",          "m" },
        { "panel.tide.next.low.depth",   "Depth at next low water",        "m" },
        { "panel.solunar",               "Moon and solunar (graph tile)",  "ratio" },
        { "panel.moon.illumination",     "Moon illumination",              "ratio" },
        { "panel.moon.rise",             "Next moonrise",                  "tod" },
        { "panel.moon.set",              "Next moonset",                   "tod" },
        { "panel.solunar.major.next",    "Next major solunar period",      "tod" },
        { "panel.solunar.minor.next",    "Next minor solunar period",      "tod" },
    };
    for (const auto &v : s_virtual) {
        if (*count >= MAX_ENTRIES) return;
        SkPathInfo &e = out[(*count)++];
        memset(&e, 0, sizeof(e));
        snprintf(e.path, sizeof(e.path), "%s", v.path);
        snprintf(e.display_name, sizeof(e.display_name), "%s", v.name);
        snprintf(e.units, sizeof(e.units), "%s", v.units);
        float val = 0;
        if (day_night_virtual_value(v.path, &val) || tides_virtual_value(v.path, &val)) e.value = val;
    }
}

static void fetch_task(void *)
{
    const char *base = signalk_auth_get_base_url();
    SkPathInfo *list = nullptr;
    char *resp = nullptr;
    int count = 0;
    bool ok = false;

    if (!base || !base[0]) {
        ESP_LOGW(TAG, "No Signal K server URL yet");
        goto done;
    }
    {
        char url[256];
        snprintf(url, sizeof(url), "%s/signalk/v1/api/vessels/self", base);

        const size_t cap = 512 * 1024;
        resp = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        list = (SkPathInfo *)heap_caps_calloc(MAX_ENTRIES, sizeof(SkPathInfo), MALLOC_CAP_SPIRAM);
        if (!resp || !list) {
            ESP_LOGE(TAG, "Out of memory");
            goto done;
        }
        resp[0] = '\0';
        HttpBuf hb = { resp, 0, cap - 1 };

        esp_http_client_config_t cfg = {};
        cfg.url = url;
        cfg.event_handler = http_event;
        cfg.user_data = &hb;
        cfg.timeout_ms = 15000;
        cfg.buffer_size = 4096;
        cfg.skip_cert_common_name_check = true;
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        static char auth_hdr[560];
        const char *token = signalk_auth_get_token();
        if (token) {
            snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", token);
            esp_http_client_set_header(c, "Authorization", auth_hdr);
        }
        esp_err_t err = esp_http_client_perform(c);
        int status = esp_http_client_get_status_code(c);
        esp_http_client_cleanup(c);

        if (err != ESP_OK || status != 200 || hb.len == 0) {
            ESP_LOGW(TAG, "Fetch failed: err=%s status=%d", esp_err_to_name(err), status);
            goto done;
        }
        cJSON *root = cJSON_ParseWithLength(resp, hb.len);
        if (!root) {
            ESP_LOGW(TAG, "JSON parse failed (%u bytes)", (unsigned)hb.len);
            goto done;
        }
        walk(root, "", list, &count);
        cJSON_Delete(root);
        add_virtual(list, &count);
        qsort(list, count, sizeof(SkPathInfo), cmp_entry);
        ok = true;
        ESP_LOGI(TAG, "Catalog: %d numeric paths", count);
    }

done:
    if (resp) heap_caps_free(resp);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ok) {
        if (s_entries) heap_caps_free(s_entries);
        s_entries = list;
        s_count = count;
        s_fetched_us = esp_timer_get_time();
        s_gen++;
        s_state = SK_CATALOG_READY;
        list = nullptr;
    } else {
        s_state = s_entries ? SK_CATALOG_READY : SK_CATALOG_ERROR;
    }
    xSemaphoreGive(s_lock);
    if (list) heap_caps_free(list);
    vTaskDelete(NULL);
}

void sk_catalog_refresh(bool force)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (s_state == SK_CATALOG_LOADING) return;
    int64_t age = esp_timer_get_time() - s_fetched_us;
    if (!force && s_entries && age < 30LL * 1000000LL) return;
    s_state = SK_CATALOG_LOADING;
    if (xTaskCreate(fetch_task, "sk_catalog", 8192, NULL, 3, NULL) != pdPASS) {
        s_state = s_entries ? SK_CATALOG_READY : SK_CATALOG_ERROR;
    }
}

#define LOCK()   do { if (!s_lock) s_lock = xSemaphoreCreateMutex(); xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define UNLOCK() xSemaphoreGive(s_lock)

#else

void sk_catalog_refresh(bool) {
    static SkPathInfo stub[] = {
        { "environment.wind.speedApparent", "", "m/s", 6.3f },
        { "environment.wind.angleApparent", "", "rad", 0.78f },
        { "navigation.speedOverGround", "", "m/s", 3.1f },
        { "environment.depth.belowTransducer", "", "m", 15.2f },
    };
    s_entries = stub; s_count = 4; s_state = SK_CATALOG_READY; s_gen++;
}
#define LOCK()   do {} while (0)
#define UNLOCK() do {} while (0)

#endif

int      sk_catalog_state(void)      { return s_state; }
uint32_t sk_catalog_generation(void) { return s_gen; }

int sk_catalog_count(void)
{
    LOCK();
    int n = s_count;
    UNLOCK();
    return n;
}

bool sk_catalog_get(int index, SkPathInfo *out)
{
    bool ok = false;
    LOCK();
    if (index >= 0 && index < s_count) { *out = s_entries[index]; ok = true; }
    UNLOCK();
    return ok;
}

bool sk_catalog_find(const char *path, SkPathInfo *out)
{
    bool ok = false;
    LOCK();
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].path, path) == 0) {
            if (out) *out = s_entries[i];
            ok = true;
            break;
        }
    }
    UNLOCK();
    return ok;
}

bool sk_catalog_has(const char *path)
{
    return sk_catalog_find(path, nullptr);
}

cJSON *sk_catalog_to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state",
        s_state == SK_CATALOG_READY ? "ready" :
        s_state == SK_CATALOG_LOADING ? "loading" :
        s_state == SK_CATALOG_ERROR ? "error" : "empty");
    cJSON *arr = cJSON_AddArrayToObject(root, "paths");
    LOCK();
    for (int i = 0; i < s_count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "path", s_entries[i].path);
        cJSON_AddStringToObject(o, "units", s_entries[i].units);
        cJSON_AddStringToObject(o, "name", s_entries[i].display_name);
        cJSON_AddItemToArray(arr, o);
    }
    UNLOCK();
    return root;
}
