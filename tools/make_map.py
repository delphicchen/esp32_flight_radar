#!/usr/bin/env python3
"""Generate map_data.h (radar map outline + ATC overlay data) for any location.

Outline: downloads Natural Earth 1:10m public-domain vector data (coastline +
country borders, optionally state/province borders), clips it to a bounding
box around your home coordinates, simplifies it with Douglas-Peucker, and
writes a map_data.h in the exact format the firmware expects.

ATC overlays (all optional, on by default where data is free):
  AIRPORTS[] / RUNWAYS[]  airports with ICAO code + runway centerlines with
                          extension lines, from OurAirports (public domain)
  FIXES[]                 navaids (VOR/NDB/DME) from OurAirports; add real
                          5-letter waypoints with --fixes-csv (name,lat,lon)
  AIRSPACES[]             CTR/TMA/CTA polygon boundaries, from a local
                          GeoJSON (--airspace-geojson) or openAIP
                          (--openaip-key, free account: www.openaip.net)

Examples:
    python make_map.py --lat 35.6762 --lon 139.6503 --radius 150   # Tokyo
    python make_map.py --lat 51.5074 --lon -0.1278 --radius 300 --states
    python make_map.py --lat 25.03 --lon 121.56 --radius 200 --geojson my.geojson
    # keep the existing outline (e.g. the stock g0v Taiwan one), refresh overlays only:
    python make_map.py --lat 23.8 --lon 121.0 --radius 320 --countries TW --no-outline
    python make_map.py --lat 25.03 --lon 121.56 --radius 200 \
        --openaip-key YOURKEY --fixes-csv my_fixes.csv

Pure standard library - no third-party packages needed.
Note: home locations within ~5 deg of the antimeridian (lon +/-180) are not
handled (neither by the firmware's equirectangular projection).
"""
import argparse
import csv
import json
import math
import os
import re
import sys
import urllib.request

NE_BASE = "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/"
OA_BASE = "https://davidmegginson.github.io/ourairports-data/"
SOURCES = {
    "coastline": NE_BASE + "ne_10m_coastline.geojson",
    "borders": NE_BASE + "ne_10m_admin_0_boundary_lines_land.geojson",
    "states": NE_BASE + "ne_10m_admin_1_states_provinces_lines.geojson",
    "airports": OA_BASE + "airports.csv",
    "runways": OA_BASE + "runways.csv",
    "navaids": OA_BASE + "navaids.csv",
}
KM_PER_DEG_LAT = 110.574
KM_PER_DEG_LON = 111.320  # at equator; scaled by cos(lat)

AIRPORT_RANK = {"small_airport": 0, "medium_airport": 1, "large_airport": 2}
CLS_MAP = {"CTR": 0, "TMA": 1, "CTA": 2}          # anything else -> 3
OPENAIP_TYPE = {4: "CTR", 7: "TMA", 26: "CTA"}    # openAIP v2 numeric type codes


def fetch(name, cache_dir):
    """Download a source file into cache_dir unless already cached."""
    url = SOURCES[name]
    path = os.path.join(cache_dir, os.path.basename(url))
    if os.path.exists(path) and os.path.getsize(path) > 0:
        print(f"  cache hit: {path}")
        return path
    os.makedirs(cache_dir, exist_ok=True)
    print(f"  downloading {url} ...")
    tmp = path + ".part"
    with urllib.request.urlopen(url, timeout=120) as r, open(tmp, "wb") as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
    os.replace(tmp, path)
    print(f"  saved {os.path.getsize(path)/1e6:.1f} MB")
    return path


def iter_polylines(geometry):
    """Yield lists of (lon, lat) tuples from any common GeoJSON geometry."""
    t, c = geometry.get("type"), geometry.get("coordinates")
    if t == "LineString":
        yield c
    elif t == "MultiLineString" or t == "Polygon":
        yield from c
    elif t == "MultiPolygon":
        for poly in c:
            yield from poly
    elif t == "GeometryCollection":
        for g in geometry.get("geometries", []):
            yield from iter_polylines(g)


