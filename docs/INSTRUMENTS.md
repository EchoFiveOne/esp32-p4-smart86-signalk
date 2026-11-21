# Instruments App (multi-function display)

An i70 / Triton2-style dashboard: any numeric Signal K path shown as large numbers in
configurable grids, across multiple swipeable pages. Component: `components/instruments_app/`.

## Using it on the panel

| Action | Result |
|--------|--------|
| Swipe left/right (start mid-screen) | Change page. Swipes within 50 px of the edge are the esp-ui Back gesture. |
| Tap a tile | Acknowledge an active alarm, otherwise toggle the min/max overlay |
| Long-press a tile | Tile editor (data path, label, units, decimals, angle mode, smoothing, trend, alarms, beep, reset min/max, clear) |
| Gear (top right) | Page settings (name, layout, group, add/duplicate/move/delete) and dashboard settings (theme, auto-cycle, auto anchor/sail, no-data timeout, start page, lock). Brightness and night mode live in **Settings > Display** (all apps). The touch area extends 24 px beyond the icon. |

- **Layouts** (12×12 grid): `1`, `2r` (2 rows), `2c` (2 columns), `3r`, `4r`, `1+2`, `1+3` (1 large + 3), `1+4`,
  `2+3` (2 large + 3), `2+4`, `2x2`, `2x3`, `3x2`, `3x3`, `4x4`. In the `1+n`/`2+n` layouts the small tiles are
  squares along the bottom and the large tile(s) fill the space above.
- **Units**: per tile, or `auto` (follows the Settings app unit preferences). Speed kn/km/h/mph/m/s/Bft,
  depth/distance m/ft/fm/NM/km/mi, temperature °C/°F/K, pressure hPa/mbar/inHg/kPa/psi/bar, ratio %, volume, etc.
- **Angles**: `auto` shows apparent/true wind angles as 0–180° with P/S (red/green); headings/courses as 0–359°.
  Angles are smoothed on the unit circle so they don't jump at 359→0.
- **Smoothing**: off / light / medium / heavy (EMA).
- **Trend**: optional 60-point sparkline in the lower part of the tile (1 s, 5 s, 15 s or 60 s per point);
  the value font shrinks to fit above it.
- **Panel values** (no Signal K path needed): `panel.time.local` (local time, DST applied), `panel.time.utc`,
  `panel.date.local` (DD/MM), `panel.sun.sunrise`, `panel.sun.sunset` (local time) and `panel.sun.elevation`.
  Use units `tod` (HH:MM) for times and `date` for the date. Sun values need a position (`navigation.position`).
- **Alarms**: low/high thresholds in display units; the tile flashes, the page dot turns red and the
  panel beeps every 4 s (if enabled) until acknowledged with a tap.
- **Themes**: Dark, Day (high contrast), Night (red), Auto (Day theme by day, Dark at night, following Settings > Display).

## Day/night mode (all apps)

**Settings > Display** (or the "Panel display" section of the web editor) controls brightness for the whole panel:
- **Mode**: Auto (sun), Day or Night. Auto switches when the calculated sun elevation at the boat's position
  crosses the chosen threshold: sunset (0°), civil dusk (−6°) or nautical dusk (−12°).
- **Day / night brightness**: 5–100 % and 1–100 %. Levels below 20 % are reached by software dimming.
- **Red filter**: renders every app in red at night.

Position comes from `navigation.position`; time from NTP (or `navigation.datetime` if plausible). Settings >
Time zone has an **Auto (GPS position)** option that picks the time zone and DST rules from the position
(also selectable under "Panel display" in the web editor). Without a live position it uses the last saved
position; before any position has ever been received it uses New Zealand time (`AUTO_TZ_FALLBACK` in
`unit_config.cpp`).
- **Lock**: disables long-press editing (gear menu remains available to unlock).
- **Auto-cycle**: rotates pages every N seconds after 30 s without touch. With auto anchor/sail on and
  "cycle within group" set, it only rotates through the pages of the current group.
- Values show `---` when no update arrives within the no-data timeout.

