#!/usr/bin/env python3
"""Build tides.bin for the panel from the public LINZ tide predictions.

Downloads the LINZ daily high/low predictions (standard ports, CSV) and the
secondary port table, converts NZ local times to UTC and packs everything into
a compact binary that the panel loads from LittleFS (upload it in the web
editor, Tides section, or with --upload <panel-ip>).

Source: Land Information New Zealand (LINZ), tide predictions, CC BY 4.0.
https://www.linz.govt.nz/products-services/tides-and-tidal-streams

Usage:
  python tools/linz_tides_pack.py [--years 2026-2029] [--out tides.bin] [--upload 192.168.1.24]

Only the Python standard library is needed.

File format (little endian), version 1:
  header   32 bytes: "NZT1", u16 version, u16 std_count, u16 port_count,
                     u16 reserved, u32 event_count, u32 first_min, u32 last_min,
                     u32 build_unix, u32 reserved
  std[]    16 bytes: u32 first_event, u32 count, i16 msl_cm, u16 port_index, u32 reserved
  port[]   52 bytes: char name[32], f32 lat, f32 lon, u16 std_index,
                     i16 hw_diff_min, i16 lw_diff_min, i16 msl_cm (secondary),
                     u16 ratio_x1000 (0 = heights unknown), u8 type (0 std, 1 secondary),
                     u8 flags (bit0 = estimated)
  times[]  u32 per event: minutes since 2020-01-01 UTC, bit 31 set = high water
  heights[] i16 per event: centimetres above chart datum
"""

import argparse
import calendar
import csv
import io
import math
import os
import re
import struct
import sys
import tempfile
import time
import unicodedata
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone

LIST_URL = ("https://www.linz.govt.nz/products-services/tides-and-tidal-streams/"
            "tide-predictions/tide-predictions-list-view")
SECONDARY_PAGE = ("https://www.linz.govt.nz/products-services/tides-and-tidal-streams/"
                  "tide-predictions/secondary-ports-tidal-information")
SECONDARY_FALLBACK = ("https://www.linz.govt.nz/sites/default/files/2026-06/"
                      "NZNA_Secondary-ports-2026-27.csv")
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36"
EPOCH = datetime(2020, 1, 1, tzinfo=timezone.utc)
NAME_LEN = 32


def fetch(url, cache_dir, cache=True):
    os.makedirs(cache_dir, exist_ok=True)
    fn = os.path.join(cache_dir, re.sub(r"[^A-Za-z0-9._-]+", "_", urllib.parse.unquote(url.split("/")[-1])))
    if cache and os.path.exists(fn) and os.path.getsize(fn) > 0:
        with open(fn, "rb") as f:
            return f.read()
    req = urllib.request.Request(urllib.parse.quote(url, safe=":/?=&%"), headers={"User-Agent": UA})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                data = r.read()
            break
        except Exception as e:  # network hiccup: retry a couple of times
            if attempt == 2:
                raise RuntimeError(f"download failed: {url}: {e}")
            time.sleep(2)
    with open(fn, "wb") as f:
        f.write(data)
    return data


def decode(data):
    for enc in ("utf-8-sig", "cp1252"):
        try:
            return data.decode(enc)
        except UnicodeDecodeError:
            pass
    return data.decode("latin-1")


def ascii_name(s):
    s = unicodedata.normalize("NFKD", s)
    s = "".join(c for c in s if not unicodedata.combining(c))
    s = s.encode("ascii", "replace").decode().replace("?", "")
    s = re.sub(r"\s+", " ", s).strip()
    return s[:NAME_LEN - 1]


def nz_offset_hours(y, mo, d, hh, mi):
    """NZST +12 / NZDT +13 for a local wall time (rules since 2007)."""
    def last_sunday(year, month):
        last = calendar.monthrange(year, month)[1]
        wd = calendar.weekday(year, month, last)  # Mon = 0
        return last - ((wd - 6) % 7)

    def first_sunday(year, month):
        wd = calendar.weekday(year, month, 1)
        return 1 + ((6 - wd) % 7)

    t = (mo, d, hh * 60 + mi)
    dst_end = (4, first_sunday(y, 4), 3 * 60)     # 03:00 NZDT -> 02:00 NZST
    dst_start = (9, last_sunday(y, 9), 2 * 60)    # 02:00 NZST -> 03:00 NZDT
    return 13 if (t < dst_end or t >= dst_start) else 12


def parse_latlon(field):
    m = re.match(r"\s*(\d+)\D+?(\d+(?:\.\d+)?)\D*([NSEW])", field)
    if not m:
        raise ValueError(f"bad position {field!r}")
    v = int(m.group(1)) + float(m.group(2)) / 60.0
    return -v if m.group(3) in "SW" else v


