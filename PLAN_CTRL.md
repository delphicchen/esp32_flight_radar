# PLAN:時鐘頁 SENSOR / CTRL 切換 —— HA 裝置控制(燈光、窗簾、空調、風扇)

起點:v1.4.1(S3)/ v1.4.4-p4(P4)。時鐘頁下方的卡片區可切換成「控制卡片」,
直接操作 Home Assistant 的 light / cover / climate / fan。

## 已定案的決定(2026-11-28)

- **切換方式:按鈕 + 滑動兩種都要。**
  - 兩顆互斥按鈕 `SENSOR` `CTRL`(選中的反白)。
  - 一般模式:放在數字時鐘正下方左側(原本日期的位置,y≈84~112,分隔線 y=116 之上)。
  - 大時鐘模式:時間置中後左上角是空的,兩顆按鈕移到左上(與 GRID 等按鈕同一列)。
  - 卡片區左右滑動也切換(LV_EVENT_GESTURE,只認卡片區;不能干擾三指截圖)。
  - 模式存 NVS,網頁同步一個 select *Clock Cards*(SENSOR / CTRL)。
- **第一版就做四類:燈光、窗簾、空調、風扇。**
- **格數沿用現有 GRID**(AUTO/1/2/3/4/6,大時鐘最多 3)。兩種模式共用同一個格數設定。
  CTRL 模式的 AUTO = 已設定的控制卡片數(至少 1)。

## 架構(沿用既有模式,不另起爐灶)

### 背景請求(radar_fetch.h,沿用 Job type)
- **type 11 = 控制實體清單**:`/api/template`,`states.light/cover/climate/fan`,
  每行 `entity_id|name|domain|state`,依名稱排序,上限 400(同 type 9 的分頁做法)。
- **type 12 = 控制實體狀態**:一次請求最多 6 個,每行
  `id|name|domain|state|brightness|current_position|current_temperature|temperature|hvac_mode|hvac_modes|percentage|min_temp|max_temp|target_temp_step|supported_features`。
  頁面顯示中每 5 秒一次;操作後 ~1 秒補一次。
- **type 13 = 呼叫服務**:`POST /api/services/<domain>/<service>`,body 由主迴圈組好(callsign 欄放 JSON)。
  - **不能被丟掉**:佇列深度只有 4,航班抓取會塞滿。控制用 `xQueueSendToFront`;
    仍失敗就留在主迴圈的 pending,下一輪(100ms)重送。
  - entity_id 一律驗證 `[a-z0-9_.]`(同 shub_ha_poll)。
- 結果照舊:ready flag + mutex,主迴圈輪詢收。網路永不進主迴圈。

### 狀態儲存
- 6 個 text 實體 `ctl_ent0..5`(網頁可直接改,同 ha_sens0..3),存 entity_id。
- 模式:global `shub_ctrl`(bool, restore)+ select。

### 畫面(sensors_hub.h 旁新增 ctrl_hub.h,兩分支共用,LVGL 8/9 用 #if)
- 控制卡片與感測卡片同一套格子位置(共用 layout 的 cols/rows/top);切換時隱藏另一組。
- 沒設定的格子顯示「+ ADD」,點了開選單:先選類型(LIGHT / COVER / CLIMATE / FAN)→ 分頁清單選實體。
- 每張卡片右上齒輪:換實體 / 進階控制。

| 類型 | 卡片上 | 齒輪 → 進階 | HA 服務 |
|---|---|---|---|
| 燈光 | 點卡片=開關;顯示亮度 % | 亮度滑桿(放開才送) | light.toggle / light.turn_on brightness_pct |
| 窗簾 | 開 / 停 / 關三鈕;位置 % | 位置滑桿 | cover.open_cover / stop_cover / close_cover / set_cover_position |
| 空調 | 室溫、設定溫度、− / +(停手 600ms 才送) | 模式:依 hvac_modes 列出 | climate.set_temperature / set_hvac_mode |
| 風扇 | 點卡片=開關;− / + 風速 %(停手才送) | 擺頭(supported_features 有才列) | fan.toggle / set_percentage / oscillate |

- **樂觀更新**:點下去卡片立刻變外觀並標「…」;回讀確認後轉正式,3 秒內沒確認或 HTTP 失敗 →
  恢復原狀並顯示錯誤(紅框 + 狀態碼,同 HA 清單失敗的顯示)。
- 不支援的功能不顯示(例:窗簾沒有 set_position 就不放滑桿;燈沒有亮度就只有開關)。
- 沒有 HA Token:CTRL 模式顯示「SET HA URL / TOKEN IN WEB UI」。
- 重繪:沿用 set_text / card_style 的「沒變就不 set」。

## 進度(2026-11-28)

第 1–4 階段完成並在白牌實測(v1.5.0)。和原計畫不同處:
- − / + 全部改成卡片上的滑桿(燈亮度、風扇風速、窗簾位置、冷氣溫度),0 = 關;燈的色溫 / 模式在齒輪。
- 冷氣模式直接放卡片下方按鈕列(OFF、COOL、DRY 優先),放不下的在齒輪 → MODE。
- 新版 HA 的 supported_features 是 IntFlag,模板 join 出 "1,2,4,8",要全部 OR 起來。
- 網頁診斷 *Control States* / *Control Last Call*。
- 窗簾(xiaomi)停下後 HA 回報位置 0:整合本身的問題,裝置端照實顯示。

## 分階段

1. **框架**:SENSOR / CTRL 按鈕(一般/大時鐘兩種位置)、滑動切換、模式存 NVS + 網頁 select、
   CTRL 模式空卡片「+ ADD」。白牌實測切換、滑動、大時鐘位置。
2. **選實體 + 讀狀態**:type 11 清單、選單(類型 → 分頁)、ctl_ent0..5、type 12 狀態輪詢、唯讀卡片。
3. **燈光 / 風扇 / 窗簾操作**:type 13(SendToFront + pending 重送)、樂觀更新與確認/回滾、
   亮度/位置/風速滑桿。
4. **空調**:設定溫度 − / +(依 min/max/step,去抖動)、模式選單。
5. **收尾**:7 片 S3 依序編譯、README 三語、USAGE 中英、CHANGELOG、發版;再移植 lvgl9(P4)。

## 待實測確認
- 滑動手勢會不會跟卡片上的按鈕/滑桿衝突(LVGL 的 GESTURE 在按鈕上按住拖動時的行為)。
- 一般模式時鐘下方的按鈕高度:800x480 只有約 28px 空間,1024x600 約 35px,夠不夠好按。
- HA 回應時間:本機 HA 的 template 請求與服務呼叫,實際延遲多少。
