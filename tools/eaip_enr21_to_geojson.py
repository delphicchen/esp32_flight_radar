#!/usr/bin/env python3
"""Convert a Taiwan eAIP ENR 2.1 page (IDS AIRNAV format) into airspace GeoJSON.

Parses the lateral-limit descriptions of the ENR 2.1 table (FIR / TMAs /
Class C/D/E airport zones) and emits a FeatureCollection usable with
make_map.py --airspace-geojson. Handles the three geometry constructs used
in the text:

  1. plain coordinate lists         "242408N 1213002E - 242511N 1214332E - ..."
  2. circles                        "Circle, radius 7NM, centre 232716N 1202412E"
  3. arc segments inside a list     "... - Clockwise Arc (radius 10NM, centre X) - ..."

Circles become 36-sided polygons; arcs are interpolated every ~6 degrees.

Usage:
    python eaip_enr21_to_geojson.py RC-ENR21-en-GB.html taiwan_airspace.geojson
    python eaip_enr21_to_geojson.py "https://ais.caa.gov.tw/eaip/.../RC-ENR%202.1-en-GB.html" out.geojson

Feature properties: name (uppercased), type (FIR / TMA / CTR).
Class C/D/E airport zones are tagged type=CTR so make_map.py keeps them by
default; the FIR boundary is tagged FIR (add it with --airspace-types).
Pure standard library.
"""
import html
import json
import math
import re
import sys
import urllib.request

KM_PER_DEG_LAT = 110.574
KM_PER_DEG_LON = 111.320
NM_KM = 1.852

COORD = r"\d{6}(?:\.\d+)?N \d{7}(?:\.\d+)?E"
ARC = r"(?:Counter )?Clockwise Arc \(radius [\d.]+ ?NM, centre " + COORD + r"\)"
CIRCLE = r"Circle, radius [\d.]+ ?NM, centre " + COORD
GEOM = re.compile(
    "((?:" + COORD + "|" + CIRCLE + ")"
    "(?: ?- ?(?:" + COORD + "|" + ARC + "))*)")
NAME_BEFORE = re.compile(r"([A-Z][A-Za-z0-9'\- ]{2,60})\s*$")


def dms(tok):
    """'242408N 1213002E' -> (lat, lon) decimal degrees."""
    la, lo = tok.split()
    def cv(s, deg_digits):
        d = float(s[:deg_digits]); m = float(s[deg_digits:deg_digits + 2])
        sec = float(s[deg_digits + 2:-1])
        return d + m / 60 + sec / 3600
    return cv(la, 2), cv(lo, 3)


def arc_points(c, p1, p2, radius_km, clockwise, step_deg=6.0):
    """Interpolated points along an arc centre c from p1 to p2 (exclusive)."""
    coslat = math.cos(math.radians(c[0]))
    def xy(p):
        return ((p[1] - c[1]) * KM_PER_DEG_LON * coslat,
                (p[0] - c[0]) * KM_PER_DEG_LAT)
    x1, y1 = xy(p1); x2, y2 = xy(p2)
    a1, a2 = math.atan2(x1, y1), math.atan2(x2, y2)
    r1, r2 = math.hypot(x1, y1), math.hypot(x2, y2)
    if r1 < 0.05 or r2 < 0.05:          # degenerate: fall back to given radius
        r1 = r2 = radius_km
    sweep = (a2 - a1) % (2 * math.pi) if clockwise else -((a1 - a2) % (2 * math.pi))
    n = max(2, int(abs(sweep) / math.radians(step_deg)))
    out = []
    for i in range(1, n):
        t = i / n
        a = a1 + sweep * t
        r = r1 + (r2 - r1) * t          # blend radii to stay continuous
        x, y = r * math.sin(a), r * math.cos(a)
        out.append((c[0] + y / KM_PER_DEG_LAT,
                    c[1] + x / (KM_PER_DEG_LON * coslat)))
    return out


def circle_points(c, radius_km, n=36):
    coslat = math.cos(math.radians(c[0]))
    pts = []
    for i in range(n + 1):
        a = 2 * math.pi * i / n
        pts.append((c[0] + radius_km * math.cos(a) / KM_PER_DEG_LAT,
                    c[1] + radius_km * math.sin(a) / (KM_PER_DEG_LON * coslat)))
    return pts


def parse_geometry(g):
    """One geometry string -> [(lat, lon), ...] closed ring, or None."""
    m = re.match(r"Circle, radius ([\d.]+) ?NM, centre (" + COORD + ")", g)
    if m:
        return circle_points(dms(m.group(2)), float(m.group(1)) * NM_KM)
    toks = [t.strip() for t in g.split(" - ")]
    pts, pending_arc = [], None
    for t in toks:
        am = re.match(r"(Counter )?Clockwise Arc \(radius ([\d.]+) ?NM, centre (" + COORD + r")\)", t)
        if am:
            pending_arc = (am.group(1) is None, float(am.group(2)) * NM_KM, dms(am.group(3)))
            continue
        p = dms(t)
        if pending_arc and pts:
            cw, rkm, c = pending_arc
            pts.extend(arc_points(c, pts[-1], p, rkm, cw))
            pending_arc = None
        pts.append(p)
    if len(pts) < 4:
        return None
    if pts[0] != pts[-1]:
        pts.append(pts[0])
    return pts


def classify(name):
    if "FIR" in name:
        return "FIR"
    if "TMA" in name:
        return "TMA"
    return "CTR"                        # Class C/D/E airport zones etc.


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[-6].strip())
    src, out = sys.argv[1], sys.argv[2]
    if src.startswith("http"):
        req = urllib.request.Request(src, headers={"User-Agent": "Mozilla/5.0"})
        raw = urllib.request.urlopen(req, timeout=60).read().decode("utf-8", "replace")
    else:
        raw = open(src, encoding="utf-8", errors="replace").read()
    txt = re.sub(r"<[^>]+>", " ", raw)
    txt = re.sub(r"\s+", " ", html.unescape(txt))

    feats = []
    for m in GEOM.finditer(txt):
        ring = parse_geometry(m.group(1))
        if ring is None:
            continue
        nm = NAME_BEFORE.search(txt[:m.start()].rstrip())
        name = (nm.group(1).strip() if nm else "?")
        # ENR 2.1 lists the FIR first with only the bare name "Taipei" before it
        if name.upper() == "TAIPEI" and len(feats) == 0:
            name = "Taipei FIR"
        name = re.sub(r"\s*(Class [A-Z]|Airspace|airspace|Aerodrome traffic circuit)\s*", " ", name).strip()
        typ = classify(nm.group(1) if nm else name)
        feats.append({
            "type": "Feature",
            "properties": {"name": name.upper()[:31], "type": typ},
            "geometry": {"type": "Polygon",
                         "coordinates": [[[round(lo, 5), round(la, 5)] for la, lo in ring]]},
        })
    with open(out, "w", encoding="utf-8") as f:
        json.dump({"type": "FeatureCollection", "features": feats}, f, indent=1)
    for ft in feats:
        p = ft["properties"]
        print(f"  {p['type']:4} {p['name']:28} {len(ft['geometry']['coordinates'][0])} pts")
    print(f"wrote {out} ({len(feats)} airspaces)")


if __name__ == "__main__":
    main()
