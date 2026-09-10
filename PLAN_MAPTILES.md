# PLAN_MAPTILES — 地圖改成開機下載,韌體不再烤地圖

## 目標
- 預編韌體對**全世界**都能用:非台灣使用者燒完就有海岸線、國界與機場,不必自己重編。
- 拿掉 `map_data.h` 這個編譯期產物,改成裝置依自己的座標/半徑抓圖磚存進 flash。
- 台灣不再是特例:縣市界 + eAIP 空域變成伺服器上的一個 detail pack,走同一條路徑。

## 已定案的決策(不要再重新討論)
1. **改分割表**,切一塊可寫的地圖分割區。代價是既有使用者要接 USB 重燒一次,已確認接受
   (大多數人本來就是插線安裝,重燒之後照樣能 OTA)。
2. **韌體不內建任何地圖**。不做「內建 vs 下載誰優先」的判斷邏輯,只有一條程式路徑。
3. **圖磚放另一個 repo**(`flight-radar-maps` + GitHub Pages),不放進本 repo ——
   每次重產都是幾 MB 的新 blob,不該進大家 clone 來編韌體的歷史。
4. **openAIP 不散布**(CC BY-NC)。空域只出我們自己轉的 detail pack;其他國家維持
   「自己帶金鑰重編」的舊路。其餘來源授權都可自由散布:
   Natural Earth(PD)、OurAirports(PD)、g0v/twgeojson(**CC0**,已查證)、
   台灣空域(我們自己從 CAA eAIP 轉的)。
5. 流量不是風險:一台裝置一輩子約幾百 KB,Pages 軟上限 100 GB/月 ≈ 87 萬次抓取;
   一顆 3.3 MB 韌體就抵 27 次地圖下載。真正的風險是**客戶端重試迴圈**,見下方防呆。

## 圖磚配置
```
https://delphicchen.github.io/flight-radar-maps/v1/L2/N50E010.bin
                                               ↑   ↑   ↑
                                         格式版本 細節層 10°x10° 格
```
- 格名 `N50E010` = 該格西南角,10 度對齊;`S`/`W` 表負值。
- **404 = 這格沒有資料(海上)**,當成空的,不算失敗、不重試。因此不需要索引檔。
- 細節層 L1/L2/L3 對應不同簡化容差,裝置照當前半徑挑(半徑越小挑越細)。
  容差沿用 `make_map.py` 的「約一個雷達像素」規則,以該層目標半徑計算。
- `v1/` 是格式版本;將來改格式走 `v2/`,舊韌體仍抓得到舊路徑。

## 二進位格式(v1,little-endian)
對應現有 `map_data.h` 的五個結構,讓 `radar_fetch.h` 的繪製迴圈盡量不用改:
```
header  magic "FRMT" | u16 ver | u16 flags | u32 crc32(payload)
        f32 cell_lat0, cell_lon0 | u8 level | u8 layers | u16 reserved
        u32 off/len x5  (outline, airports, runways, fixes, airspaces+strtab)
outline    f32 lat,lon 交錯;lat=NaN 為分隔,lon 帶 kind
           (0海岸/1國界/2州界/3縣市郡/4河流/5公路/6鐵路)
airports   char icao[5] + f32 lat,lon
runways    f32 lat1,lon1,lat2,lon2,xlat1,xlon1,xlat2,xlon2
fixes      char name[6] + f32 lat,lon
airspaces  u8 cls | u16 npts | u16 name_off(指字串表) | 之後接 npts 組 f32 lat,lon
```
`MapAirspace.name` 現在是 `const char *` 指向 rodata,改成指進解析後的字串表。

額外輪廓層(自建圖磚,官方 CDN 仍只有海岸/國界/州界+既有 detail pack):
- `--cities` 用 DataV 中國地級市界,kind 與 `--add-geojson FILE:3` 相同(縣/郡/市)。
  產生時會去掉與海岸/國界/省界重合的邊(共用邊只留一份),末端仍接到岸。
- `--add-geojson` 維持 `FILE[:KIND]`,KIND 0–3 使用中;4–6 留給之後的河/路/鐵 UI 開關。
- `make_tiles.py` 單張圖磚 ≥ 128 KB 會警告:韌體 `maps` 分割區只有 512 KB,500 km 可能同時載入約 4 格。

## 裝置端
- **分割表**:app0/app1 各 0x7C0000(7.75 MB)但韌體只用 3.23 MB。兩邊各縮到 0x500000(5 MB),
  空出的位置切 `maps, data, 0x82, , 0x80000`(512 KB;實測需求約 120 KB,留五倍餘裕)。
  用 `esp32: partitions: partitions.csv`(2026.3 已支援,`CONF_PARTITIONS`)。
- **不需要檔案系統**。單一 blob、整份重寫、循序讀取 —— `esp_partition_erase_range()` +
  `esp_partition_write()` 就夠,零依賴,每塊板都能用。LittleFS 對這個存取樣式沒有好處。
- **有 SD/USB 的板子**當第二層(存更大範圍/更細的圖),照 p1ngb4ck `storage` 元件的
  capability 介面寫,不要自己刻。**但那是後續**,基礎層必須所有板子都能用。
- **抓取時機**:第一次開機、以及座標/半徑變動到需要換格時。走既有的背景 job 模式
  (ready flag + mutex),絕不放主迴圈 —— 專案鐵律。
- **寫入防呆**:header 的 `flags` 先寫成「未完成」,全部寫完再回頭改成有效。開機讀到
  未完成就當作沒有地圖並重抓,避免斷線留下半份。
- **流量防呆**:分割區裡記下「這份是為哪個座標/半徑抓的」,沒變就不重抓;
  失敗走指數退避;404 不算失敗。
- **404 有 body,不能只看「有沒有拿到資料」**:實測 Pages 的 404 會回 9379 bytes 的
  HTML 錯誤頁。抓取端一定要先看 HTTP 狀態碼,再驗 magic `FRMT` 與 crc32,三關都過才寫入 ——
  只看長度的實作會把一頁 HTML 當成圖磚存進分割區。

## 分階段(依序做,每階段可獨立驗證)
1. ✅ **產生器** —— `tools/make_tiles.py`(import `make_map.py`,不重寫裁切/簡化)。
2. ✅ **圖磚 repo** —— https://delphicchen.github.io/flight-radar-maps/
   1203 格 / 6.8 MB(L1 1.41 / L2 2.04 / L3 3.36 MB),401 格有資料、其餘是海。
   線上檔與本地 sha256 一致,海上格確實 404,台灣兩格帶 detail pack。
3. **裝置端讀取**:分割表 + 解析器 + `radar_fetch.h` 改成讀解析後的指標(此時先用手動
   寫進分割區的圖磚測繪製,不含下載)。
4. **下載**:背景 job、防呆、UI 狀態顯示。
5. **收尾**:刪 `map_data.h` 與相關產生流程、更新 README/USAGE/BOARDS、致謝補上圖磚 repo。

## 待確認
- ✅ 全球總大小:**6.8 MB**(比估的 15–20 MB 小一半)。點狀圖層三層重複約 2.3 MB,
  已知、不值得為此把格式拆複雜。
- 半徑 500 km 時橫跨的格數上限(高緯度經度格較窄,可能超過 4 格)。
- `radar_fetch.h` 目前把 `MAP_OUTLINE_LEN` 等當編譯期常數用,改成變數後要確認
  底圖快取(`c_lat/c_lon/c_rng/c_map`)的失效條件要不要加上「地圖版本」。
