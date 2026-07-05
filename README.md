# ✈️ ESP32 Flight Radar

A desktop flight-radar ornament for the **ESP32-S3 5" 800×480 RGB touch panel**, built entirely with **ESPHome**. It shows live aircraft over your location on an ATC-style radar scope, and doubles as a weather-radar display, a Home Assistant panel, and an alarm clock.

> 一款以 **ESPHome** 打造的桌面航班雷達擺件,執行於 **ESP32-S3 5 吋 800×480 RGB 觸控屏**。以航管雷達風格顯示你所在位置上空的即時航班,同時也是氣象雷達顯示器、Home Assistant 控制面板與鬧鐘。

Inspired by [AnthonySturdy/micro-radar](https://github.com/AnthonySturdy/micro-radar) — reimagined for a large landscape touch display with a much larger feature set.

---

## 📸 Screenshot / 畫面

![ESP32 Flight Radar](docs/screenshot.jpg)

## 🎬 Demo video / 示範影片

[![Watch the demo on YouTube](https://img.youtube.com/vi/3JHcLvoOxh8/maxresdefault.jpg)](https://youtu.be/3JHcLvoOxh8)

▶ **[Watch on YouTube / 在 YouTube 觀看](https://youtu.be/3JHcLvoOxh8)**


---

## English

### Features

- **Live flight radar** — pulls aircraft states from the [OpenSky Network](https://opensky-network.org/) around your coordinates and plots them on a 480×480 radar scope with a rotating sweep, fading trail, and target glow as the beam passes each aircraft.
- **ATC-style labels** — each aircraft shows its callsign, flight level and speed, with a heading-oriented plane icon. Tap any aircraft to see its **origin → destination** (via [adsbdb.com](https://www.adsbdb.com/)), altitude, speed, heading, vertical rate, distance and bearing.
- **Weather echo overlay** — optional rain-radar layer from [RainViewer](https://www.rainviewer.com/), downloaded, decoded and composited **entirely on a background core** so the UI never stutters. Toggle with an on-screen button.
- **Map outline overlay** — optional coastline / administrative border layer (Taiwan by default), toggle on screen.
- **Home Assistant integration** — the device auto-discovers in HA; backlight, Wi-Fi signal and buttons become HA entities.
- **Alarm clock** — up to 4 alarms, each with per-weekday scheduling. Alarms ring through a **Home Assistant media player** (any Wi-Fi speaker). On-screen **Snooze / Dismiss** overlay when ringing.
- **Fully on-device setup** — first boot opens a Wi-Fi captive portal. Coordinates, scan range, poll interval, OpenSky credentials and the alarm speaker can all be entered **on the touch screen** (or via the web page / Home Assistant). Everything is stored in NVS and survives reboots.
- **OTA updates** — after the first USB flash, all future updates are wireless.

### Hardware

| Part | Detail |
|------|--------|
| Board | ESP32-S3 5" RGB panel board (`esp32-s3-5inch-rgb-001`), 8 MB PSRAM (octal) + 16 MB flash |
| Panel | 800×480 IPS, ST7262 RGB driver |
| Touch | GT911 capacitive (I²C) |
| Power | USB-C |
| Enclosure | 3D-printable case ships with the board's SDK |

### Software requirements

- [ESPHome](https://esphome.io/) 2025.7 or newer (`pip install esphome`)
- The dependency `pngle` is pulled in automatically via `platformio_options`.

### Flashing

```bash
git clone https://github.com/delphicchen/esp32_flight_radar
cd esp32_flight_radar
esphome run radar.yaml
```

First flash must be over **USB** (`/dev/ttyUSB0` or `/dev/ttyACM0`; add yourself to the `dialout` group on Linux). If the upload stalls, hold **BOOT**, tap **RESET**, release **BOOT** to enter download mode. After that, `esphome run` updates over the air.

### First-time setup

1. On first boot the panel opens a Wi-Fi hotspot **`Radar-Setup`** (password `12345678`). Connect with your phone and pick your home Wi-Fi in the captive portal.
2. Register a **free OpenSky account**, then create an **API Client** in your account settings — this gives you a `client_id` and `client_secret` (OpenSky uses OAuth2, not your login password).
3. Tap the **Wi-Fi icon** (top right) or the **status line** to open the network / API page and enter your OpenSky credentials on screen. You can also fill them at `http://flight-radar.local` or in Home Assistant.
4. Tap the **coordinates line** to set your latitude / longitude and scan range on the numeric keypad.
5. Aircraft should appear within a minute. Toggle **MAP** / **ECHO** as you like.

### Alarm clock

- Tap the **clock** to open the alarm page (4 slots). For each: enable, set hour / minute, and choose the weekdays.
- In the config fields set **Alarm Speaker** (a Home Assistant `media_player` entity, e.g. `media_player.living_room`) and optionally **Alarm Sound URL** (an mp3).
- In Home Assistant, open the device page and enable **"Allow the device to perform Home Assistant actions."** Otherwise the ESP32 cannot command the speaker.
- When an alarm fires, a **SNOOZE 9m / DISMISS** panel appears on screen. The sound **re-plays every 15 s until you press DISMISS**, so a short mp3 still keeps ringing.

#### Using a Google Nest / Chromecast speaker

1. Add the **Google Cast** integration in Home Assistant (it auto-discovers Nest/Cast devices on your LAN) → your speaker becomes a `media_player` entity.
2. In **Developer Tools → States** find its id (e.g. `media_player.nest_mini`) and put it in **Alarm Speaker**.
3. Cast devices only play a **full, reachable URL**. Put an mp3 in Home Assistant's `config/www/` and use `http://<HA-IP>:8123/local/alarm.mp3` (use the IP, not `homeassistant.local`).
4. Test first in **Developer Tools → Actions**: `media_player.play_media` with your entity and URL. If the speaker rings, the alarm will too.

### Configuration reference

All of these are Home Assistant / web entities, stored in NVS:

| Setting | Meaning |
|---------|---------|
| OpenSky Client ID / Secret | OAuth2 API client credentials |
| Home Latitude / Longitude | Radar center (your location) |
| Radar Range | Scan radius in km (10–200) |
| Poll Interval | Seconds between fetches (default 30 → 2880/day, within the 4000/day quota) |
| Alarm Speaker | HA `media_player` entity to ring through |
| Alarm Sound URL | mp3 to play when an alarm fires |

### Using it outside Taiwan

`map_data.h` contains a simplified Taiwan county outline. To use your own region, take a boundary GeoJSON, simplify it (e.g. Douglas–Peucker) and emit a C array of `lat,lon` pairs separated by `NAN,NAN` in the same format. The radar projects it automatically for your center and range.

### Data sources & credits

- Aircraft states — [OpenSky Network](https://opensky-network.org/)
- Route lookup — [adsbdb.com](https://www.adsbdb.com/)
- Weather radar — [RainViewer](https://www.rainviewer.com/)
- Taiwan boundaries — [g0v/twgeojson](https://github.com/g0v/twgeojson)
- Concept — [AnthonySturdy/micro-radar](https://github.com/AnthonySturdy/micro-radar)

Please respect each provider's free-tier terms; this project is a hobby build, not a service.

---

## 中文

### 功能

- **即時航班雷達** — 從 [OpenSky Network](https://opensky-network.org/) 取得你座標周圍的航班,繪製在 480×480 雷達盤上,附旋轉掃描線、漸暗餘暉,以及掃描線掃過飛機時的高亮效果。
- **航管風格標籤** — 每架飛機顯示呼號、飛航高度層與速度,搭配依航向旋轉的飛機圖示。點選任一飛機可查看**起點 → 目的地**(透過 [adsbdb.com](https://www.adsbdb.com/))、高度、速度、航向、垂直速率、距離與方位。
- **氣象回波圖層** — 可選的降雨雷達層,資料來自 [RainViewer](https://www.rainviewer.com/);下載、解碼、合成**全部在背景核心完成**,主畫面完全不卡。以螢幕按鈕開關。
- **地圖輪廓圖層** — 可選的海岸線 / 行政區界(預設台灣),螢幕按鈕開關。
- **Home Assistant 整合** — 裝置會自動被 HA 探索;背光、Wi-Fi 訊號與按鈕都成為 HA 實體。
- **鬧鐘** — 最多 4 組,每組可設定特定星期幾。鬧鐘透過 **Home Assistant 的媒體播放器**(任何 Wi-Fi 喇叭)發聲。響鈴時螢幕出現**貪睡 / 關閉**面板。
- **完全在裝置上設定** — 首次開機開啟 Wi-Fi 設定熱點。座標、掃描半徑、輪詢間隔、OpenSky 憑證、鬧鐘喇叭都可以**直接在觸控螢幕上輸入**(也可透過網頁 / Home Assistant)。全部存於 NVS,重開機保留。
- **OTA 無線更新** — 第一次用 USB 燒錄後,之後都能無線更新。

### 硬體

| 零件 | 說明 |
|------|------|
| 主板 | ESP32-S3 5 吋 RGB 屏方案板(`esp32-s3-5inch-rgb-001`),8 MB PSRAM(octal)+ 16 MB flash |
| 面板 | 800×480 IPS,ST7262 RGB 驅動 |
| 觸控 | GT911 電容式(I²C) |
| 供電 | USB-C |
| 外殼 | 方案板 SDK 附 3D 列印外殼檔 |

### 軟體需求

- [ESPHome](https://esphome.io/) 2025.7 以上(`pip install esphome`)
- 相依的 `pngle` 會由 `platformio_options` 自動安裝。

### 燒錄

```bash
git clone https://github.com/delphicchen/esp32_flight_radar
cd esp32_flight_radar
esphome run radar.yaml
```

第一次必須用 **USB** 燒錄(`/dev/ttyUSB0` 或 `/dev/ttyACM0`;Linux 上把自己加入 `dialout` 群組)。若燒錄卡住,按住 **BOOT**、點一下 **RESET**、放開 **BOOT** 進入下載模式。之後 `esphome run` 就能走 OTA 無線更新。

### 首次設定

1. 首次開機面板會開啟 Wi-Fi 熱點 **`Radar-Setup`**(密碼 `12345678`)。用手機連上,在跳出的設定頁選擇你家的 Wi-Fi。
2. 註冊**免費的 OpenSky 帳號**,到帳號設定裡建立一個 **API Client**,取得 `client_id` 與 `client_secret`(OpenSky 使用 OAuth2,不是用你的登入密碼)。
3. 點螢幕右上角的 **Wi-Fi 圖示**或**底部狀態列**開啟網路 / API 設定頁,在螢幕上輸入 OpenSky 憑證。也可以在 `http://flight-radar.local` 或 Home Assistant 填寫。
4. 點**座標列**用數字鍵盤設定你的經緯度與掃描半徑。
5. 約一分鐘內飛機就會出現。依喜好切換 **MAP** / **ECHO**。

### 鬧鐘

- 點**時鐘**開啟鬧鐘頁(4 組)。每組:啟用、設定時 / 分、選擇星期幾。
- 在設定欄位填入 **Alarm Speaker**(Home Assistant 的 `media_player` 實體,例如 `media_player.living_room`),以及可選的 **Alarm Sound URL**(mp3)。
- 在 Home Assistant 的裝置頁開啟「**允許此裝置執行 Home Assistant 動作**」,否則 ESP32 無法命令喇叭。
- 鬧鐘響時,螢幕會出現 **SNOOZE 9m / DISMISS** 面板。聲音會**每 15 秒重播一次,直到你按下 DISMISS**,所以短音檔也能持續響。

#### 使用 Google Nest / Chromecast 喇叭

1. 在 Home Assistant 新增 **Google Cast** 整合(會自動發現區網內的 Nest / Cast 裝置)→ 喇叭變成一個 `media_player` 實體。
2. 到 **開發者工具 → 狀態** 找出它的 id(例如 `media_player.nest_mini`),填進 **Alarm Speaker**。
3. Cast 裝置只吃**完整、連得到的 URL**。把 mp3 放到 HA 的 `config/www/`,網址用 `http://<HA的IP>:8123/local/alarm.mp3`(用 IP,不要用 `homeassistant.local`)。
4. 先在 **開發者工具 → 動作** 用 `media_player.play_media` 帶入你的實體與網址測試;喇叭有響,鬧鐘就會響。

### 設定項一覽

以下皆為 Home Assistant / 網頁實體,存於 NVS:

| 設定 | 意義 |
|------|------|
| OpenSky Client ID / Secret | OAuth2 API 憑證 |
| Home Latitude / Longitude | 雷達中心(你的位置) |
| Radar Range | 掃描半徑(公里,10–200) |
| Poll Interval | 抓取間隔秒數(預設 30 → 每日 2880 次,在 4000 次/日額度內) |
| Alarm Speaker | 用來發聲的 HA `media_player` 實體 |
| Alarm Sound URL | 鬧鐘響時播放的 mp3 |

### 在台灣以外地區使用

`map_data.h` 內含簡化過的台灣縣市輪廓。若要換成你的地區,取一份邊界 GeoJSON,做簡化(如 Douglas–Peucker),以相同格式輸出 `lat,lon` 座標對、以 `NAN,NAN` 分隔折線的 C 陣列即可。雷達會依你的中心與半徑自動投影。

### 資料來源與致謝

- 航班狀態 — [OpenSky Network](https://opensky-network.org/)
- 航線查詢 — [adsbdb.com](https://www.adsbdb.com/)
- 氣象雷達 — [RainViewer](https://www.rainviewer.com/)
- 台灣界線 — [g0v/twgeojson](https://github.com/g0v/twgeojson)
- 概念啟發 — [AnthonySturdy/micro-radar](https://github.com/AnthonySturdy/micro-radar)

請遵守各資料來源的免費方案條款;本專案是自用興趣作品,並非商業服務。

---

## 📄 License / 授權

**Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)**

You are free to **use, share and adapt** this project for **non-commercial purposes**, as long as you give appropriate credit and license your derivatives under the same terms. **Commercial use is not permitted.** See [`LICENSE`](LICENSE).

你可以基於**非商業目的**自由**使用、分享與改作**本專案,前提是註明出處並以相同條款授權你的衍生作品。**不允許商業使用。** 詳見 [`LICENSE`](LICENSE)。

© 2026 delphicchen