On first boot a default set of pages (Sail, Nav, Depth, Env…) is created, keeping only tiles whose paths
the server actually provides.

## Auto anchor / sail pages

Each page can belong to the **Anchor** group, the **Sail** group, or no group. Set this with "Page group" in
the gear menu or "Group" in the web editor; tabs there show ⚓ / ⛵. With **Auto anchor / sail** on (gear menu
or web editor settings), the panel watches `navigation.speedOverGround`, smoothed over about 2 s:

- SOG below the threshold (default 2 kn) for the anchor delay (default 5 min) → **Anchor**.
- SOG above the threshold for the sail delay (default 60 s) → **Sail**. A speed exactly at the threshold
  counts towards neither.

When the state changes, the Instruments app jumps to the first page of that group. It waits until the panel
has had no touch for 5 s and no menu is open. It doesn't jump if you're already on a page in that group. If
the state changes while another app is open, the jump happens the next time Instruments is opened. Every page
stays reachable by swiping; the switch only happens on a state change. SOG older than 30 s is ignored, so the
countdown pauses without GPS. The web editor shows the current state, SOG and countdown. Settings keys are
`autoSwitch`, `autoSpeed` (kn, 0.2–20), `anchorDelay` / `sailDelay` (s, 5–3600) and `cycleGroup`; the page
key is `group` (`anchor`/`sail`).

## Tides and solunar (New Zealand)

Tide predictions come from the LINZ standard and secondary port tables (Land Information New Zealand,
CC BY 4.0), packed into a compact file and stored on the panel. They are astronomical predictions only:
wind and weather can shift real levels by ±10–30 cm, and small secondary ports are less accurate.

**Installing the tide table**
1. Build it on a PC: `python tools/linz_tides_pack.py --years 2026-2029 --out tides.bin`
   (downloads the LINZ CSVs, about 3 MB result). Add `--upload <panel-ip>` to send it straight to the panel.
2. Or upload `tides.bin` in the web editor under **Tides (NZ, LINZ)**. The file is validated before it
   replaces the old one and survives reboots and app flashing. Rebuild it every few years.

**Settings** (web editor, applied immediately, stored in NVS)
- **Tide port**: Auto picks the nearest port with heights within 50 NM of `navigation.position`
  (re-checked after moving 2 NM), or choose a port by name. Ports marked "times only" have no heights.
- **Transducer offset**: metres from the water surface down to the transducer. Leave blank to use
  `environment.depth.surfaceToTransducer`. `environment.depth.belowSurface` is used first if available.
- **Barometric correction**: −1 cm per hPa above 1013.25 hPa (inverse barometer), using
  `environment.outside.pressure` when it is less than 15 minutes old.

**Projected depth** at the next high/low water = depth below the surface now + (tide height at that event −
tide height now). Example: 1.5 m below the transducer + 0.3 m offset = 1.8 m now; halfway through a 2 m rise
to high water, 1.0 m is still to come, so the projected depth at high water is 2.8 m.

**Tiles** (`panel.*` paths, listed under "Panel" in the path pickers):

| Path | Shows |
|------|-------|
| `panel.tide.curve` | Graphic: ±12 h tide curve, high/low times and heights, now marker, rate, range, port. Best in the large tile of `1+3` |
| `panel.tide.height`, `panel.tide.trend`, `panel.tide.range` | Height above chart datum now, rate (m/h), range of the current tide |
| `panel.tide.next.high.time` / `.in` / `.height` / `.depth` | Next high water: local time, countdown (H:MM), height, projected depth |
| `panel.tide.next.low.time` / `.in` / `.height` / `.depth` | Same for the next low water |
| `panel.tide.port.distance` | Distance to the selected port |
| `panel.solunar` | Graphic laid out like the sun tile: moon phase disc (mirrored in the southern hemisphere), phase name, % lit, altitude/azimuth, ±12 h moon altitude curve over the sun's sky bands with solunar periods shaded, and a time-ordered list of the next moonrise/set, overhead/underfoot and major/minor periods |
| `panel.moon.illumination`, `panel.moon.phase`, `panel.moon.rise`, `panel.moon.set` | Moon values (rise/set as local time) |
| `panel.solunar.major.next`, `panel.solunar.minor.next` | Centre of the current or next solunar period |

