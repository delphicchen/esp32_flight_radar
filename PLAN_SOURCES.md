# PLAN_SOURCES — 資料來源選擇 + fallback(airplanes.live / adsb.lol)

## 目標
- 設定頁可選資料來源:OpenSky(預設)/ airplanes.live / adsb.lol。
- OpenSky 失敗(token 失敗、401/429、非 200)時自動 fallback 到免費來源,10 分鐘後再回試。
- 免費來源有獨立的 polling interval(POLL2),因為它們免金鑰、無每日額度,可以抓得比 OpenSky 密。

## API 事實(兩個免費來源格式相同,readsb /v2 風格)
| | OpenSky(現況) | airplanes.live | adsb.lol |
|---|---|---|---|
| URL | states/all?lamin=… | `https://api.airplanes.live/v2/point/{lat}/{lon}/{r}` | `https://api.adsb.lol/v2/point/{lat}/{lon}/{r}` |
| 認證 | OAuth2 client credentials | 無 | 無 |
| 額度 | 4000 credits/日 | rate limit 約 1 req/s | 無明示,合理使用 |
| 半徑 | bbox(度) | **海里**,上限 250 nm(=463 km) | 同左 |
| 單位 | m、m/s | **英尺、節、ft/min** | 同左 |

回應 JSON:`{"ac":[{hex, flight, lat, lon, alt_baro, gs, track, baro_rate, seen, …}], "now": epoch_ms}`
- `alt_baro` 可能是字串 `"ground"` → 視同 on_ground 跳過。
- 換算:`alt = ft*0.3048`、`vel = kt*0.514444`、`vr = ft/min*0.00508`(內部維持 OpenSky 的公制單位,UI 全部不用動)。
- `lc = now/1000 - seen`(等價 OpenSky last_contact epoch 秒,ATC 模式黃色延遲判斷直接沿用)。
- 呼號 `flight` 尾端有空白,沿用現有 trim。
- 半徑:`range_km / 1.852`,clamp 250 nm(RANGE 最大 500 km 略超 463 km,超過就吃 clamp,估算列註明)。

## 設計
- global `data_src`(int 0/1/2,restore NVS)= 主要來源。
- number `poll_interval_alt`(5–300 s,預設 15,restore NVS)= 免費來源用的間隔;OpenSky 仍用原 `poll_interval`。
- **fallback**:`data_src==0` 且 OpenSky 該輪失敗 → 同輪直接改抓 airplanes.live,失敗再試 adsb.lol;
  並設 `g_os_cooldown_until`(10 分鐘),冷卻中節拍器改用 POLL2 間隔、bg task 直接抓免費來源,到期回試 OpenSky。
- `SET OPENSKY CREDENTIALS` 擋門只在 `data_src==0` 時生效;選免費來源完全不需要憑證。
- 「目前實際來源」存 `g_last_src`,SYS 面板顯示(OpenSky 額度列在非 OpenSky 時顯示來源名)。

## 改動清單(分階段)

### 階段 1 — radar_fetch.h(抓取引擎)
1. `Job` 加 `int src`;`request_states(..., int src)`。
2. 新增 `do_states_v2(const Job&, int src)`:組 URL(兩個 host 用表)、GET 無 bearer、
   解析 `ac[]` + 單位換算 + `now`→lc、`"ground"`/無座標跳過、同樣排序後發佈到 `g_result`。
3. `do_states` 依 src 分派;OpenSky 路徑不動,失敗時走 fallback 鏈 + 設冷卻。
4. `inline volatile uint32_t g_os_cooldown_until`、`inline volatile int g_last_src`。

### 階段 2 — radar.yaml(邏輯)
5. globals:`data_src`(restore);number:`poll_interval_alt`(5–300,預設 15)。
6. `fetch_flights` script 傳 `id(data_src)` 與對應憑證。
7. 1s 節拍器:憑證擋門加 `data_src==0` 條件;間隔取
   `data_src!=0 || 冷卻中 ? poll_interval_alt : poll_interval`。

### 階段 3 — radar.yaml(設定頁 UI)
8. SRC 三顆 radio 式 checkable button(OPENSKY / AIRPLANES / ADSB.LOL)——**不用 dropdown**
   (LVGL compound widget 的 id() 是包裝類指標,見既有教訓)。位置:右欄 poff_btn 上方或標題列右側。
9. `ta_poll2` textarea(POLL 列縮窄成兩格,或加一列);`kb_target==4`。
10. **grep `ta_poll` 全部同步改**(~25 處):13 顆鍵盤鍵的 ternary 鏈、4+1 處 border reset、
    頁面開啟預填(≈L2659)、SET 儲存(兩處:≈L3122 與 ≈L3425)、est 估算輸入檢查(≈L1028)。
11. `est_l`:src==0 照舊估 credits;否則顯示 "FREE API - NO DAILY QUOTA (MIN 5s)"。
12. SYS 面板 API 額度列:非 OpenSky 顯示來源名稱。

### 階段 4 — 文件
13. README **中英兩節**:features、setup(免費來源可跳過 OpenSky 註冊)、設定表(SRC、POLL2)、
    stale-data 說明、**credits 加 airplanes.live 與 adsb.lol**。

## 驗證
- `esphome compile radar.yaml`(長編譯放背景);裝置上切三個來源各抓一輪、拔憑證測 fallback。
