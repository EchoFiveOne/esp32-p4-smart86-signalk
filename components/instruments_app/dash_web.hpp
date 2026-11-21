#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// HTTP server (port 80) hosting the dashboard web editor and JSON API:
//   GET  /                   editor page
//   GET  /api/config         dashboard configuration
//   PUT  /api/config         replace configuration
//   POST /api/reset          restore defaults
//   GET  /api/paths          available Signal K paths (?refresh=1 to re-fetch)
//   GET  /api/units          display unit options per Signal K unit
//   GET  /api/values         live formatted tile values
//   GET  /api/screenshot.bmp current screen
//   GET  /api/perf           display frame timing since the last call (diagnostics)
// No authentication: intended for a trusted boat LAN.
void dash_web_start(void);

// Records how long a graphic tile took to render into its cache (reported by /api/perf)
void dash_web_perf_note_gfx(uint32_t ms);

// "http://192.168.1.24/" — false if Wi-Fi has no IP address
bool dash_web_url(char *out, size_t len);
