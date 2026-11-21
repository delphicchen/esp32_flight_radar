# 時鐘/感測器頁計畫(Issue #24)

> 需求:dumdumbee(Panda Touch)想要全螢幕時鐘頁,顯示日期、時間、網路與本地感測器的溫濕度,
> 並且要能定時關螢幕。他手上的感測器是 AHT20 + BME280,接 Panda 的 I2C 口。
> 擴充(2026-11-21 與擁有者確認):做成**通用感測器頁**,上方是時鐘和 ALARM,下方分格顯示
> 各種感測器數值和變化曲線(擁有者有 CO2 感測器),也可以接 HA 的感測器。
> 原則:**所有板子都要能用、不必設定**。沒接感測器的板子只顯示網路資料,不能因此報錯或卡住。
> 分階段做,每階段在 800×480 與 1024×600 都要編譯通過並上機確認。

## 設計決定
1. **入口**:點雷達頁的時鐘 → 時鐘/感測器頁;時鐘頁上放 **ALARM** 鈕進原本的鬧鐘頁。
2. **版面**(800×480,1024×600 用 scale_layout 產生):
   - 上方約 120 px:大字時間、日期與星期、ALARM / PWR / 返回雷達 按鈕。
   - 下方:**3×2 共 6 格**感測器卡片。每格有名稱與來源標籤、大字數值加單位、24 小時小曲線。
     少於 6 個數值時,卡片照順序排,剩下的空格隱藏。
3. **卡片自動排列,不必逐格設定**,順序如下:
   1. 網路:OUTDOOR 溫度、濕度(Open-Meteo,現有 `g_wx`)
   2. I2C 偵測到的數值(例:INDOOR 溫度/濕度/氣壓/CO2)
   3. 網頁上填的 HA 實體(最多 4 個;名稱和單位直接用 HA 的 `friendly_name`、`unit_of_measurement`)
   超過 6 格就截掉排在後面的。同一種量可以出現兩次(例:INDOOR 溫度和 OUTDOOR 溫度)。
   **格數與內容:預設自動,保留手動調整(2026-11-21 確認)**
   - 格數選項 AUTO / 1 / 2 / 3 / 4 / 6,排列依序是 1×1、2×1、3×1、2×2、3×2。
     AUTO = 依目前有效數值的個數(1~6)決定,數值變多變少時自動重排。存 NVS,設定頁與網頁都能改。
   - 每格可指定內容(網頁上每格一個下拉選單,選項是目前登記的數值),預設「自動」= 沿用上面的排序。
     手動指定的數值暫時沒資料時,那格顯示 `--`,不讓別的數值補上(避免格子內容跳來跳去)。
   - 卡片在 C++ 裡依螢幕寬度(800 或 1024)動態建立與重排,不寫死在 UI yaml 裡。
4. **曲線**:每個數值一個 ring buffer,每 1 分鐘取樣一次、存 1440 點(24 小時;2026-11-21 由 5 分鐘改快)。曲線時間窗隨資料長度伸縮(1~24 小時),放 PSRAM,只存在 RAM 裡(重開機就清空)。
   主迴圈只更新 buffer;**時鐘頁顯示時**才把資料推給 `lv_chart`,不在畫面上時不花渲染成本。
5. **不用 ESPHome 的 `sensor:` 元件讀 I2C**:那些是編譯時寫死的,裝置不存在時會一直報錯。
   改成開機時探測固定位址,在 C++ 裡自己讀,同一份韌體在有沒有接感測器的板子上都能跑。
   I2C 讀取放在主迴圈,用非阻塞狀態機(這次送出量測、下一輪才讀結果),**不放背景 task**。
   讀到的值另外用 template sensor 公開到網頁和 HA。
6. **HA 讀取走背景 task**:照 `request_speakers` 的 job type 模式加 `request_ha_state`,
   用 ready flag 讓主迴圈收結果。