A "Tide" page (`1+3`: curve, HW IN, LW IN, LW DPT) can be added from the web editor. Solunar majors are moon
transit/underfoot ±1 h, minors moonrise/set ±30 min.

## Sun and twilight

Calculated on the panel from `navigation.position` and the clock (works anywhere, no table needed).

| Path | Shows |
|------|-------|
| `panel.sun` | Graphic: sun elevation/azimuth, ±12 h elevation curve over sky bands (night, astronomical, nautical, civil, golden hour, day), and a table of dawn/dusk times. Large tiles show the table beside or below the curve; small tiles show a compact rise/set line |
| `panel.sun.elevation`, `panel.sun.azimuth` | Sun elevation and true azimuth as number tiles |
| `panel.sun.sunrise`, `panel.sun.sunset`, `panel.sun.noon` | Local times (noon = highest elevation) |
| `panel.sun.sunrise.in`, `panel.sun.sunset.in` | Countdown to the next sunrise / sunset |
| `panel.sun.daylength` | Sunrise to sunset duration |
| `panel.sun.dawn.civil` / `.nautical` / `.astronomical` | Morning twilight start: sun at −6°, −12°, −18° |
| `panel.sun.dusk.civil` / `.nautical` / `.astronomical` | Evening twilight end at the same angles |
| `panel.sun.golden.morning`, `panel.sun.golden.evening` | End of morning / start of evening golden hour (sun at +6°) |

Times show today's values and roll over to tomorrow once evening astronomical twilight has ended. Events that
don't happen (e.g. no astronomical darkness at high latitudes in summer) show `---`.

## Wind rose

`panel.wind.rose` (listed as "Wind rose (graph tile)") replaces the former standalone Wind app and uses the
dashboard theme colours. Best in the large tile of `1+4` (e.g. a "Wind" page with AWA, TWA, STW and HDG below).

- The compass card turns with `navigation.headingMagnetic` (or `headingTrue`), heading up; without a heading
  it shows bow-relative angles (0–180 each side).
- Port (red) and starboard (green) close-hauled sectors and the lubber line are fixed to the bow.
- Markers: apparent wind angle (value colour) and true wind angle (accent colour) from
  `environment.wind.angleApparent` / `angleTrueWater`.
- Centre: AWS (`speedApparent`) in the largest font that fits, TWS (`speedTrue`) below, in the automatic
  wind speed unit. Small tiles show only the card, markers and AWS.
- Angles and speeds are smoothed (vector average for angles) and the tile redraws when they change by 1° / 0.1.

## Display performance

- The firmware is built with `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF`), LVGL refreshes and samples touch
  every 15 ms, uses the newlib `memcpy`/`memset` (`CONFIG_LV_MEMCPY_MEMSET_STD`), and draws in 50-line strips
  into an internal-SRAM buffer (rendering into PSRAM was about twice as slow).
- Graphic tiles (tide curve, sun, solunar, wind rose) are rendered once into a PSRAM image and copied to the
  screen, so swipes don't redraw curves and text. The visible page is kept current (the wind rose at most twice
  a second); other pages refresh one tile per 250 ms tick (neighbours at most every 5 s), and nothing
  re-renders while a swipe is in progress.
- Measured while auto-cycling pages: average frame 65 ms → 36 ms, worst frame 180 ms → 110 ms.
- `GET /api/perf` returns frame timing since the previous call (average/max render ms, frames over 33 ms,
  graphic tile render times, free memory).

## Web editor

Browse to `http://<panel-ip>/` (the URL is shown in the gear menu). The editor offers the same page and
tile editing with a live preview, a path picker fed from the server catalog, Export/Import of the
configuration as JSON, Reset, and a live screenshot of the panel.

