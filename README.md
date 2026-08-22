# ✈️ ESP32 Flight Radar

A desktop flight-radar ornament built entirely with **ESPHome**. Live aircraft over your
location on an ATC-style scope — plus weather radar, a Home Assistant panel and an alarm clock.

以 **ESPHome** 打造的桌面航班雷達擺件。航管雷達風格顯示你上空的即時航班,兼具氣象雷達、
Home Assistant 面板與鬧鐘。

以 **ESPHome** 打造的桌面航班雷达摆件。航管雷达风格显示你上空的实时航班,兼具气象雷达、
Home Assistant 面板与闹钟。

**[English](#english) · [繁體中文](#繁體中文) · [简体中文](#简体中文)**

![ESP32 Flight Radar — radar sweep, ATC mode and the alarm page](docs/ATC_clock.gif)

▶ **[Demo video / 示範影片 / 示范影片](docs/demo.mp4)**

Inspired by [AnthonySturdy/micro-radar](https://github.com/AnthonySturdy/micro-radar).

---

## Boards / 支援板子 / 支持板子

| Entry | Board | Panel | Branch |
|-------|-------|-------|--------|
| `radar.yaml` | generic ESP32-S3 5" (esp32-s3-5inch-rgb-001) | 800×480 RGB | `main` |
| `radar-jc8048w550.yaml` | Guition JC8048W550**C** = Sunton ESP32-8048S050 *(alpha-test)* | 800×480 RGB | `main` |
| `radar-s3-5.yaml` | Waveshare ESP32-S3-Touch-LCD-5 | 800×480 RGB | `main` |
| `radar-s3-5b.yaml` | Waveshare ESP32-S3-Touch-LCD-5B | 1024×600 RGB | `main` |
| `radar-p4-7b.yaml` | Waveshare ESP32-P4-WIFI6-Touch-LCD-7B | 1024×600 MIPI-DSI | `lvgl9` |

All need **≥8 MB octal PSRAM**, a **GT911** touch controller and **16 MB flash**.

> **The map is downloaded now, and that needs one USB flash.** The firmware used to
> compile the map in, so a prebuilt image only had a useful map near Taiwan; it now
> fetches tiles for your own coordinates, which required a custom partition table — and
> **a partition table cannot be changed over OTA**. Coming from v1.2.0 or earlier, flash
> once over USB; OTA works normally again afterwards. To stay on the old behaviour, use
> the **[`pre-maptiles`](https://github.com/delphicchen/esp32_flight_radar/releases/tag/pre-maptiles)**
> tag (`git checkout pre-maptiles`) — that is the last commit with the baked-in map and
> the stock partition table.
>
> **地圖改成下載,升級需要接 USB 燒一次。** 以前地圖是編譯進韌體的,預編版只有台灣能用;
> 現在改成依你的座標抓圖磚,為此換了分割表,而**分割表無法用 OTA 更新**。從 v1.2.0 以前
> 升上來要接線燒一次,之後照樣能 OTA。想留在舊行為請用 `git checkout pre-maptiles`。

> **Two branches, not old and new.** `main` runs the ESP32-S3 boards on ESPHome 2026.3.3 /
> LVGL 8. `lvgl9` runs the ESP32-P4 on 2026.6.5 / LVGL 9.5. The P4 has no choice — its
> microSD support needs a component that will not load on the older ESPHome. The S3 boards
> stay on LVGL 8 because LVGL 9 measurably slowed their radar sweep down. Both are maintained.
>
> **兩個分支不是新舊關係,是各自服務不同板子** —— P4 沒得選(microSD 需要新版才載得起來的
> 元件),S3 留在 LVGL 8 是因為實測 LVGL 9 會讓掃描線變慢。兩邊都持續維護。
>
> **两个分支不是新旧关系,是各自服务不同板子** —— P4 没得选(microSD 需要新版才载得起来的
> 组件),S3 留在 LVGL 8 是因为实测 LVGL 9 会让扫描线变慢。两边都持续维护。

---

## English

### What it does

- **Live flight radar** — up to 40 aircraft from [OpenSky](https://opensky-network.org/),
  [airplanes.live](https://airplanes.live/) or [adsb.lol](https://adsb.lol/), with a rotating
  sweep and a glow as the beam passes each target. Switch source on screen; OpenSky failures
  fall back to the free APIs automatically.
- **Tap any aircraft** for origin → destination, squawk (red on 7500/7600/7700), altitude,
  speed, heading, vertical rate, distance and ICAO type.
- **Tap the type badge** for the airframe itself: a bright-yellow top-down silhouette drawn
  to scale by real wingspan, revealed under a scan line, plus manufacturer, model, wingspan,
  length, MTOW, cruise speed, registration, country of registry, engine series and operator.
  318 type designators, 107 drawings and 6004 operators, all compiled into the firmware —
  no lookup, no network, works offline (on the OpenSky source the type code itself is
  fetched once per selected aircraft, since OpenSky does not report it).
- **ATC mode** — target squares, 2-minute velocity vectors, fading history trails and a local
  conflict alert.
- **Weather echo** — rain radar from [RainViewer](https://www.rainviewer.com/), decoded on a
  background core so the UI never stutters.
- **Map outline** — coastlines, borders, airports, runways, navaids and airspace.
- **Alarm clock** — 4 alarms with per-weekday scheduling, ringing through a Home Assistant
  speaker or the board's own (P4).
- **Home Assistant** — auto-discovers; backlight, Wi-Fi signal and buttons become entities.
- **Screenshots** — three-finger swipe saves a BMP, downloadable over HTTP and, on the P4,
  written to microSD.
- **Set up entirely on the touch screen** — Wi-Fi via captive portal, everything else on the
  panel. Stored in NVS, survives reboots. **OTA after the first flash.**

### Install

**Easiest — flash from your browser** (Chrome or Edge, no toolchain):
**[open the installer page](https://delphicchen.github.io/esp32_flight_radar/install.html)**

**From source:**

```bash
git clone https://github.com/delphicchen/esp32_flight_radar
cd esp32_flight_radar
# S3 boards -> main branch:
pip install 'esphome==2026.3.*'
esphome run radar-s3-5b.yaml       # or radar.yaml / radar-s3-5.yaml / radar-jc8048w550.yaml
# P4 board -> lvgl9 branch:
git checkout lvgl9 && pip install 'esphome==2026.6.*'
# OpenSky 憑證(選用):專案根目錄放 credentials.json 後同步成 secrets.yaml
python3 tools/sync_secrets_from_credentials.py
ESPHOME_BUILD_PATH=build9 esphome run radar-p4-7b.yaml
```

First flash must be over **USB**. If it stalls: hold **BOOT**, tap **RESET**, release **BOOT**.

### First run

1. Connect to the **`Radar-Setup`** hotspot (password `12345678`) and pick your Wi-Fi.
2. Tap the coordinates line to set your latitude, longitude and scan range.
3. Aircraft appear within a minute. Toggle **MAP** / **ECHO** as you like.

No account or API key is needed unless you specifically choose the OpenSky source.

### More

- **[Usage guide](docs/USAGE.md)** — alarms, ATC mode, screenshots to Home Assistant, using it
  outside Taiwan
- **[Boards & internals](docs/BOARDS.md)** — per-board pins and timings, local component
  overrides, how to add a board
- **[Changelog](CHANGELOG.md)** — what changed per release, and what an upgrade costs you

---

## 繁體中文

### 它能做什麼

- **即時航班雷達** —— 從 [OpenSky](https://opensky-network.org/)、
  [airplanes.live](https://airplanes.live/) 或 [adsb.lol](https://adsb.lol/) 取得最多 40 架
  航班,掃描線轉到時目標會亮起。資料來源可在螢幕上切換,OpenSky 失敗會自動退回免金鑰的來源。
- **點選任一航班** 顯示起訖機場、squawk(7500/7600/7700 轉紅)、高度、速度、航向、
  升降率、距離與 ICAO 機型代碼。
- **點機型徽章** 看這台飛機本身:亮黃色俯視輪廓(依真實翼展等比縮放、以掃描線方式現形),
  加上製造商、機型全名、翼展、機身長度、最大起飛重量、巡航速度、註冊號、註冊國、
  發動機系列與營運者。318 個機型代碼、107 張輪廓、6004 家營運者全部編進韌體 ——
  不查詢、不連網,離線可用(OpenSky 不給機型代碼,選中的那一架會另外查一次)。
- **ATC 模式** —— 目標方塊、2 分鐘速度向量、漸淡歷史軌跡,以及本地衝突警示。
- **氣象回波** —— 來自 [RainViewer](https://www.rainviewer.com/) 的雨區雷達,在背景核心
  解碼合成,UI 完全不卡。
- **地圖輪廓** —— 海岸線、行政邊界、機場、跑道、導航點與空域。
- **鬧鐘** —— 4 組鬧鐘、可分別設定星期,透過 Home Assistant 喇叭或板載喇叭(P4)響鈴。
- **Home Assistant** —— 自動被探索;背光、Wi-Fi 訊號與按鈕都會成為實體。
- **截圖** —— 三指滑動存成 BMP,可經 HTTP 下載;P4 還會另存一份到 microSD。
- **全部在觸控螢幕上設定** —— Wi-Fi 走 captive portal,其餘都在面板上完成。設定存進 NVS、
  重開機不遺失。**首次燒錄之後就能 OTA 更新。**

### 安裝

**最簡單 —— 用瀏覽器燒錄**(Chrome 或 Edge,不必安裝任何工具):
**[開啟安裝頁面](https://delphicchen.github.io/esp32_flight_radar/install.html)**

**從原始碼:**

```bash
git clone https://github.com/delphicchen/esp32_flight_radar
cd esp32_flight_radar
# S3 板 → main 分支:
pip install 'esphome==2026.3.*'
esphome run radar-s3-5b.yaml       # 或 radar.yaml / radar-s3-5.yaml / radar-jc8048w550.yaml
# P4 板 → lvgl9 分支:
git checkout lvgl9 && pip install 'esphome==2026.6.*'
# OpenSky 憑證(選用):專案根目錄放 credentials.json 後同步成 secrets.yaml
python3 tools/sync_secrets_from_credentials.py
ESPHOME_BUILD_PATH=build9 esphome run radar-p4-7b.yaml
```

第一次必須用 **USB** 燒錄。卡住的話:按住 **BOOT**、點一下 **RESET**、放開 **BOOT**。

### 首次啟動

1. 連上 **`Radar-Setup`** 熱點(密碼 `12345678`),選擇你家的 Wi-Fi。
2. 點座標那一行,設定緯度、經度與掃描半徑。
3. 一分鐘內就會出現航班。**MAP** / **ECHO** 依喜好開關。

除非你指定要用 OpenSky 來源,否則不需要任何帳號或金鑰。

### 更多

- **[使用指南](docs/USAGE.md)** —— 鬧鐘、ATC 模式、截圖存到 Home Assistant、在台灣以外地區使用
- **[板子與內部細節](docs/BOARDS.md)** —— 逐板腳位與時序、本地元件覆寫、如何新增板子
- **[更新紀錄](CHANGELOG.md)** —— 每個版本改了什麼,以及升級要付出什麼代價

---

## 简体中文

### 它能做什么

- **实时航班雷达** —— 从 [OpenSky](https://opensky-network.org/)、
  [airplanes.live](https://airplanes.live/) 或 [adsb.lol](https://adsb.lol/) 获取最多 40 架
  航班,扫描线转到时目标会亮起。数据源可在屏幕上切换,OpenSky 失败会自动退回免密钥的来源。
- **点选任一航班** 显示起讫机场、squawk(7500/7600/7700 转红)、高度、速度、航向、
  升降率、距离与 ICAO 机型代码。
- **点机型徽章** 看这台飞机本身:亮黄色俯视轮廓(依真实翼展等比缩放、以扫描线方式现形),
  加上制造商、机型全名、翼展、机身长度、最大起飞重量、巡航速度、注册号、注册国、
  发动机系列与营运者。318 个机型代码、107 张轮廓、6004 家营运者全部编进固件 ——
  不查询、不连网,离线可用(OpenSky 不给机型代码,选中的那一架会另外查一次)。
- **ATC 模式** —— 目标方块、2 分钟速度向量、渐淡历史轨迹,以及本地冲突警示。
- **气象回波** —— 来自 [RainViewer](https://www.rainviewer.com/) 的雨区雷达,在后台核心
  解码合成,UI 完全不卡。
- **地图轮廓** —— 海岸线、行政边界、机场、跑道、导航点与空域。
- **闹钟** —— 4 组闹钟、可分别设定星期,通过 Home Assistant 音箱或板载喇叭(P4)响铃。
- **Home Assistant** —— 自动被发现;背光、Wi-Fi 信号与按钮都会成为实体。
- **截图** —— 三指滑动存成 BMP,可经 HTTP 下载;P4 还会另存一份到 microSD。
- **全部在触摸屏上设定** —— Wi-Fi 走 captive portal,其余都在面板上完成。设定存进 NVS、
  重启不丢失。**首次烧录之后就能 OTA 更新。**

### 安装

**最简单 —— 用浏览器烧录**(Chrome 或 Edge,不必安装任何工具):
**[打开安装页面](https://delphicchen.github.io/esp32_flight_radar/install.html)**

**从源码:**

```bash
git clone https://github.com/delphicchen/esp32_flight_radar
cd esp32_flight_radar
# S3 板 → main 分支:
pip install 'esphome==2026.3.*'
esphome run radar-s3-5b.yaml       # 或 radar.yaml / radar-s3-5.yaml / radar-jc8048w550.yaml
# P4 板 → lvgl9 分支:
git checkout lvgl9 && pip install 'esphome==2026.6.*'
# OpenSky 憑證(選用):專案根目錄放 credentials.json 後同步成 secrets.yaml
python3 tools/sync_secrets_from_credentials.py
ESPHOME_BUILD_PATH=build9 esphome run radar-p4-7b.yaml
```

第一次必须用 **USB** 烧录。卡住的话:按住 **BOOT**、点一下 **RESET**、放开 **BOOT**。

### 首次启动

1. 连上 **`Radar-Setup`** 热点(密码 `12345678`),选择你家的 Wi-Fi。
2. 点坐标那一行,设定纬度、经度与扫描半径。
3. 一分钟内就会出现航班。**MAP** / **ECHO** 依喜好开关。

除非你指定要用 OpenSky 数据源,否则不需要任何账号或密钥。

### 更多

- **[使用指南](docs/USAGE.md)** —— 闹钟、ATC 模式、截图存到 Home Assistant、在台湾以外地区使用
- **[板子与内部细节](docs/BOARDS.md)** —— 逐板引脚与时序、本地组件覆盖、如何新增板子
- **[更新记录](CHANGELOG.md)** —— 每个版本改了什么,以及升级要付出什么代价

---

## Data sources & credits / 資料來源與致謝 / 数据来源与致谢

- Aircraft states — [OpenSky Network](https://opensky-network.org/), [airplanes.live](https://airplanes.live/), [adsb.lol](https://adsb.lol/)
- Route and aircraft-type lookup — [adsbdb.com](https://www.adsbdb.com/)
- Aircraft silhouettes — [plane-watch/pw-silhouettes](https://github.com/plane-watch/pw-silhouettes) (CC BY-NC-SA 4.0); airframes it does not cover (the 747 family) are drawn from published dimensions by `tools/make_local_silhouettes.py`
- Type designators, operators, ICAO24 allocations — ICAO Doc 8643 / Doc 8585 / Annex 10 via [rikgale/ICAOList](https://github.com/rikgale/ICAOList)
- Aircraft performance — [openap](https://github.com/TUDelft-CNS-ATM/openap) (TU Delft)
- Weather radar — [RainViewer](https://www.rainviewer.com/)
- Local weather — [Open-Meteo](https://open-meteo.com/)
- Taiwan boundaries — [g0v/twgeojson](https://github.com/g0v/twgeojson)
- Map tiles — served from [flight-radar-maps](https://github.com/delphicchen/flight-radar-maps),
  generated by `tools/make_tiles.py` and downloaded by the device for its own coordinates
- World map data — [Natural Earth](https://www.naturalearthdata.com/) (public domain)
- Airports / runways / navaids — [OurAirports](https://ourairports.com/) (public domain)
- Taiwan airspace — [Taiwan CAA eAIP](https://ais.caa.gov.tw/) ENR 2.1
- Airspace elsewhere (optional) — [openAIP](https://www.openaip.net/) (CC BY-NC)
- microSD storage components (`lvgl9` branch) — [p1ngb4ck's ESPHome fork](https://github.com/p1ngb4ck/esphome)
- Concept — [AnthonySturdy/micro-radar](https://github.com/AnthonySturdy/micro-radar)
- Climb/descent arrow glyphs — [DejaVu Sans](https://dejavu-fonts.github.io/)

Please respect each provider's free-tier terms; this is a hobby build, not a service.
請遵守各來源的免費方案條款;這是業餘作品,不是服務。
请遵守各来源的免费方案条款;这是业余作品,不是服务。

## 🔗 Links / 友链

- 非常感谢 [LINUX DO](https://linux.do/latest) 社区提供的交流平台 / Many thanks to the LINUX DO community for the great discussion platform.

---

## 📄 License / 授權 / 授权

**Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)**

You are free to **use, share and adapt** this project for **non-commercial purposes**, as long as you give appropriate credit and license your derivatives under the same terms. **Commercial use is not permitted.** See [`LICENSE`](LICENSE).

你可以基於**非商業目的**自由**使用、分享與改作**本專案,前提是註明出處並以相同條款授權你的衍生作品。**不允許商業使用。** 詳見 [`LICENSE`](LICENSE)。

你可以基于**非商业目的**自由**使用、分享与改作**本项目,前提是注明出处并以相同条款授权你的衍生作品。**不允许商业使用。** 详见 [`LICENSE`](LICENSE)。

© 2026 delphicchen
