#pragma once

// Catalog of numeric Signal K paths available on the connected server,
// fetched from /signalk/v1/api/vessels/self with meta (units, displayName).
// Thread-safe; fetch runs in a background task.

#include <stdint.h>
#include <stdbool.h>

struct cJSON;

struct SkPathInfo {
    char  path[96];
    char  display_name[32];
    char  units[12];
    float value;
};

enum SkCatalogState {
    SK_CATALOG_EMPTY = 0,
    SK_CATALOG_LOADING,
    SK_CATALOG_READY,
    SK_CATALOG_ERROR,
};

void     sk_catalog_refresh(bool force);   // no-op if fresh (<30 s) or already loading
int      sk_catalog_state(void);
uint32_t sk_catalog_generation(void);      // increments after each successful fetch
int      sk_catalog_count(void);
bool     sk_catalog_get(int index, SkPathInfo *out);
bool     sk_catalog_find(const char *path, SkPathInfo *out);
bool     sk_catalog_has(const char *path);
cJSON   *sk_catalog_to_json(void);
