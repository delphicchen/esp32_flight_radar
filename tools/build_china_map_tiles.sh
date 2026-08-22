#!/usr/bin/env bash
# Build China map tiles: coastline + country + province + city borders.
# Rivers/roads/rail make the scope too busy at 150–300 km ranges.
#
# Usage (from repo root, WSL/Linux):
#   ./tools/build_china_map_tiles.sh
#   ./tools/build_china_map_tiles.sh /path/to/flight-radar-maps
#
# Then host on GitHub Pages and set:
#   substitutions:
#     maps_base_url: "https://<you>.github.io/flight-radar-maps/v1"
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/../flight-radar-maps}"
# Qingdao / Shandong / Bohai / Jiangsu-ish cells for ~300 km around 36N 120E
CELLS="N30E110,N30E120,N40E110,N40E120"

mkdir -p "$OUT"
python3 "$ROOT/tools/make_tiles.py" \
  --out "$OUT" \
  --cells "$CELLS" \
  --levels 1,2,3 \
  --states \
  --cities \
  --min-airport medium

echo
echo "Tiles written under $OUT/v1/ (coast + country + province + city)"
echo "Push that repo to GitHub Pages, then set maps_base_url to its /v1 URL."
