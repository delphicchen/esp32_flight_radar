#!/usr/bin/env python3
"""Generate the downloadable map tiles the firmware fetches at runtime.

Same data and same clipping/simplification code as make_map.py -- this only
changes *when* the clipping happens. make_map.py runs on your PC before a
compile and bakes one location into map_data.h; this runs once ahead of time
for the whole world and writes 10x10 degree binary tiles that the device
downloads for its own coordinates. See PLAN_MAPTILES.md.

Layout written under --out:

    v1/L2/N50E010.bin      cell named by its south-west corner, 10 deg aligned
                           S/W for negative values

Cells with no data are simply not written -- the firmware treats 404 as
"empty ocean here", so there is no index file to keep in sync.

Detail levels mirror make_map.py's "about one radar pixel" rule, evaluated at
the largest range each level is meant to serve:

    L1  range > 250 km   tol 0.0198 deg
    L2  range 100-250    tol 0.0079
    L3  range < 100      tol 0.0020

Sources (all redistributable, which is why these tiles can be hosted at all):
  Natural Earth 1:10m   coastline / country / state lines   public domain
  OurAirports           airports, runways, navaids          public domain
  --airspace-geojson    e.g. tools/taiwan_airspace.geojson  our own conversion
openAIP is deliberately NOT wired up here: CC BY-NC, fine for a user to fetch
with their own key in make_map.py, not something we redistribute.

Pure standard library. Reuses make_map.py as a module, so the two never drift.

Examples:
    # one cell, to eyeball the output
    python make_tiles.py --out /tmp/tiles --cells N20E120

    # Taiwan detail pack: county outline + eAIP airspace ON TOP of Natural Earth,
    # over the two cells Taiwan straddles (Kinmen is west of 120E).
    # The county file is 9 MB, so it lives in the gitignored cache; fetch it with:
    #   curl -Lo tools/cache/twcounty2010.geojson \
    #     https://raw.githubusercontent.com/g0v/twgeojson/master/json/twCounty2010.geo.json
    python make_tiles.py --out ../flight-radar-maps --cells N20E120,N20E110 \
        --add-geojson tools/cache/twcounty2010.geojson \
        --airspace-geojson tools/taiwan_airspace.geojson --min-airport small

    # everything, all levels
    python make_tiles.py --out ../flight-radar-maps
"""
import argparse
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_map as mm  # noqa: E402  (same directory, reused on purpose)

CELL_DEG = 10
MAGIC = b"FRMT"
FORMAT_VERSION = 1
FLAG_COMPLETE = 1

# level -> (tolerance in degrees, the max radar range it serves in km)
LEVELS = {
    1: (0.0198, 500),
    2: (0.0079, 250),
    3: (0.0020, 100),
}

LAYER_OUTLINE = 1
LAYER_AIRPORTS = 2
LAYER_RUNWAYS = 4
LAYER_FIXES = 8
LAYER_AIRSPACES = 16

HEADER_FMT = "<4sHHIffBBH"          # magic ver flags crc lat0 lon0 level layers pad
HEADER_SIZE = struct.calcsize(HEADER_FMT) + 5 * 8   # + 5 x (u32 off, u32 len)


def cell_name(lat0, lon0):
    """South-west corner -> 'N50E010' / 'S30W060'."""
    return "%s%02d%s%03d" % ("N" if lat0 >= 0 else "S", abs(lat0),
                             "E" if lon0 >= 0 else "W", abs(lon0))


def parse_cell(name):
    """'N50E010' -> (50, 10). Raises ValueError on anything malformed."""
    if len(name) != 7 or name[0] not in "NS" or name[3] not in "EW":
        raise ValueError("bad cell name %r (expected e.g. N50E010)" % name)
    lat = int(name[1:3]) * (1 if name[0] == "N" else -1)
    lon = int(name[4:7]) * (1 if name[3] == "E" else -1)
    if lat % CELL_DEG or lon % CELL_DEG:
        raise ValueError("cell %r is not on the %d degree grid" % (name, CELL_DEG))
    return lat, lon


def all_cells():
    """Every cell on the grid, south-west corners.

    Poles are skipped: the firmware's equirectangular projection is not valid
    there and make_map.py refuses |lat| > 85 for the same reason.
    """
    for lat in range(-80, 80, CELL_DEG):
        for lon in range(-180, 180, CELL_DEG):
            yield lat, lon


# ------------------------------------------------------------------ sections

def pack_outline(lines):
    """(kind, [(lat, lon), ...]) runs -> f32 pairs, NaN,kind as separator.

    Same convention as MAP_OUTLINE in map_data.h so the drawing loop in
    radar_fetch.h keeps working unchanged: a NaN latitude starts a new
    polyline and the longitude slot carries the brightness class.
    """
    out = bytearray()
    for kind, pts in lines:
        out += struct.pack("<ff", float("nan"), float(kind))
        for lat, lon in pts:
            out += struct.pack("<ff", lat, lon)
    return bytes(out)


