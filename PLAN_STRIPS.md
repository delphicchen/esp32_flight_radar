# 航班 Strip 列表計畫(Issue #22)

> 需求:lauraq68(ESP32-S3-Touch-7)希望右側顯示航班列表(機型、高度)。
> 設計已於 2026-11-07 與使用者確認。分兩階段做,每階段都要在兩種解析度上編譯+實機確認。

## 已確認的設計決定
1. **只用把手切換**:右欄上下分隔處放一個把手按鈕,點一下展開/收合。不做滑動手勢
   (右欄有滑桿與下拉選單,手勢容易被子元件吃掉)。
2. **展開後**:控制項全部收起,只留時鐘(+ wifi 圖示、底部 status_label),下方改顯示
   flight progress strips。
3. **雙向選取(照真實 ATC 的 strip–track correlation)**:
   - 點 strip = 沿用既有選機邏輯;該 strip 反白,並多展開一行航線(出發→目的)。
   - 在雷達上點飛機 → 對應 strip 反白並確保看得見。
   - 再點同一架或點雷達空白處 → 取消選取(與現行行為一致)。列表模式不會被打斷。
4. **第二階段:完整航跡**:選中的飛機畫出「進入畫面以來」的路線。
5. **兩種解析度都要正常運作**:800×480 與 1024×600。

## 現況(實作前已讀碼確認)
- `ui/ui_800x480.yaml` 是**唯一手改的來源**;`ui/ui_1024x600.yaml` 由
  `tools/scale_layout.py ... 1.25` 產生。目前重跑後只差 1 行手調(`sweep_btn y: 526`,腳本算出 525)。
  字級由各板子的 substitutions(`f_small`/`f_mono`/`f_mid`)決定,不在 UI 檔裡。
- 800×480 右欄幾何:x 490–792(約 300px 寬)
  - 時鐘 `clock_label` y16、`coords_label` y100、五顆按鈕 y154(高 32)
  - 選機區 `sel_cs_l`/`sel_type_l` y208–212、`sel_route_l` y248、`sel_l1~l5` 到約 y404
  - `brt_l`/`bl_slider` y412–420、`status_label` y446
  - 覆蓋層 `spec_panel`(y246)、`atc_panel`(y200)、`sys_tabbar`(y198),**顯隱一律由
    `atc_panel_sync`(core.yaml:705)決定**
- 選機:`select_slot` script(core.yaml:514)依觸控點找最近的資料索引 `i`,再點同一架則清掉
  `sel_cs_g` 並 `show_weather`。選取狀態以呼號 `sel_cs_g` 識別。
- 資料陣列:`ac_cs / ac_ty / ac_alt / ac_vel / ac_vr / ac_trk / ac_lat / ac_lon / ac_hex`,上限
  `AC_SLOTS = 40`(radar_fetch.h:45)。`ac_ty` 在 adsb.lol 來源有值(`a["t"]`);OpenSky 沒有,
  只有選中那架會向 adsbdb 補查。
- 航線:`fetch_route` → 背景 `request_route` → 主迴圈收 `g_route_ready`(core.yaml:1052)寫入
  `sel_route_l`。只查選中那一架。
- 尾跡:`trail_lat/trail_lon` 每個 slot 只存 3 點,換機就作廢(core.yaml:1126–1148)。
- 每秒更新在 `interval: 1s`(core.yaml:937)。

---

## 進度(接手時先看這裡;分支 `strips`)
- [x] 1. `select_slot` 抽出 `select_index`(純重構)— S3-7 編譯通過
- [x] 2–4. 把手、`list_apply`、`strips_update`、翻頁、雙向選取(程式已寫)
- [x] 2–4 S3-7 編譯通過
- [ ] 2–4 800×480 實機確認
- [ ] 5. 重新產生 1024×600,全部板子編譯
- [ ] 6. 移植 lvgl9
- [ ] 7. 第二階段:完整航跡
- [ ] 文件(README 中英、CHANGELOG)、截圖、發布

