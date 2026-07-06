#!/usr/bin/env python3
"""產生常用中文 glyph 清單,供 radar.yaml 的 font extras 使用。

- cjk_glyphs.yaml    = Big5 一級常用字(0xA440–0xC67E,5401 字)+ 常見全形標點 → Noto Sans TC
- cjk_glyphs_sc.yaml = GB2312 一級常用字(3755 字)扣掉已在 Big5 集內的 → Noto Sans SC
純 stdlib、離線可跑;輸出為單一 YAML 字串 scalar,radar.yaml 以 !include 引入。
用法:python3 tools/make_cjk_glyphs.py   (在 flight_radar/ 目錄下執行)
"""
import json
import os

BASE = os.path.join(os.path.dirname(__file__), "..")


def write_yaml(name, s):
    path = os.path.join(BASE, name)
    with open(path, "w", encoding="utf-8") as f:
        # JSON 字串是合法的 YAML scalar,雙引號格式避免特殊字元問題
        f.write(json.dumps(s, ensure_ascii=False) + "\n")
    print(f"wrote {len(s)} glyphs -> {os.path.abspath(path)}")


# 繁體:Big5 一級常用字
tc = []
for hi in range(0xA4, 0xC7):
    los = list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF))
    for lo in los:
        if hi == 0xC6 and lo > 0x7E:
            break
        try:
            tc.append(bytes([hi, lo]).decode("big5"))
        except UnicodeDecodeError:
            continue

punct = ",。、;:?!()「」『』—…‧"
write_yaml("cjk_glyphs.yaml", "".join(tc) + punct)

# 簡體:GB2312 一級字庫(區 16–55),只留 Big5 集沒有的字
tc_set = set(tc)
sc = []
for hi in range(0xB0, 0xD8):        # 區 16–55
    for lo in range(0xA1, 0xFF):
        try:
            c = bytes([hi, lo]).decode("gb2312")
        except UnicodeDecodeError:
            continue
        if c not in tc_set:
            sc.append(c)
write_yaml("cjk_glyphs_sc.yaml", "".join(sc))
