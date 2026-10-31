#!/usr/bin/env python3
"""產生 tz_db.h:IANA 時區名稱 → POSIX TZ 字串對照表(時區 AUTO 模式用)。

Open-Meteo 以經緯度回傳 IANA 名稱(如 America/Chicago),ESP32 沒有 tzdata,
time.set_timezone() 只吃 POSIX 字串(如 CST6CDT,M3.2.0,M11.1.0)。
POSIX 字串直接取自系統 tzdata:每個 TZif 檔的最後一行就是它。

用法:  python3 tools/make_tz_db.py [zoneinfo 目錄,預設 /usr/share/zoneinfo]
"""
import os
import sys

ZI = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/zoneinfo"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tz_db.h")


def posix_of(name):
    with open(os.path.join(ZI, name), "rb") as f:
        tail = f.read().rstrip(b"\n").rsplit(b"\n", 1)[-1]
    return tail.decode("ascii")


def main():
    names = set()
    with open(os.path.join(ZI, "zone.tab")) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            names.add(line.split("\t")[2].strip())
    names.add("UTC")
    names.add("GMT")
    version = "?"
    try:
        with open(os.path.join(ZI, "tzdata.zi")) as f:
            version = f.readline().split()[-1]
    except OSError:
        pass

    rows = []
    for n in sorted(names):
        p = posix_of(n)
        if not p or "\"" in p or "\\" in p:
            sys.exit(f"bad POSIX string for {n}: {p!r}")
        rows.append((n, p))

    with open(OUT, "w") as f:
        f.write("// 自動產生 —— 勿手改。產生器:tools/make_tz_db.py"
                f"(tzdata {version},{len(rows)} 筆)\n")
        f.write("// IANA 時區名稱 → POSIX TZ 字串,供時區 AUTO 模式查表。\n")
        f.write("#pragma once\n#include <cstring>\n\n")
        f.write("namespace tzdb {\n\n")
        f.write("struct Entry { const char *name, *posix; };\n")
        f.write("static const Entry ENTRIES[] = {\n")
        for n, p in rows:
            f.write(f'  {{"{n}", "{p}"}},\n')
        f.write("};\n\n")
        f.write("// 查無此名回傳 nullptr(呼叫端改用 utc_offset_seconds 組固定偏移)\n")
        f.write("inline const char *lookup(const char *name) {\n")
        f.write("  for (const auto &e : ENTRIES)\n")
        f.write("    if (strcmp(e.name, name) == 0) return e.posix;\n")
        f.write("  return nullptr;\n}\n\n")
        f.write("}  // namespace tzdb\n")
    print(f"wrote {OUT}: {len(rows)} zones, tzdata {version}")


if __name__ == "__main__":
    main()
