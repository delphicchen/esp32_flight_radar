#!/usr/bin/env bash
# 青岛流亭附近:轮廓 + 省界 + 机场/跑道/导航点(ATC 图层)
# 半径请 >= 实际最大扫描半径;过小会导致外围机场缺跑道数据。
python3 tools/make_map.py \
  --lat 36.36167 --lon 120.08750 --radius 300 --states \
  --min-airport medium --rwy-ext 15
