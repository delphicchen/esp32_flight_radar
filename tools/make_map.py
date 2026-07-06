#!/usr/bin/env python3
"""Generate map_data.h (radar map outline) for any location on Earth.

Downloads Natural Earth 1:10m public-domain vector data (coastline + country
borders, optionally state/province borders), clips it to a bounding box around
your home coordinates, simplifies it with Douglas-Peucker, and writes a
map_data.h in the exact format the firmware expects.

Examples:
    python make_map.py --lat 35.6762 --lon 139.6503 --radius 150   # Tokyo
    python make_map.py --lat 51.5074 --lon -0.1278 --radius 300 --states
    python make_map.py --lat 25.03 --lon 121.56 --radius 200 --geojson my.geojson

Pure standard library - no third-party packages needed.
Note: home locations within ~5 deg of the antimeridian (lon +/-180) are not
handled (neither by the firmware's equirectangular projection).
"""
import argparse
import json
import math
import os
import sys
import urllib.request

NE_BASE = "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/"
SOURCES = {
    "coastline": "ne_10m_coastline.geojson",
    "borders": "ne_10m_admin_0_boundary_lines_land.geojson",
    "states": "ne_10m_admin_1_states_provinces_lines.geojson",
}
KM_PER_DEG_LAT = 110.574
KM_PER_DEG_LON = 111.320  # at equator; scaled by cos(lat)


def fetch(name, cache_dir):
    """Download a Natural Earth file into cache_dir unless already cached."""
    path = os.path.join(cache_dir, SOURCES[name])
    if os.path.exists(path) and os.path.getsize(path) > 0:
        print(f"  cache hit: {path}")
        return path
    os.makedirs(cache_dir, exist_ok=True)
    url = NE_BASE + SOURCES[name]
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


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--lat", type=float, required=True, help="home latitude")
    ap.add_argument("--lon", type=float, required=True, help="home longitude")
    ap.add_argument("--radius", type=float, required=True, help="max radar range you plan to use, km")
    ap.add_argument("--states", action="store_true", help="also include state/province borders")
    ap.add_argument("--geojson", action="append", default=[],
                    help="use local GeoJSON file(s) instead of downloading Natural Earth")
    ap.add_argument("--tol", type=float, default=0.0,
                    help="Douglas-Peucker tolerance in degrees (default: ~1 radar pixel)")
    ap.add_argument("--max-points", type=int, default=4000,
                    help="point budget; tolerance is raised automatically if exceeded")
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

    if args.geojson:
        files = args.geojson
    else:
        cache = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cache")
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

    # ~1 px on the 456 px radar disc at full radius, floor 0.002 deg
    tol = args.tol or max(0.002, 2.0 * args.radius / 456.0 / 111.0)
    while True:
        lines = build(clipped, args.lat, args.lon, tol, coslat)
        npts = sum(len(l) for l in lines)
        if npts <= args.max_points or not lines:
            break
        tol *= 1.5
    print(f"{len(lines)} polylines, {npts} points (tol {tol:.4f} deg)")

    src = ", ".join(os.path.basename(p) for p in files)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(f"// Map outline generated by tools/make_map.py - do not edit by hand\n")
        f.write(f"// source: {src}\n")
        f.write(f"// center {args.lat:.4f},{args.lon:.4f}  radius {args.radius:.0f} km"
                f"  tol {tol:.4f} deg  {npts} points\n")
        f.write("// format: lat,lon pairs; NAN,NAN = polyline separator\n")
        f.write("#pragma once\n#include <math.h>\n")
        f.write("inline const float MAP_OUTLINE[] = {\n")
        for pl in lines:
            f.write("  " + "".join(f"{la:.4f}f,{lo:.4f}f," for la, lo in pl) + "\n")
            f.write("  NAN,NAN,\n")
        f.write("};\n")
        f.write("static const int MAP_OUTLINE_LEN = sizeof(MAP_OUTLINE)/sizeof(float);\n")
    print(f"wrote {os.path.abspath(args.out)}")


if __name__ == "__main__":
    main()