def clip_polyline(pts, lat0, lon0, dlat, dlon):
    """Split a polyline into runs that touch the bbox around (lat0, lon0).

    A segment is kept if either endpoint is inside the bbox, so lines
    crossing the box edge are not cut short.
    """
    def inside(p):
        return abs(p[1] - lat0) <= dlat and abs(p[0] - lon0) <= dlon

    runs, cur = [], []
    flags = [inside(p) for p in pts]
    for i, p in enumerate(pts):
        keep = flags[i] or (i > 0 and flags[i - 1]) or (i + 1 < len(pts) and flags[i + 1])
        if keep:
            cur.append(p)
        elif cur:
            runs.append(cur)
            cur = []
    if cur:
        runs.append(cur)
    return [r for r in runs if len(r) >= 2]


def dp_simplify(pts, tol):
    """Douglas-Peucker on (x, y) points, iterative to avoid recursion limits."""
    n = len(pts)
    if n < 3:
        return list(pts)
    keep = [False] * n
    keep[0] = keep[-1] = True
    stack = [(0, n - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = pts[a]
        bx, by = pts[b]
        dx, dy = bx - ax, by - ay
        seg2 = dx * dx + dy * dy
        dmax, imax = -1.0, -1
        for i in range(a + 1, b):
            px, py = pts[i]
            if seg2 == 0:
                d2 = (px - ax) ** 2 + (py - ay) ** 2
            else:
                t = ((px - ax) * dx + (py - ay) * dy) / seg2
                t = 0.0 if t < 0 else (1.0 if t > 1 else t)
                d2 = (px - ax - t * dx) ** 2 + (py - ay - t * dy) ** 2
            if d2 > dmax:
                dmax, imax = d2, i
        if dmax > tol * tol:
            keep[imax] = True
            stack.append((a, imax))
            stack.append((imax, b))
    return [pts[i] for i in range(n) if keep[i]]


def build(polylines, lat0, lon0, tol, coslat):
    """Clip -> project lon by cos(lat) -> simplify -> back to (lat, lon)."""
    out = []
    for pl in polylines:
        scaled = [(lon * coslat, lat) for lon, lat in pl]
        simp = dp_simplify(scaled, tol)
        if len(simp) >= 2:
            out.append([(y, x / coslat) for x, y in simp])  # (lat, lon)
    return out


# ---------------------------------------------------------------- ATC overlays

def ffloat(s):
    """Parse a CSV float field; '' or garbage -> None."""
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def in_box(lat, lon, lat0, lon0, dlat, dlon):
    return lat is not None and lon is not None and \
        abs(lat - lat0) <= dlat and abs(lon - lon0) <= dlon


def dist_km(lat, lon, lat0, lon0, coslat):
    return math.hypot((lat - lat0) * KM_PER_DEG_LAT,
                      (lon - lon0) * KM_PER_DEG_LON * coslat)


def load_airports(path, lat0, lon0, dlat, dlon, min_rank, countries):
    """OurAirports airports.csv -> [(icao, lat, lon)], sorted by distance."""
    out = []
    with open(path, encoding="utf-8") as f:
        for row in csv.DictReader(f):
            rank = AIRPORT_RANK.get(row["type"], -1)
            if rank < 0:
                continue                      # heliport / seaplane / closed
            if rank < min_rank and row.get("scheduled_service") != "yes":
                continue
            if countries and row.get("iso_country") not in countries:
                continue
            lat, lon = ffloat(row["latitude_deg"]), ffloat(row["longitude_deg"])
            if not in_box(lat, lon, lat0, lon0, dlat, dlon):
                continue
            icao = row.get("icao_code") or row.get("gps_code") or row["ident"]
            if not re.fullmatch(r"[A-Z][A-Z0-9]{3}", icao or ""):
                continue                      # skip strips without a real ICAO code
            out.append((icao, lat, lon, row["ident"]))
    out.sort(key=lambda a: dist_km(a[1], a[2], lat0, lon0, math.cos(math.radians(lat0))))
    return out


def extend_line(lat1, lon1, lat2, lon2, ext_km, coslat):
    """Return (xlat1, xlon1, xlat2, xlon2): both ends pushed out by ext_km."""
    x1, y1 = lon1 * KM_PER_DEG_LON * coslat, lat1 * KM_PER_DEG_LAT
    x2, y2 = lon2 * KM_PER_DEG_LON * coslat, lat2 * KM_PER_DEG_LAT
    dx, dy = x2 - x1, y2 - y1
    length = math.hypot(dx, dy)
    if length < 0.05:                         # degenerate runway record
        return None
    ux, uy = dx / length, dy / length
    def to_ll(x, y):
        return (y / KM_PER_DEG_LAT, x / (KM_PER_DEG_LON * coslat))
    a = to_ll(x1 - ux * ext_km, y1 - uy * ext_km)
    b = to_ll(x2 + ux * ext_km, y2 + uy * ext_km)
    return (a[0], a[1], b[0], b[1])


def load_runways(path, idents, ext_km, coslat):
    """OurAirports runways.csv -> [(lat1,lon1,lat2,lon2, xlat1,xlon1,xlat2,xlon2)]."""
    out = []
    with open(path, encoding="utf-8") as f:
        for row in csv.DictReader(f):
            if row["airport_ident"] not in idents or row.get("closed") == "1":
                continue
            lat1, lon1 = ffloat(row["le_latitude_deg"]), ffloat(row["le_longitude_deg"])
            lat2, lon2 = ffloat(row["he_latitude_deg"]), ffloat(row["he_longitude_deg"])
            if None in (lat1, lon1, lat2, lon2):
                continue                      # many small strips lack threshold coords
            ext = extend_line(lat1, lon1, lat2, lon2, ext_km, coslat)
            if ext:
                out.append((lat1, lon1, lat2, lon2) + ext)
    return out


def load_navaids(path, lat0, lon0, dlat, dlon, countries):
    """OurAirports navaids.csv -> [(name<=5, lat, lon)]."""
    out = []
    with open(path, encoding="utf-8") as f:
        for row in csv.DictReader(f):
            if countries and row.get("iso_country") not in countries:
                continue
            lat, lon = ffloat(row["latitude_deg"]), ffloat(row["longitude_deg"])
            if not in_box(lat, lon, lat0, lon0, dlat, dlon):
                continue
            name = (row.get("ident") or "").strip().upper()[:5]
            if name:
                out.append((name, lat, lon))
    return out


def load_fixes_csv(files, lat0, lon0, dlat, dlon):
    """User CSV 'name,lat,lon' (header optional) -> [(name<=5, lat, lon)]."""
    out = []
    for path in files:
        with open(path, encoding="utf-8") as f:
            for row in csv.reader(f):
                if len(row) < 3:
                    continue
                lat, lon = ffloat(row[1]), ffloat(row[2])
                if lat is None:
                    continue                  # header line
                if in_box(lat, lon, lat0, lon0, dlat, dlon):
                    out.append((row[0].strip().upper()[:5], lat, lon))
    return out


def iter_outer_rings(geometry):
    """Yield outer rings ((lon,lat) lists) of Polygon/MultiPolygon geometries."""
    t, c = geometry.get("type"), geometry.get("coordinates")
    if t == "Polygon" and c:
        yield c[0]
    elif t == "MultiPolygon":
        for poly in c or []:
            if poly:
                yield poly[0]


def airspace_features_from_geojson(files):
    feats = []
    for path in files:
        with open(path, encoding="utf-8") as f:
            gj = json.load(f)
        feats.extend(gj["features"] if gj.get("type") == "FeatureCollection" else [gj])
    return feats


def fetch_openaip_airspaces(key, lat0, lon0, dlat, dlon):
    """openAIP core API -> GeoJSON-like feature list (needs a free API key)."""
    feats, page = [], 1
    while True:
        url = ("https://api.core.openaip.net/api/airspaces?limit=1000&page=%d"
               "&bbox=%.4f,%.4f,%.4f,%.4f&apiKey=%s"
               % (page, lon0 - dlon, lat0 - dlat, lon0 + dlon, lat0 + dlat, key))
        # Cloudflare 會擋 Python-urllib 的預設 UA(error 1010),要給正常的 User-Agent
        req = urllib.request.Request(url, headers={"x-openaip-api-key": key,
                                                   "User-Agent": "Mozilla/5.0 (make_map.py)"})
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                data = json.load(r)
        except urllib.error.HTTPError as e:
            sys.exit(f"openAIP request failed: HTTP {e.code} {e.read()[:200]!r}\n"
                     "check the API key (free account at www.openaip.net)")
        items = data.get("items", [])
        for it in items:
            typ = it.get("type")
            feats.append({"geometry": it.get("geometry") or {},
                          "properties": {"name": it.get("name", ""),
                                         "type": OPENAIP_TYPE.get(typ, str(typ))}})
        if not items or page >= int(data.get("totalPages", 1)):
            return feats
        page += 1


def build_airspaces(features, lat0, lon0, dlat, dlon, tol, coslat, allowed):
    """Filter/clip/simplify airspace polygons -> [(name, cls, [(lat,lon)...])]."""
    out = []
    for ft in features:
        props = ft.get("properties") or {}
        typ = str(props.get("type") or props.get("class")
                  or props.get("TYPE") or "").strip().upper()
        if typ and typ not in allowed:
            continue
        name = str(props.get("name") or props.get("NAME")
                   or props.get("title") or typ or "?").strip().upper()[:31]
        cls = CLS_MAP.get(typ, 3)
        for ring in iter_outer_rings(ft.get("geometry") or {}):
            if not any(in_box(p[1], p[0], lat0, lon0, dlat, dlon) for p in ring):
                continue
            scaled = [(lon * coslat, lat) for lon, lat in ring]
            simp = dp_simplify(scaled, tol)
            if len(simp) >= 4:                # closed ring needs >= 4 points
                out.append((name, cls, [(y, x / coslat) for x, y in simp]))
    return out


# ------------------------------------------------------------------- emission

def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def read_existing_outline(path):
    """Extract the verbatim MAP_OUTLINE block from an existing map_data.h."""
    try:
        with open(path, encoding="utf-8") as f:
            txt = f.read()
    except OSError:
        sys.exit(f"--no-outline: cannot read existing {path} to preserve its outline")
    a = txt.find("inline const float MAP_OUTLINE[] = {")
    b = txt.find("};", a)
    if a < 0 or b < 0:
        sys.exit(f"--no-outline: no MAP_OUTLINE block found in {path}")
    return txt[a:b + 2]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--lat", type=float, required=True, help="home latitude")
    ap.add_argument("--lon", type=float, required=True, help="home longitude")
    ap.add_argument("--radius", type=float, required=True, help="max radar range you plan to use, km")
    ap.add_argument("--states", action="store_true", help="also include state/province borders")
    ap.add_argument("--geojson", action="append", default=[],
                    help="use local GeoJSON file(s) for the outline instead of Natural Earth")
    ap.add_argument("--tol", type=float, default=0.0,
                    help="Douglas-Peucker tolerance in degrees (default: ~1 radar pixel)")
    ap.add_argument("--max-points", type=int, default=4000,
                    help="outline point budget; tolerance is raised automatically if exceeded")
    ap.add_argument("--no-outline", action="store_true",
                    help="keep the MAP_OUTLINE block already in --out (only refresh ATC overlays)")
    ap.add_argument("--no-airports", action="store_true", help="skip AIRPORTS[]/RUNWAYS[]")
    ap.add_argument("--no-fixes", action="store_true", help="skip OurAirports navaids in FIXES[]")
    ap.add_argument("--fixes-csv", action="append", default=[],
                    help="extra waypoints CSV 'NAME,lat,lon' (e.g. 5-letter AIP fixes); repeatable")
    ap.add_argument("--airspace-geojson", action="append", default=[],
                    help="local GeoJSON with CTR/TMA polygons (properties: name, type); repeatable")
    ap.add_argument("--openaip-key", default="",
                    help="openAIP API key: fetch CTR/TMA/CTA airspaces (free account, CC BY-NC data)")
    ap.add_argument("--airspace-types", default="CTR,TMA,CTA",
                    help="airspace types to keep (comma list, default CTR,TMA,CTA)")
    ap.add_argument("--rwy-ext", type=float, default=10.0,
                    help="runway centerline extension beyond each end, km (default 10)")
    ap.add_argument("--min-airport", choices=["small", "medium", "large"], default="medium",
                    help="smallest airport size to include (scheduled-service ones always kept)")
    ap.add_argument("--countries", default="",
                    help="restrict airports/navaids to ISO country codes (comma list, e.g. TW,JP)")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "map_data.h"))
    args = ap.parse_args()

    if abs(args.lat) > 85:
        sys.exit("latitudes beyond +/-85 are not supported")
    coslat = math.cos(math.radians(args.lat))
    if abs(args.lon) + args.radius * 1.3 / (KM_PER_DEG_LON * coslat) > 180:
        sys.exit("bounding box would cross the antimeridian - not supported")

    # bbox with 1.3x margin so the map still covers a later range increase
    dlat = args.radius * 1.3 / KM_PER_DEG_LAT
    dlon = args.radius * 1.3 / (KM_PER_DEG_LON * coslat)
    cache = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cache")
    countries = {c.strip().upper() for c in args.countries.split(",") if c.strip()}
    # ~1 px on the 456 px radar disc at full radius, floor 0.002 deg
    tol = args.tol or max(0.002, 2.0 * args.radius / 456.0 / 111.0)

    # ---- outline ----
    preserved = None
    lines, npts = [], 0
    if args.no_outline:
        preserved = read_existing_outline(args.out)
        src = "outline preserved from previous file"
    else:
        if args.geojson:
            files = args.geojson
        else:
            names = ["coastline", "borders"] + (["states"] if args.states else [])
            print("Fetching Natural Earth data (public domain):")
            files = [fetch(n, cache) for n in names]
        clipped = []
        for path in files:
            with open(path, encoding="utf-8") as f:
                gj = json.load(f)
            feats = gj["features"] if gj.get("type") == "FeatureCollection" else [gj]
            for ft in feats:
                geom = ft.get("geometry") or ft
                for pl in iter_polylines(geom):
                    clipped.extend(clip_polyline(pl, args.lat, args.lon, dlat, dlon))
        if not clipped:
            sys.exit("no map lines inside the bounding box - check --lat/--lon/--radius")
        while True:
            lines = build(clipped, args.lat, args.lon, tol, coslat)
            npts = sum(len(l) for l in lines)
            if npts <= args.max_points or not lines:
                break
            tol *= 1.5
        print(f"{len(lines)} polylines, {npts} points (tol {tol:.4f} deg)")
        src = ", ".join(os.path.basename(p) for p in files)

    # ---- ATC overlays ----
    airports, runways, fixes, airspaces = [], [], [], []
    if not args.no_airports:
        print("Fetching OurAirports data (public domain):")
        ap_rows = load_airports(fetch("airports", cache), args.lat, args.lon,
                                dlat, dlon, AIRPORT_RANK[args.min_airport + "_airport"],
                                countries)
        airports = [(a[0], a[1], a[2]) for a in ap_rows]
        idents = {a[3] for a in ap_rows}
        runways = load_runways(fetch("runways", cache), idents, args.rwy_ext, coslat)
    if not args.no_fixes or args.fixes_csv:
        seen = set()
        raw = load_fixes_csv(args.fixes_csv, args.lat, args.lon, dlat, dlon)
        if not args.no_fixes:
            raw += load_navaids(fetch("navaids", cache), args.lat, args.lon,
                                dlat, dlon, countries)
        for name, la, lo in raw:              # user CSV first, wins on duplicate ident
            if name not in seen:
                seen.add(name)
                fixes.append((name, la, lo))
        fixes.sort(key=lambda x: dist_km(x[1], x[2], args.lat, args.lon, coslat))
    feats = airspace_features_from_geojson(args.airspace_geojson)
    if args.openaip_key:
        feats += fetch_openaip_airspaces(args.openaip_key, args.lat, args.lon, dlat, dlon)
    if feats:
        allowed = {t.strip().upper() for t in args.airspace_types.split(",") if t.strip()}
        airspaces = build_airspaces(feats, args.lat, args.lon, dlat, dlon,
                                    tol, coslat, allowed)
    as_floats = sum(2 * len(a[2]) for a in airspaces)
    if as_floats > 60000:
        sys.exit(f"airspace data too large ({as_floats} floats > 60000): "
                 "raise --tol or trim --airspace-types")
    print(f"{len(airports)} airports, {len(runways)} runways, "
          f"{len(fixes)} fixes, {len(airspaces)} airspace polygons")

    # ---- write header ----
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("// Map data generated by tools/make_map.py - do not edit by hand\n")
        f.write(f"// outline source: {src}\n")
        f.write(f"// center {args.lat:.4f},{args.lon:.4f}  radius {args.radius:.0f} km"
                f"  tol {tol:.4f} deg\n")
        f.write("// overlays: OurAirports (public domain)"
                + (", openAIP (CC BY-NC)" if args.openaip_key else "")
                + (", local GeoJSON" if args.airspace_geojson else "") + "\n")
        f.write("// MAP_OUTLINE format: lat,lon pairs; NAN,NAN = polyline separator\n")
        f.write("#pragma once\n#include <math.h>\n#include <stdint.h>\n")
        if preserved:
            f.write(preserved + "\n")
        else:
            f.write("inline const float MAP_OUTLINE[] = {\n")
            for pl in lines:
                f.write("  " + "".join(f"{la:.4f}f,{lo:.4f}f," for la, lo in pl) + "\n")
                f.write("  NAN,NAN,\n")
            f.write("};\n")
        f.write("static const int MAP_OUTLINE_LEN = sizeof(MAP_OUTLINE)/sizeof(float);\n\n")

        f.write("// ---- ATC overlay: airports / runways(+centerline ext) / fixes / airspaces ----\n")
        f.write("struct MapAirport  { char icao[5]; float lat, lon; };\n")
        f.write("struct MapRunway   { float lat1, lon1, lat2, lon2;        // runway thresholds\n")
        f.write("                     float xlat1, xlon1, xlat2, xlon2; }; // extended centerline\n")
        f.write("struct MapFix      { char name[6]; float lat, lon; };\n")
        f.write("struct MapAirspace { const char *name;\n")
        f.write("                     uint8_t cls;          // 0=CTR 1=TMA 2=CTA 3=other\n")
        f.write("                     uint16_t off, npts; }; // AIRSPACE_PTS float offset / point pairs\n\n")

        if airports:
            f.write("inline const MapAirport AIRPORTS[] = {\n")
            for icao, la, lo in airports:
                f.write(f"  {{ {c_str(icao)}, {la:.4f}f, {lo:.4f}f }},\n")
            f.write("};\n")
        else:
            f.write("inline const MapAirport AIRPORTS[] = { { \"\", 0, 0 } };\n")
        f.write(f"static const int AIRPORTS_LEN = {len(airports)};\n\n")

        if runways:
            f.write("inline const MapRunway RUNWAYS[] = {\n")
            for r in runways:
                f.write("  { " + ", ".join(f"{v:.4f}f" for v in r) + " },\n")
            f.write("};\n")
        else:
            f.write("inline const MapRunway RUNWAYS[] = { { 0,0,0,0,0,0,0,0 } };\n")
        f.write(f"static const int RUNWAYS_LEN = {len(runways)};\n\n")

        if fixes:
            f.write("inline const MapFix FIXES[] = {\n")
            for name, la, lo in fixes:
                f.write(f"  {{ {c_str(name)}, {la:.4f}f, {lo:.4f}f }},\n")
            f.write("};\n")
        else:
            f.write("inline const MapFix FIXES[] = { { \"\", 0, 0 } };\n")
        f.write(f"static const int FIXES_LEN = {len(fixes)};\n\n")

        if airspaces:
            f.write("inline const float AIRSPACE_PTS[] = {  // lat,lon pairs\n")
            offs, off = [], 0
            for _, _, ring in airspaces:
                f.write("  " + "".join(f"{la:.4f}f,{lo:.4f}f," for la, lo in ring) + "\n")
                offs.append((off, len(ring)))
                off += 2 * len(ring)
            f.write("};\n")
            f.write("inline const MapAirspace AIRSPACES[] = {\n")
            for (name, cls, _), (o, n) in zip(airspaces, offs):
                f.write(f"  {{ {c_str(name)}, {cls}, {o}, {n} }},\n")
            f.write("};\n")
        else:
            f.write("inline const float AIRSPACE_PTS[] = { NAN, NAN };\n")
            f.write("inline const MapAirspace AIRSPACES[] = { { \"\", 3, 0, 0 } };\n")
        f.write(f"static const int AIRSPACES_LEN = {len(airspaces)};\n")
    print(f"wrote {os.path.abspath(args.out)}")


if __name__ == "__main__":
    main()
