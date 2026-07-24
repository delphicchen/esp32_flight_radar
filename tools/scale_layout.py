#!/usr/bin/env python3
"""Generate a scaled LVGL layout package from another resolution.

The Flight Radar UI (ui/ui_*.yaml) is laid out with absolute LVGL
coordinates. ESPHome substitutions cannot do arithmetic, so a second
resolution is produced by scaling the geometry of the base layout here.
The 800x480 file stays the single source of truth; re-run this whenever
it changes.

    python3 tools/scale_layout.py ui/ui_800x480.yaml ui/ui_1024x600.yaml 1.25

Only pixel-geometry keys are scaled (positions, sizes, paddings, line
widths, radii and `points` coordinate lists). Colors, opacities, angles,
slider/arc value ranges, font ids, text and enums are left untouched.
The result is a *starting point* — fine-tune on real hardware.
"""
import re
import sys
from math import floor

# Keys whose integer value is a pixel measure and should be scaled.
# Longer names first so `width` does not shadow `border_width` etc.
SCALE_KEYS = [
    "border_width", "line_width", "pad_all", "pad_top", "pad_bottom",
    "pad_left", "pad_right", "pad_row", "pad_column",
    "width", "height", "radius", "x", "y",
]
KEY_RE = re.compile(
    r"(?<![A-Za-z0-9_])(" + "|".join(SCALE_KEYS) + r")(\s*:\s*)(-?\d+)(?![\d.%A-Za-z])"
)
POINTS_RE = re.compile(r"(points\s*:\s*\[)([^\]]*)(\])")
INT_RE = re.compile(r"-?\d+")


def rnd(v: float) -> int:
    return floor(v + 0.5) if v >= 0 else -floor(-v + 0.5)


def scale_ints(text: str, factor: float) -> str:
    return INT_RE.sub(lambda m: str(rnd(int(m.group(0)) * factor)), text)


def scale_line(line: str, factor: float) -> str:
    # Do not touch comment-only lines.
    stripped = line.lstrip()
    if stripped.startswith("#"):
        return line
    # Split off a trailing inline comment so we never rewrite its contents.
    code, sep, comment = line, "", ""
    hash_idx = line.find("#")
    if hash_idx != -1 and '"' not in line[:hash_idx] and "'" not in line[:hash_idx]:
        code, sep, comment = line[:hash_idx], "#", line[hash_idx + 1:]

    code = POINTS_RE.sub(
        lambda m: m.group(1) + scale_ints(m.group(2), factor) + m.group(3), code
    )
    code = KEY_RE.sub(
        lambda m: m.group(1) + m.group(2) + str(rnd(int(m.group(3)) * factor)), code
    )
    return code + sep + comment


def main() -> None:
    src, dst, factor = sys.argv[1], sys.argv[2], float(sys.argv[3])
    lines = open(src).read().split("\n")
    out = [scale_line(l, factor) for l in lines]
    banner = [
        f"# AUTO-GENERATED from {src} by tools/scale_layout.py (x{factor}).",
        "# Do not edit by hand — edit the source layout and re-run the script.",
        "# Pixel geometry is scaled; tune on hardware if elements misalign.",
        "",
    ]
    open(dst, "w").write("\n".join(banner + out))
    print(f"wrote {dst} ({len(out)} lines, x{factor})")


if __name__ == "__main__":
    main()