def pack_airports(rows):
    """char icao[5] + f32 lat, lon."""
    out = bytearray()
    for icao, lat, lon in rows:
        out += struct.pack("<5sff", icao.encode()[:5], lat, lon)
    return bytes(out)


def pack_runways(rows):
    out = bytearray()
    for r in rows:
        out += struct.pack("<8f", *r)
    return bytes(out)


def pack_fixes(rows):
    """char name[6] + f32 lat, lon."""
    out = bytearray()
    for name, lat, lon in rows:
        out += struct.pack("<6sff", name.encode()[:6], lat, lon)
    return bytes(out)


def pack_airspaces(spaces):
    """count/strtab header, fixed-size records, string table, then the points.

    spaces: [(name, cls, [(lat, lon), ...]), ...]
    """
    strtab = bytearray()
    offsets = []
    for name, _cls, _pts in spaces:
        offsets.append(len(strtab))
        strtab += name.encode()[:63] + b"\0"
    recs = bytearray()
    pts_blob = bytearray()
    for (name, cls, pts), off in zip(spaces, offsets):
        recs += struct.pack("<BBHHH", cls, 0, off, len(pts), 0)
        for lat, lon in pts:
            pts_blob += struct.pack("<ff", lat, lon)
    return (struct.pack("<HH", len(spaces), len(strtab))
            + bytes(recs) + bytes(strtab) + bytes(pts_blob))


def build_tile(lat0, lon0, level, cache, args):
    """Clip every layer to one cell. Returns the tile bytes, or None if empty."""
    tol, _max_range = LEVELS[level]
    clat, clon = lat0 + CELL_DEG / 2.0, lon0 + CELL_DEG / 2.0
    dlat = dlon = CELL_DEG / 2.0
    # Segments with one endpoint inside the cell are kept whole (clip_polyline
    # does that), so lines join up across the seam without an overlap margin.
    coslat = math.cos(math.radians(clat)) or 1e-6

    layers = 0
    lines = []
    if not args.no_outline:
        if args.geojson:
            files = [(p, 0) for p in args.geojson]
        else:
            names = ["coastline", "borders"]
            if args.states:
                names.append("states")
            if args.cities:
                names.append("cities")
            if args.rivers:
                names.append("rivers")
            if args.roads:
                names.append("roads")
            if args.railroads:
                names.append("railroads")
            files = [(mm.fetch(n, cache), mm.OUTLINE_KIND[n]) for n in names]
        # A detail pack ADDS to Natural Earth, it does not replace it. One cell
        # is 10 degrees across and holds several countries -- swapping the whole
        # outline for a national boundary file would erase every neighbour's
        # coastline in that cell. (--geojson keeps make_map.py's replace
        # semantics, which is what you want for a single-location build.)
        # add-geojson 預設當市界(kind=3),例如縣/市界補充包。
        files += [(p, mm.OUTLINE_KIND["cities"]) for p in (args.add_geojson or [])]
        clipped = []
        for path, kind in files:
            for feat in mm.json.load(open(path, encoding="utf-8"))["features"]:
                if kind == mm.OUTLINE_KIND["roads"] and not mm.keep_road_feature(feat):
                    continue
                if kind == mm.OUTLINE_KIND["cities"] and not mm.keep_city_feature(feat):
                    continue
                for pl in mm.iter_polylines(feat.get("geometry") or {}):
                    clipped += [(kind, run)
                                for run in mm.clip_polyline(pl, clat, clon, dlat, dlon)]
        clipped = mm.retain_shared_city_segments(clipped)
        lines = mm.build(clipped, clat, clon, tol, coslat)
        refs = []
        if args.cities and not args.geojson:
            refs = mm.load_china_bound_refs(cache, clat, clon, dlat, dlon, tol, coslat)
        lines = mm.strip_city_border_overlaps(lines, coslat, extra_refs=refs)
        if lines:
            layers |= LAYER_OUTLINE

    airports = runways = fixes = []
    if not args.no_airports:
        ap_rows = mm.load_airports(mm.fetch("airports", cache), clat, clon,
                                   dlat, dlon,
                                   mm.AIRPORT_RANK[args.min_airport + "_airport"],
                                   set())
        airports = [(r[0], r[1], r[2]) for r in ap_rows]
        # runways.csv joins on OurAirports' `ident` (index 3), NOT the ICAO code
        # in index 0 -- they differ for plenty of fields.
        idents = {r[3] for r in ap_rows}
        runways = mm.load_runways(mm.fetch("runways", cache), idents,
                                  args.rwy_ext, coslat)
        if airports:
            layers |= LAYER_AIRPORTS
        if runways:
            layers |= LAYER_RUNWAYS
    if not args.no_fixes:
        fixes = mm.load_navaids(mm.fetch("navaids", cache), clat, clon,
                                dlat, dlon, set())
        if fixes:
            layers |= LAYER_FIXES

    spaces = []
    if args.airspace_geojson:
        feats = mm.airspace_features_from_geojson(args.airspace_geojson)
        allowed = {t.strip().upper() for t in args.airspace_types.split(",") if t.strip()}
        spaces = mm.build_airspaces(feats, clat, clon, dlat, dlon, tol, coslat, allowed)
        if spaces:
            layers |= LAYER_AIRSPACES

    if not layers:
        return None

    sections = [
        pack_outline(lines),
        pack_airports(airports),
        pack_runways(runways),
        pack_fixes(fixes),
        pack_airspaces(spaces),
    ]
    payload = bytearray()
    table = []
    for s in sections:
        table.append((len(payload), len(s)))
        payload += s
    crc = zlib.crc32(bytes(payload)) & 0xFFFFFFFF
    head = struct.pack(HEADER_FMT, MAGIC, FORMAT_VERSION, FLAG_COMPLETE, crc,
                       float(lat0), float(lon0), level, layers, 0)
    for off, ln in table:
        head += struct.pack("<II", off, ln)
    assert len(head) == HEADER_SIZE, (len(head), HEADER_SIZE)
    return head + bytes(payload)