### 實作與原計畫的差異
- **每頁 6 條**(不是 7 條):選中那條要多展開一行(高 68),6 條才放得下
  (5×50 + 68 = 318 ≤ strip_box 高 320)。
- **翻頁鈕放在把手那一列**(◀ 頁碼 ▼把手 ▶,y96),不放 strip_box 右下角。
- **選機區不逐一隱藏**:`strip_box` 不透明、疊在上層,直接蓋住 `sel_*`;只隱藏
  `coords_label` 與五顆按鈕。這樣 `radar_type_badge`/`show_weather` 等原有程式改了 sel_* 也不會穿幫。
- 爬升/下降用 `^`/`v` 字元(`font_mono` 沒有 ↑↓ glyph),加上左側色條顏色區分。
- 地速固定用 kt、高度固定用 FL(航管慣例),只有距離跟著公制/英制切換。
- strip widget 由一次性腳本產生後貼進 `ui_800x480.yaml`(插在 `alarm_overlay` 之前,
  z 順序才會在 spec/atc/sys 覆蓋層之上)。

## 第一階段:把手 + Strip 列表 + 雙向選取

### 1. 新 global
- `list_on`(bool,`restore_value: true`,預設 false):是否為列表模式。比照 `atc_on` 的寫法。
- `list_page`(int,預設 0):目前翻到第幾頁。

### 2. UI(只改 `ui/ui_800x480.yaml`)
- **把手 `list_handle`**:小按鈕,收合時位於按鈕列與選機區之間(約 y188,置中,約 80×16);
  展開時移到時鐘正下方(約 y96)。圖示用 montserrat 內建的 `LV_SYMBOL_DOWN/UP`
  (免去補字型 glyph;wifi_icon 已在用 montserrat_20)。
- **Strip 容器 `strip_box`**:x490 y116 w302 h326(到 status_label 上方),預設 hidden。
- **7 條 strip `st0~st6`**(每條約 302×44,固定數量、預先建好,執行中不新增/刪除物件):
  - 左側 4px 色條:爬升/平飛/下降著色(沿用 ATC 尾跡的綠色系);STCA 衝突時改紅色。
  - 第 1 行(`font_mono`):`呼號   機型`
  - 第 2 行(`font_mono`):`FL350 ↑  450kt  12km`(依 `units_imperial` 切換單位)。
    箭頭沿用 `font_tiny` 已有的 DejaVu ↑↓ extras,或改用文字 `+`/`-`,實作時以實機為準。
  - 第 3 行(只有選中那條才顯示,strip 高度跟著加高):航線 `出發 → 目的`。
  - `clickable` + `on_click` → `select_index(i)`。
- **翻頁**:`strip_box` 右下角放 ▲▼ 兩顆小鈕(montserrat 符號),一頁 7 架。不做捲動清單
  (40 條實體物件太重,虛擬列表只更新文字就好)。
- 7 條的數量在 1024×600 上維持一樣:幾何由 scale_layout 乘 1.25、字級由 substitutions 放大,
  比例一致。

### 3. 切換邏輯(core.yaml)
- 新 script `list_apply`:依 `list_on` 顯示/隱藏
  - 隱藏:`coords_label`、五顆按鈕、`sel_*` 全部、`brt_l`、`bl_slider`
  - 顯示:`strip_box`;把手移位並換圖示
  - 覆蓋層:**擴充 `atc_panel_sync`**,`list_on` 為真時三個覆蓋層一律隱藏,維持「覆蓋層只在一處決定」的規則
- 開機時(on_boot 已有還原 dropdown 等的區塊)呼叫一次 `list_apply`。
- 每秒「工具列置頂」的迴圈(core.yaml:941 起)要把 `list_handle` 和 `strip_box` 一起置頂,
  避免被航班標記蓋住。
- 動畫:先做即時切換;實機手感 OK 後再考慮用 `lv_anim` 讓把手滑動(可選,不列入驗收)。

### 4. Strip 內容更新
- 新 helper `radar_strips_update(...)`(放 radar_fetch.h 或新的小 .h,沿用專案 helper 慣例),
  由 `interval: 1s` 在 `list_on` 時呼叫;收合時完全不跑。