## 現況(已讀碼確認)
- 時鐘 `clock_label`(`ui/ui_800x480.yaml:5035`)目前點下去是進鬧鐘頁 `alarm_page`。
- 網路天氣:`request_weather` → 背景抓 Open-Meteo → `g_wx`(temp/hum/wspd/wdir)→
  主迴圈收 `g_wx_ready`(`common/core.yaml:1298`)→ `show_weather` 填右下面板。
- PWR:`pwr_btn` 用 `id(backlight).turn_on/turn_off`;喚醒防誤觸靠 `wake_ms`。
- HA:已經有 `ha_url` / `ha_token`(`common/core.yaml:240-247`)和背景請求 `request_speakers`。
- 各板 I2C(觸控那條,400 kHz):
  | 板子 | SDA | SCL | 同一條匯流排上已有 | 外接 I2C 口 |
  |---|---|---|---|---|
  | Panda Touch | GPIO2 | GPIO1 | GT911 | **有,獨立的 I²C1:GPIO4/3** |
  | 白牌 800×480 | GPIO16 | GPIO15 | GT911 | 待查 |
  | JC8048W550 | GPIO19 | GPIO20 | GT911(= USB D-/D+)| 待查 |
  | Waveshare 5/7/7B | GPIO8 | GPIO9 | GT911 + CH422G / IO 擴充 | 板上 I2C 座接的就是這條 |
- Panda 外接口(PandaTouch_PlatformIO `docs/PINOUT.md`):P2 是 I²C1,腳序
  **SDA1(GPIO4)/ SCL1(GPIO3)/ VCC 3V3 / GND**,官方 README 還拿 AHT20 當範例。
  常見模組的腳序是 VCC/GND/SCL/SDA,這應該就是使用者說「要對調兩支腳」的原因。
- UI:只手改 `ui/ui_800x480.yaml`,`ui/ui_1024x600.yaml` 用 `tools/scale_layout.py ... 1.25` 產生。

## 支援的 I2C 感測器(第三階段)
| 位址 | 晶片 | 數值 | 備註 |
|---|---|---|---|
| 0x38 | AHT20 | 溫度、濕度 | **和 CH422G 撞位址**,見下方 |
| 0x44 | SHT3x / SHT4x | 溫度、濕度 | 用指令回應區分兩代 |
| 0x76 / 0x77 | BME280 / BMP280 | 溫度、(濕度)、氣壓 | chip id 0x60 / 0x58 |
| 0x62 | SCD40 / SCD41 | CO2、溫度、濕度 | 每 5 秒一筆的週期量測 |
| 0x61 | SCD30 | CO2、溫度、濕度 | 先不做,有人要再加 |

**SenseAir S8(擁有者的 CO2 感測器,2026-11-21 確認)是 UART(Modbus,9600 baud),不是 I2C**,
所以沒辦法自動偵測,只能做成選配的編譯期 package:
- 新增 `common/sensor_senseair_s8.yaml`:用 ESPHome 內建的 `uart:` + `sensor: platform: senseair`,
  TX/RX 腳位用 substitution 指定;`on_value` 時把數值登記進 hub(名稱 CO2、來源 `INDOOR · S8`、單位 ppm)。
- 需要的人在自己板子的 yaml 加 `packages:` 那一行再自己編譯。網頁安裝器的韌體不包含這個 package。
- 動工前要先查白牌 800×480(.228)有沒有空的 GPIO 能接 S8 的 TX/RX。
- CO2 卡片顏色分級:< 800 綠、800~1200 琥珀、> 1200 紅。
- 同一種量有多顆感測器:溫度/濕度優先用專用的溫濕度晶片(SHT/AHT),CO2 晶片的溫度不顯示,因為它會被自身發熱影響。
- **位址衝突**:CH422G 用 0x24/0x38/0x23/0x26,其中 **0x38 和 AHT20 撞位址**,
  而且對 0x38 寫入會改到 CH422G 的 IO 輸出,可能關掉背光或重置 LCD。
  → 用 board substitution 指定「感測器用哪條匯流排」和「要不要探測 0x38」。
  Panda 用獨立的外接匯流排,不受影響;Waveshare 那條匯流排上有 CH422G,**絕對不能探測 0x38**。
  7B 的 IO 擴充位址也要查。GT911 是 0x5D/0x14,不衝突。