def main():
    p = argparse.ArgumentParser(
        description="Generate downloadable 10x10 degree map tiles for the firmware.")
    p.add_argument("--out", required=True, help="output directory (tiles repo root)")
    p.add_argument("--cells", help="comma list of cells (e.g. N20E120); default: whole world")
    p.add_argument("--levels", default="1,2,3", help="detail levels to build (default all)")
    p.add_argument("--states", action="store_true", default=True,
                   help="include state/province borders (default on)")
    p.add_argument("--no-states", action="store_false", dest="states",
                   help="omit state/province borders")
    p.add_argument("--cities", action="store_true",
                   help="include admin_2 city/county borders (地級市界)")
    p.add_argument("--rivers", action="store_true", help="include river centerlines")
    p.add_argument("--roads", action="store_true", help="include major roads")
    p.add_argument("--railroads", action="store_true", help="include railroads")
    p.add_argument("--geojson", action="append",
                   help="local GeoJSON outline INSTEAD of Natural Earth")
    p.add_argument("--add-geojson", action="append",
                   help="local GeoJSON outline drawn IN ADDITION as city borders "
                        "(kind=3; e.g. county/city boundary pack)")
    p.add_argument("--airspace-geojson", action="append",
                   help="local GeoJSON with CTR/TMA polygons (name + type properties)")
    p.add_argument("--airspace-types", default="CTR,TMA,CTA",
                   help="airspace types to keep (comma list, default CTR,TMA,CTA)")
    p.add_argument("--min-airport", default="medium", choices=["small", "medium", "large"])
    p.add_argument("--rwy-ext", type=float, default=10.0)
    p.add_argument("--no-outline", action="store_true")
    p.add_argument("--no-airports", action="store_true")
    p.add_argument("--no-fixes", action="store_true")
    p.add_argument("--cache-dir", default=os.path.join(os.path.dirname(__file__), "cache"))
    args = p.parse_args()

    levels = [int(x) for x in args.levels.split(",") if x.strip()]
    for lv in levels:
        if lv not in LEVELS:
            p.error("unknown level %d (have %s)" % (lv, sorted(LEVELS)))

    if args.cells:
        cells = [parse_cell(c.strip().upper()) for c in args.cells.split(",") if c.strip()]
    else:
        cells = list(all_cells())

    os.makedirs(args.cache_dir, exist_ok=True)
    total = written = 0
    for lv in levels:
        d = os.path.join(args.out, "v%d" % FORMAT_VERSION, "L%d" % lv)
        os.makedirs(d, exist_ok=True)
        for lat0, lon0 in cells:
            blob = build_tile(lat0, lon0, lv, args.cache_dir, args)
            if blob is None:
                continue
            path = os.path.join(d, cell_name(lat0, lon0) + ".bin")
            with open(path, "wb") as f:
                f.write(blob)
            written += 1
            total += len(blob)
            print("L%d %s  %6d bytes" % (lv, cell_name(lat0, lon0), len(blob)))
    print("\n%d tiles, %.1f MB total" % (written, total / 1e6))


if __name__ == "__main__":
    main()