def parse_standard_csv(text):
    lines = text.splitlines()
    head = next(csv.reader([lines[0]]))
    name = head[1].strip()
    lat, lon = parse_latlon(head[2]), parse_latlon(head[3])
    events = []
    for row in csv.reader(lines[3:]):
        if len(row) < 6 or not row[0].strip().isdigit():
            continue
        d, mo, y = int(row[0]), int(row[2]), int(row[3])
        for k in range(4, len(row) - 1, 2):
            tm, h = row[k].strip(), row[k + 1].strip()
            if not tm or not h:
                continue
            hh, mi = map(int, tm.split(":"))
            off = nz_offset_hours(y, mo, d, hh, mi)
            utc = datetime(y, mo, d, hh, mi, tzinfo=timezone.utc) - timedelta(hours=off)
            events.append((int((utc - EPOCH).total_seconds() // 60), round(float(h) * 100)))
    return name, lat, lon, events


def parse_hhmm(s):
    s = s.strip()
    m = re.fullmatch(r"([+-]?)(\d{2})(\d{2})", s)
    if not m:
        return None
    v = int(m.group(2)) * 60 + int(m.group(3))
    return -v if m.group(1) == "-" else v


def to_float(s):
    try:
        return float(s.strip())
    except (ValueError, AttributeError):
        return None


def parse_secondary(text):
    """Returns list of dicts; standard ports carry is_std=True and their MSL."""
    out = []
    current_std = None
    for row in csv.reader(io.StringIO(text)):
        if len(row) < 16 or not row[1].strip():
            continue
        name = row[1].strip()
        try:
            lat = -(int(row[2]) + float(row[3]) / 60.0)
            lon = int(row[4]) + float(row[5]) / 60.0
        except ValueError:
            continue  # section headings
        if row[6].strip().lower() == "hhmm":
            current_std = {"name": name, "lat": lat, "lon": lon, "msl": to_float(row[14]), "is_std": True}
            out.append(current_std)
            continue
        if current_std is None:
            continue
        out.append({
            "name": name, "lat": lat, "lon": lon, "is_std": False, "std": current_std["name"],
            "hw": parse_hhmm(row[6]), "lw": parse_hhmm(row[8]),
            "msl": to_float(row[14]), "ratio": to_float(row[15]),
            "estimated": row[16].strip() == "*" if len(row) > 16 else False,
        })
    return out


def dist_nm(a_lat, a_lon, b_lat, b_lon):
    p1, p2 = math.radians(a_lat), math.radians(b_lat)
    dl = math.radians(b_lon - a_lon)
    c = math.sin(p1) * math.sin(p2) + math.cos(p1) * math.cos(p2) * math.cos(dl)
    return math.degrees(math.acos(max(-1.0, min(1.0, c)))) * 60.0


def norm(s):
    return re.sub(r"[^a-z]", "", ascii_name(s).lower())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--years", default=None, help="e.g. 2026-2029 (default: this year onwards)")
    ap.add_argument("--out", default="tides.bin")
    ap.add_argument("--cache", default=os.path.join(tempfile.gettempdir(), "linz_tides_cache"),
                    help="download cache directory")
    ap.add_argument("--secondary-url", default=None)
    ap.add_argument("--upload", metavar="PANEL_IP", help="upload the result to the panel")
    args = ap.parse_args()

    this_year = datetime.now().year
    if args.years:
        a, _, b = args.years.partition("-")
        years = set(range(int(a), int(b or a) + 1))
    else:
        years = set(range(this_year, this_year + 10))

    print("Reading LINZ prediction list ...")
    html = decode(fetch(LIST_URL, args.cache, cache=False))
    urls = sorted(set(re.findall(r'href="([^"]+/maj-ports/csv/[^"]+\.csv)"', html)))
    by_port = {}
    for u in urls:
        m = re.search(r"/([^/]+) (\d{4})\.csv$", urllib.parse.unquote(u))
        if m and int(m.group(2)) in years:
            by_port.setdefault(m.group(1), []).append((int(m.group(2)), u))
    if not by_port:
        sys.exit("No standard port CSV links found for the requested years")
    years_found = sorted({y for v in by_port.values() for y, _ in v})
    print(f"  {len(by_port)} ports, years {years_found[0]}-{years_found[-1]}")

    std_ports = []   # dicts with name, lat, lon, events
    for i, (pname, files) in enumerate(sorted(by_port.items())):
        events, lat, lon, name = [], None, None, pname
        for y, u in sorted(files):
            name, lat, lon, ev = parse_standard_csv(decode(fetch(u, args.cache)))
            events.extend(ev)
        events.sort()
        dedup = []
        for t, h in events:
            if dedup and dedup[-1][0] == t:
                continue
            dedup.append((t, h))
        std_ports.append({"name": name, "lat": lat, "lon": lon, "events": dedup})
        print(f"\r  downloaded {i + 1}/{len(by_port)}", end="", flush=True)
    print()

    sec_url = args.secondary_url
    if not sec_url:
        try:
            page = decode(fetch(SECONDARY_PAGE, args.cache, cache=False))
            links = re.findall(r'href="([^"]+Secondary[^"]+\.csv)"', page, re.I)
            if links:
                sec_url = urllib.parse.urljoin(SECONDARY_PAGE, links[0])
        except Exception:
            pass
        sec_url = sec_url or SECONDARY_FALLBACK
    print(f"Reading secondary ports: {sec_url}")
    secondary = parse_secondary(decode(fetch(sec_url, args.cache)))

    # MSL of each standard port: from the secondary table if listed, else the event mean
    std_by_norm = {norm(p["name"]): i for i, p in enumerate(std_ports)}
    for p in std_ports:
        hs = [h for _, h in p["events"]]
        p["msl"] = round(sum(hs) / len(hs)) if hs else 0
    for s in secondary:
        if s["is_std"] and s["msl"] is not None and norm(s["name"]) in std_by_norm:
            std_ports[std_by_norm[norm(s["name"])]]["msl"] = round(s["msl"] * 100)

    ports = []  # (name, lat, lon, std_idx, hw, lw, msl_cm, ratio_x1000, type, flags)
    for i, p in enumerate(std_ports):
        ports.append((p["name"], p["lat"], p["lon"], i, 0, 0, p["msl"], 1000, 0, 0))
    skipped = 0
    for s in secondary:
        if s["is_std"]:
            continue
        si = std_by_norm.get(norm(s["std"]))
        if si is None or s["hw"] is None or s["lw"] is None:
            skipped += 1
            continue
        # A port that LINZ also predicts in full is better served by its own table
        if norm(s["name"]) in std_by_norm or any(
                dist_nm(s["lat"], s["lon"], p["lat"], p["lon"]) < 0.5 for p in std_ports):
            continue
        ratio, msl = s["ratio"], s["msl"]
        flags = 1 if s["estimated"] else 0
        if ratio is None or msl is None:
            ratio, msl, flags = 0.0, std_ports[si]["msl"] / 100.0, flags | 1
        ports.append((s["name"], s["lat"], s["lon"], si, s["hw"], s["lw"],
                      round(msl * 100), round(ratio * 1000), 1, flags))
    print(f"  {len(ports) - len(std_ports)} secondary ports ({skipped} without a usable standard port)")

    # Pack
    times, heights, std_tab = [], [], []
    for p_idx, p in enumerate(std_ports):
        ev = p["events"]
        first = len(times)
        for k, (t, h) in enumerate(ev):
            prev_h = ev[k - 1][1] if k > 0 else None
            next_h = ev[k + 1][1] if k + 1 < len(ev) else None
            ref = prev_h if prev_h is not None else next_h
            high = h > ref if ref is not None else True
            times.append((t & 0x7FFFFFFF) | (0x80000000 if high else 0))
            heights.append(max(-32768, min(32767, h)))
        std_tab.append(struct.pack("<IIhHI", first, len(ev), p["msl"], p_idx, 0))

    all_t = [t & 0x7FFFFFFF for t in times]
    out = bytearray()
    out += struct.pack("<4sHHHHIIIII", b"NZT1", 1, len(std_ports), len(ports), 0,
                       len(times), min(all_t), max(all_t), int(time.time()), 0)
    out += b"".join(std_tab)
    for (name, lat, lon, si, hw, lw, msl, ratio, typ, flags) in ports:
        nb = ascii_name(name).encode("ascii")
        out += struct.pack("<32sffHhhhHBB", nb, lat, lon, si, hw, lw, msl, ratio, typ, flags)
    out += struct.pack(f"<{len(times)}I", *times)
    out += struct.pack(f"<{len(heights)}h", *heights)

    with open(args.out, "wb") as f:
        f.write(out)
    first = EPOCH + timedelta(minutes=min(all_t))
    last = EPOCH + timedelta(minutes=max(all_t))
    print(f"Wrote {args.out}: {len(out) / 1024:.0f} KB, {len(ports)} ports, {len(times)} events, "
          f"{first:%Y-%m-%d} to {last:%Y-%m-%d} UTC")

    if args.upload:
        url = f"http://{args.upload}/api/tides"
        print(f"Uploading to {url} ...")
        req = urllib.request.Request(url, data=bytes(out), method="PUT",
                                     headers={"Content-Type": "application/octet-stream"})
        with urllib.request.urlopen(req, timeout=300) as r:
            print(r.read().decode())


if __name__ == "__main__":
    main()