The path picker is a combo box:
- Clicking the field or **▾** lists every path grouped by top-level group, with the current path marked ✓.
- Typing filters the list by any words in any order, across the path, name and units (e.g. `spe wind`), with matches highlighted.
- Use ↑/↓ and Enter/Tab to pick a path, or Esc to revert.
- A path the server doesn't report yet can be entered as typed, as long as it looks like a Signal K path (`a.b.c`).

> **Security**: the editor has no authentication. Only use it on a trusted network.

The panel's web server handles a limited number of connections. If a request is reset while the page loads,
the editor retries it, and loads the configuration on the next poll if the first load failed.

### HTTP API

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/config` | Current dashboard JSON (includes `revision`) |
| PUT/POST | `/api/config` | Replace dashboard; returns the normalised config |
| POST | `/api/reset` | Restore default pages |
| GET | `/api/paths[?refresh=1]` | Numeric paths from the Signal K server (`state`: loading/ready/error) |
| GET | `/api/units` | Unit choices per Signal K unit |
| GET | `/api/values` | Live formatted values per page/tile, `connected`, and `motion` (`state`, `sog`, `pending`, `pendingS`, `needS`) when auto anchor/sail is on |
| GET | `/api/screenshot.bmp` | 720×720 screenshot of the current screen |
| GET | `/api/perf` | Display frame timing and memory since the previous call |
| GET | `/api/display` | Day/night `settings` (incl. `timeZone`, `timeZones` list) and `status` (night, level, sun elevation, sunrise/sunset, time zone) |
| PUT | `/api/display` | Update any of `mode` (auto/day/night), `day`, `night`, `red`, `switch` (0/−6/−12), `timeZone` (option name, e.g. `Auto (GPS position)`) |
| GET | `/api/tides[?ports=1]` | Tide table info, settings, selected port, height/rate, next events with projected depth, moon/solunar; `ports=1` adds the port list |
| PUT | `/api/tides` | Upload `tides.bin` (`application/octet-stream`); validated, then installed |
| PUT | `/api/tides/settings` | Any of `port` (`""` = auto), `offset` (metres or `null`), `baro` (bool) |

## Storage

- Dashboard: `/dash/dashboard.json` on LittleFS (the `spiffs` partition, formatted on first mount).
- Tide table: `/dash/tides.bin` (about 3 MB, loaded into PSRAM); tide settings in NVS namespace `tides`.
- Last viewed page: NVS namespace `dash`, key `last`.
- Flashing only the app (`0x10000`) preserves all of these.

## Architecture

| File | Role |
|------|------|
| `dash_model.*` | Config structs, layouts, unit conversion/formatting, JSON, persistence (revision counter) |
| `dash_engine.*` | 250 ms LVGL timer running from boot: subscription sync, smoothing, min/max, trend, alarms |
| `sk_catalog.*` | Background HTTP fetch of all numeric paths + units from `/signalk/v1/api/vessels/self` |
| `tides.*` | LINZ tide table loader, port selection, tide/depth projection, moon/solunar calculator, `panel.tide.*`/`panel.moon.*` values |
| `instruments_app.*` | esp-ui app: tileview pages, tile rendering/font fitting, custom-drawn tide curve, solunar, sun and wind rose tiles (cached as images), editor/picker/menu overlays |
| `dash_web.*`, `dash_editor.html` | `esp_http_server` on port 80 + embedded editor page |

Subscriptions are reference-counted in `signalk_client` (`signalk_client_subscribe/unsubscribe`), and
re-sent on reconnect. signalk-server only supports a wildcard `unsubscribe` (a per-path unsubscribe makes
it close the stream), so the client unsubscribes from `*` and re-sends the remaining subscriptions.

Value fonts: `lv_font_inst_{176,144,116,92,72,56,44}.c` (Montserrat Bold; digits, `-`, `.`, `%`, space, `°`)
plus `lv_font_montserrat_{360,310,120}.c` (360/310/210 px, in the same component). The largest font whose width fits the tile is chosen.

Titles and units use `lv_font_inst_label_{36,30}.c` (Montserrat Bold). A title gets the tile width less the
unit width. It uses 36 px if it fits, otherwise 30 px. If it still doesn't fit, it is shortened to a single
line ending in `...`, so it never wraps into the value area.