- 排序:依與 home 的距離由近到遠(與現行選機距離公式相同)。每秒只對 ≤40 筆排序,成本可忽略。
- 選中那架**釘在第一條**(若不在目前頁)—— 這樣「雷達上點飛機 → strip 反白並看得見」
  不需要捲動邏輯。
- 只在文字有變時才 `lv_label_set_text`,減少重繪。
- 機型為空時顯示 `----`(OpenSky 來源)。

### 5. 選機重構
- 把 `select_slot` 的核心(從「已找到資料索引 i」開始到結尾)抽成新 script `select_index(int i)`;
  `select_slot` 改成「找 i → 呼叫 `select_index`」。行為完全不變。
- strip 的 on_click 直接呼叫 `select_index`,所以再點同一條 = 取消選取,與雷達一致。
- 航線:`g_route_ready` 收到結果時除了寫 `sel_route_l`,也存進新 global `sel_route_g`,
  給選中那條 strip 的第 3 行用。

### 6. 產生 1024×600 與移植
1. 重跑 `python3 tools/scale_layout.py ui/ui_800x480.yaml ui/ui_1024x600.yaml 1.25`
2. 把手調的 `sweep_btn y: 526` 補回去(或在腳本加例外,擇一)
3. main 完成後,比照 #21 掃描線的做法以 patch 移植到 `lvgl9`(兩分支 UI/core 差異很小);
   lvgl9 用 `envs/esphome-lvgl9` + `PLATFORMIO_CORE_DIR=~/.platformio-lvgl9`

### 7. 驗收
- [ ] 6 個 S3 板子設定 + P4 都編譯通過
- [ ] 800×480 實機:展開/收合、翻頁、點 strip 選取/取消、雷達點選 → strip 反白
- [ ] 1024×600 實機:同上,確認文字不截斷、7 條放得下、把手位置正確
- [ ] 收合後畫面與改動前完全相同(含 ATC/SYS/規格覆蓋層的顯隱)
- [ ] 公制/英制兩種單位顯示都正確
- [ ] README(中英兩節)與 CHANGELOG 更新,截圖補上

---

## 第二階段:選中飛機的完整航跡

### 1. 資料
- 新的航跡緩衝區,**以 ICAO hex 為 key**(不綁 slot,飛機暫時被擠出 40 架名單再回來也接得上):
  - 固定大小池,最多 40 條 × 128 點(lat/lon float,約 40KB),配置在 PSRAM。
  - 每次新資料進來(與現行 trail 推入同一個時機,core.yaml:1126 附近)就附加一點。
  - 點滿了就每隔一點丟一點(頭尾保留),早期的路線變稀疏但不會斷。
  - 超過 N 分鐘沒更新的條目回收給新飛機用。
- 現行 3 點尾跡**保留不動**,其他飛機照舊顯示。

### 2. 繪製
- 只替選中那架多畫一條 `lv_line`(預先建好一條,最多 128 點,點陣列放在 global,
  LVGL 8 的 `lv_line_set_points` 不會複製點陣列)。
- 存經緯度,每次重繪時用目前的中心點/範圍投影 → 改範圍或中心點也正確。
- 超出雷達圓的點裁掉(與現行尾跡的處理一致)。
- 顏色比現行尾跡淡,避免與向量線混淆。

### 3. 限制(要寫進 README)
- 只有進入畫面之後的路線;看不到起飛到進入畫面之間那段。
- 點距 = 抓取間隔(預設 30 秒),轉彎處會是折線。
- 重開機清空。

### 4. 驗收
- [ ] 兩種解析度實機:選中飛機後航跡正確、換範圍/中心點後仍對齊、取消選取後消失
- [ ] 長時間(>1 小時)跑下來 PSRAM 不持續增長
- [ ] 文件更新

---

## 使用者決定(2026-11-07 已確認)
1. 排序:依與 home 的距離由近到遠。
2. 重開機沿用最後狀態:`list_on` 用 `restore_value: true`。
3. 發布:兩階段都完成後一起發,第一階段完成時不單獨發 beta。