## 第一階段:頁面骨架 + 網路資料(所有板子)
- [ ] 新增 `clock_page`:上方時鐘區,下方 6 格卡片(先用 `lv_obj` + label + `lv_chart`)。
- [ ] 入口:`clock_label` on_click 改成進 `clock_page`;時鐘頁的 ALARM 鈕接原本那段 on_click
      (`alarm_refresh` / `spk_dd_refresh` / `spk_scan`)。
- [ ] 時鐘頁的 PWR 鈕與雷達頁 `pwr_btn` 同步(共用同一段背光邏輯)。
- [ ] 通用數值模型(`sensors_hub.h`):`struct Reading { name, src, unit, value, valid, hist[288] }`,
      最多 6 筆,由「網路 → I2C → HA」依序登記。
- [ ] 每 5 分鐘取樣一次寫進 ring buffer;時鐘頁顯示時把資料推給 chart。
- [ ] 網路 OUTDOOR 溫度/濕度接上去;公制/英制沿用 `units_imperial`。

## 第二階段:定時關螢幕
- [ ] OFF 時間 / ON 時間 / 啟用開關,存 NVS;放在鬧鐘頁,網頁也提供對應欄位。
- [ ] 到 OFF 時間關背光,到 ON 時間開背光;中間觸控喚醒照現有行為。

## 第三階段:I2C 本地感測器(2026-11-21 實作,`sensors_i2c.h`)
- [x] 各板 I2C 匯流排加 `id: sens_i2c`;可探測 0x38 的板子加 build flag `-DSHUB_I2C_PROBE_38=1`
      (白牌、JC8048W550、Panda)。Panda 新增第二條匯流排 GPIO4/3 100 kHz,觸控那條改名 `touch_i2c`;
      原本佔用 GPIO4 的 vsync 佔位改成和 hsync 共用 GPIO42。
- [x] 開機 3 秒後探測一次;主迴圈每 100ms 讓到時間的裝置走一步(溫濕度/氣壓 30 秒、SCD4x 5 秒),驗 CRC。
      連續失敗 5 次記一次 log,改成每 60 秒重試;卡片超過 30 分鐘沒新值就顯示 `--`。
- [x] 偵測結果:網頁 text sensor `I2C Sensors`(例 `SHT4x@44 BME280@76`),卡片的來源標籤顯示晶片名。
      SYS 頁沒有空行,先不放。
- [x] 讀值用 template sensor 公開:Indoor Temperature / Humidity / Pressure / CO2。
- [ ] 上機驗證:白牌(.228)接 AHT20/BME280 實測;Panda 等使用者回報。

## 第四階段:HA 感測器
- [ ] 網頁加 `HA Sensor 1~4` 四個實體 ID 欄位(用現有的 `ha_url` / `ha_token`)。
- [ ] 背景 `request_ha_state`:`GET /api/states/<entity>`,取 state、unit、friendly_name;每 15 秒抓一次(2026-11-21 由 60 秒改快)。清單上限 400 筆、每頁 30 筆分頁。
- [ ] 非數字的 state(例:`unavailable`)顯示 `--`,不寫進曲線。

## 告警門檻與顏色(2026-11-21 實作)
- 齒輪 → ALERT ...:每個數值各自的上限/下限(−/+ 可按住連續調、OFF)與正常/超上限/低於下限三種顏色(6 色)。
- 跟著數值走、存 NVS(`shub_alert_v1`);溫度以 °C 存。HA 1~4 換實體時重設。
- 超標只改顏色:數值、曲線、卡片外框(加粗)。不發聲、不亮屏(擁有者決定)。
- CO2 預設上限 1200(紅),取代原本寫死的 800/1200 兩段。

## 文件(每階段做完都要更新)
- `docs/USAGE.md` 中英兩節、README(三種語言)、CHANGELOG。

## 範圍外
- lvgl9 分支(P4)要另外移植,等 main 穩定後再做。
- 曲線不寫進 flash(避免 flash 磨損);重開機後從空的開始。
