// 背景航班抓取引擎:跑在 core 1 的 FreeRTOS task,主迴圈(UI)完全不阻塞。
// 主迴圈用 request_states()/request_route() 丟工作,
// 每秒輪詢 g_states_ready / g_route_ready 取結果。
#pragma once
// ---- Per-board geometry (override via build_flags in boards/*.yaml) ----
//   RADAR_CANVAS  square radar canvas side (px); must equal the LVGL
//                 base_canvas size in the matching ui/ package.
//   SCREEN_W/H    panel size, used by the screenshot framebuffer copy.
//   RADAR_DISPLAY_RGB  which display driver the screenshot grab should use:
//                 1 = parallel-RGB via `rpi_dpi_rgb`  (generic 800x480 board)
//                 2 = parallel-RGB via `mipi_rgb`     (Waveshare Touch-LCD-5/5B)
//                 3 = MIPI-DSI via `mipi_dsi`         (Waveshare ESP32-P4 7B)
//                 0 = none -> screenshot disabled
//                 1 and 2 are both parallel-RGB panels, so the IDF call
//                 esp_lcd_rgb_panel_get_frame_buffer() works for both; only the
//                 ESPHome wrapper class holding the panel handle differs.
//                 3 is a DPI panel created by esp_lcd_new_panel_dpi(), so the
//                 read-back call is esp_lcd_dpi_panel_get_frame_buffer() instead
//                 -- everything after the grab (BMP header, row conversion, the
//                 :8081 server, sd_save_shot) is shared. The macro name says RGB
//                 for historical reasons; renaming it would touch every board
//                 file and both README language sections for no benefit.
#ifndef RADAR_CANVAS
#define RADAR_CANVAS 456
#endif
#ifndef SCREEN_W
#define SCREEN_W 800
#endif
#ifndef SCREEN_H
#define SCREEN_H 480
#endif
#ifndef RADAR_DISPLAY_RGB
#define RADAR_DISPLAY_RGB 1
#endif
// 背光是否接在可 PWM 的原生 GPIO。Waveshare ESP32-S3-Touch-LCD-5/5B 的背光走
// CH422G 擴充腳(EXIO2),官方 waveshare_lcd_port.h 的 EXAMPLE_LCD_BL_IO = -1,
// 且 CH422G 無 PWM 通道 → 只能開/關。那些板子設 0,亮度滑桿改控 LVGL 暗化遮罩。
#ifndef RADAR_BL_PWM
#define RADAR_BL_PWM 1
#endif
// 三指截圖是否另存一份到 microSD。Waveshare Touch-LCD-5/5B 的 TF 走 SPI:
// MOSI/CLK/MISO 是原生 GPIO,但 CS 在 CH422G EXIO4(擴充腳,無法給 sdspi 驅動用)。
// 因為那條 SPI 上只有 SD 一個裝置,板檔用一個 gpio switch 於開機時把 EXIO4 拉低
// (restore_mode: ALWAYS_OFF)並保持,驅動這邊就以 SDSPI_SLOT_NO_CS 掛載。
// 沒有卡或掛載失敗時只是不寫檔,HTTP(:8081)那條路照舊。
#ifndef RADAR_SD_SPI
#define RADAR_SD_SPI 0
#endif
// RADAR_SD_EXT:卡由「別人」掛好,我們只負責寫檔。ESP32-P4 走這條——TF 插槽接在
// 原生 SDMMC 上,掛載交給板檔的 sd_storage 元件(見 boards/waveshare_esp32p4_*)。
// 兩種模式的差別只在 sd_mount();寫檔那半段完全共用,因為掛好之後就只是
// fopen("/sdcard/...") 而已。兩者互斥,不要同時開。
#ifndef RADAR_SD_EXT
#define RADAR_SD_EXT 0
#endif
#define RADAR_SD_ANY (RADAR_SD_SPI || RADAR_SD_EXT)
// 雷達可同時顯示的航班數(= ui/*.yaml 裡 ac/ai/sq/ad/vec/tr 這幾組 widget 的組數)。
// 資料端不設上限:radar_fetch 依距離由近而遠排序,超過這個數的遠機不繪出。
// 改這個值必須同步 ui/ui_800x480.yaml 的 widget 組數(並重跑 tools/scale_layout.py)。
#define AC_SLOTS 40
#define RADAR_CX (RADAR_CANVAS / 2)        // canvas center
#define RADAR_R  (RADAR_CANVAS / 2 - 2)    // usable radar radius
// UI scale factor vs the reference 800x480 layout (456px canvas). The LVGL
// layouts for other resolutions are generated from that reference by
// tools/scale_layout.py with the same round-half-up rule, so RS() of a
// reference-layout pixel value always lands on the generated widget position.
// Used by the YAML lambdas in common/core.yaml for page-coordinate math.
#define RADAR_SCALE ((float) RADAR_CANVAS / 456.0f)
#define RS(v) ((int) ((v) * RADAR_SCALE + 0.5f))
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cmath>
#include <map>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#if RADAR_DISPLAY_RGB == 1 || RADAR_DISPLAY_RGB == 2
#include "esp_lcd_panel_rgb.h"
#elif RADAR_DISPLAY_RGB == 3
#include "esp_lcd_mipi_dsi.h"
#endif
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#if RADAR_SD_SPI
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#endif
#if RADAR_SD_ANY
#include <ctime>          // 檔名的時間戳,兩種掛載模式都要
#endif
extern "C" {
#include "pngle.h"
}
#include "esp_log.h"
#include "esphome/core/application.h"   // App.feed_wdt():截圖寫卡是主迴圈裡的長工作
#include "esphome/components/json/json_util.h"
#include "esphome/components/image/image.h"
#if RADAR_DISPLAY_RGB == 1
#include "esphome/components/rpi_dpi_rgb/rpi_dpi_rgb.h"
#elif RADAR_DISPLAY_RGB == 2
#include "esphome/components/mipi_rgb/mipi_rgb.h"
#elif RADAR_DISPLAY_RGB == 3
#include "esphome/components/mipi_dsi/mipi_dsi.h"
#endif
#include "map_tiles.h"   // 地圖改為執行期從 maps 分割區載入(原 map_data.h)

namespace radar_bg {

struct AcInfo {
  float lat, lon, trk, vel, alt, vr, dist;
  uint32_t lc;   // last_contact (epoch 秒),ATC 模式判斷訊號延遲用
  std::string cs;
  std::string sq;   // squawk / mode-A code(三家來源都有;OpenSky 是 states[14])
  std::string ty;   // ICAO 機型代碼(如 B738);只有 airplanes.live / adsb.lol 提供
  // 以下四項只有 readsb v2(airplanes.live / adsb.lol)有,OpenSky 全部留空/0。
  std::string reg;  // 註冊號(如 B-5416)
  std::string desc; // 機型全名(如 BOEING 737-800);aircraft_db 查不到時的後備
  std::string cat;  // ADS-B 發射器類別(A1~A7 / B1~B7 / C1~C3),查不到機型時的通用輪廓
  uint32_t hex = 0; // ICAO24 位址,查註冊國籍用(0 = 未知,OpenSky 走這條)
};

struct Job {
  int type;  // 1 = states, 2 = route, 3 = echo, 4 = weather, 5 = speakers,
             // 6 = route cache, 7 = 機型查詢(callsign 欄放 ICAO24 十六進位字串),
             // 8 = 下載地圖圖磚
  std::string cid, sec, callsign;
  float lat, lon, range;
  int src;   // 0=OpenSky 1=A.LIVE 2=ADSB.LOL 3=ADSB.FI 4=MERGE(多源合併)
};

// ---- task → main 的結果(g_states_ready/g_route_ready 當柵欄)----
inline std::vector<AcInfo> g_result;
inline volatile bool g_states_ready = false;
inline std::string g_route;
inline volatile bool g_route_ready = false;
inline uint8_t *g_echo_buf = nullptr;    // 離屏合成緩衝 456*456*3 (PSRAM)
inline volatile bool g_echo_ready = false;   // true=g_echo_buf 有整幀待主迴圈換上
// 回波非透明像素的逐列水平範圍(x0 > x1 表示該列全透明)。
// 底圖重建每次都要把回波混進去,原本是整張 RADAR_CANVAS² 逐像素掃(570² =
// 324,900 次,每次讀 3 bytes PSRAM ≈ 950KB),但降雨通常只佔畫面一小塊、常常
// 甚至整張全空。合成時順手記下每列的左右界,混合就只走真的有資料的區段;
// 空白列連一次 PSRAM 讀取都不必。初值 0/0 是安全的:未寫入的緩衝 alpha=0,
// 混合迴圈本來就會跳過。
inline int16_t g_echo_x0[RADAR_CANVAS];
inline int16_t g_echo_x1[RADAR_CANVAS];
inline void echo_spans_clear() {
  for (int y = 0; y < RADAR_CANVAS; y++) { g_echo_x0[y] = RADAR_CANVAS; g_echo_x1[y] = -1; }
}
inline volatile bool g_auth_fail = false;
struct WxInfo { float temp, hum, wspd, wdir; };   // 在地天氣(Open-Meteo)
inline WxInfo g_wx;
inline volatile bool g_wx_ready = false;   // true=g_wx 有新資料待主迴圈取用
inline bool g_wx_valid = false;            // 曾成功抓過至少一次
inline std::string g_speakers;             // HA 喇叭清單:每行 entity_id|friendly_name
inline volatile bool g_speakers_ready = false;
inline int g_spk_status = 0;               // HTTP 狀態:200 成功 / 401 token 錯 / <=0 連不上

inline volatile int g_os_remaining = -1;   // OpenSky X-Rate-Limit-Remaining(-1=未知)
inline bool g_want_rl = false;             // 只在 states 請求期間擷取(bg task 序列執行,無競態)
inline volatile uint32_t g_os_cooldown_until = 0;  // OpenSky 失敗冷卻期限(millis 秒),期間走免費來源
inline volatile uint32_t g_alive_cooldown_until = 0;  // airplanes.live 403/失敗後冷卻(秒)
inline volatile int g_last_src = -1;       // 最近成功來源:0..4,-1=尚未;MERGE 時為 4

inline const char *src_name(int src) {
  switch (src) {
    case 1: return "A.LIVE";
    case 2: return "ADSB.LOL";
    case 3: return "ADSB.FI";
    case 4: return "MERGE";
    case 0: return "OPENSKY";
    default: return "----";
  }
}

// HTTP body 上限。150KB 太緊:250km 半徑在繁忙空域(英國、日本)的航班清單就會
// 超過,回應被截斷後解析必定失敗。字串配在 PSRAM,384KB 對 8MB PSRAM 綽綽有餘,
// 而且每次成長前都會檢查最大可用區塊,不會因為調高而變得容易 abort。
static const size_t HTTP_MAX_BODY = 384 * 1024;

inline esp_err_t http_evt_cb(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_HEADER && g_want_rl &&
      strcasecmp(evt->header_key, "X-Rate-Limit-Remaining") == 0)
    g_os_remaining = atoi(evt->header_value);
  return ESP_OK;
}

inline SemaphoreHandle_t mtx() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();
  return m;
}
inline QueueHandle_t queue() {
  static QueueHandle_t q = xQueueCreate(4, sizeof(Job *));
  return q;
}

// ---- 簡易 HTTP(直接用 esp_http_client,不經 ESPHome 元件)----
inline std::string http_req(const std::string &url, bool post, const std::string &body,
                            const char *ctype, const std::string &bearer, int &status,
                            size_t reserve_hint = 8192) {
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = 20000;
  cfg.buffer_size = 4096;
  cfg.buffer_size_tx = 8192;   // OpenSky JWT 很大
  cfg.method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET;
  cfg.event_handler = http_evt_cb;   // 擷取 OpenSky 額度 header(g_want_rl 才動作)
  status = -1;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c == nullptr) return "";
  if (ctype != nullptr) esp_http_client_set_header(c, "Content-Type", ctype);
  std::string auth;
  if (!bearer.empty()) {
    auth = "Bearer " + bearer;
    esp_http_client_set_header(c, "Authorization", auth.c_str());
  }
  // 記憶體不足時要「這次抓取失敗」,不能變成「整台重開」。
  // C++ 例外是關閉的(CONFIG_COMPILER_CXX_EXCEPTIONS 未設),所以 std::string
  // 配置失敗會丟 bad_alloc → __wrap___cxa_allocate_exception → abort()。
  // 專案裡所有 heap_caps_malloc 都有檢查 null,只有 std::string 這條路徑沒有、
  // 也無法用 try/catch 擋,只能事前確認要得到才動手。
  // (P4 v1.3.0 實機就是這樣無限重開的:回波把記憶體推到尖峰,下一個請求的
  //  128KB reserve 失敗 → abort。)
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
  if (largest < reserve_hint + 16384) {
    ESP_LOGW("radar_bg", "http: skipping %.60s -- largest free block %uKB < %uKB needed",
             url.c_str(), (unsigned) (largest / 1024),
             (unsigned) ((reserve_hint + 16384) / 1024));
    esp_http_client_cleanup(c);
    status = -2;              // 呼叫端一律把非 200 視為失敗,會自行退避重試
    return "";
  }
  std::string out;
  out.reserve(reserve_hint);   // >16KB 的配置會進 PSRAM(SPIRAM_USE_MALLOC)
  if (esp_http_client_open(c, body.size()) == ESP_OK) {
    if (!body.empty()) esp_http_client_write(c, body.c_str(), body.size());
    esp_http_client_fetch_headers(c);
    status = esp_http_client_get_status_code(c);
    char buf[2048];
    int n;
    bool truncated = false;
    while ((n = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
      // 成長也會丟例外,所以上限之外再看一次剩餘空間,寧可截斷也不要 abort
      if (out.size() > HTTP_MAX_BODY) {
        ESP_LOGW("radar_bg", "http: response over %uKB cap -- %.60s",
                 (unsigned) (HTTP_MAX_BODY / 1024), url.c_str());
        truncated = true;
        break;
      }
      if (out.size() + n > out.capacity() &&
          heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < out.capacity() * 2 + 16384) {
        ESP_LOGW("radar_bg", "http: truncating at %u bytes -- out of room", (unsigned) out.size());
        truncated = true;
        break;
      }
      out.append(buf, n);
    }
    // 截斷過的 body 一定是壞掉的 JSON。以前直接回傳,呼叫端照樣拿去解析,
    // 結果只看得到一句 `Parse error: IncompleteInput` —— 完全指不出真正的原因
    // (使用者把半徑調到 250km、空域又密,航班清單就超過上限)。現在當成失敗
    // 回報,呼叫端本來就會退避重試,而 log 直接說出是哪個網址、超過多少。
    if (truncated) {
      status = -3;
      out.clear();
      out.shrink_to_fit();
    }
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return out;
}

// ---- OpenSky token(task 內部狀態)----
inline std::string t_token;
inline uint32_t t_token_exp = 0;

inline bool ensure_token(const Job &j) {
  if (!t_token.empty() && (millis() / 1000) < t_token_exp) return true;
  int st = 0;
  std::string b = "grant_type=client_credentials&client_id=" + j.cid +
                  "&client_secret=" + j.sec;
  std::string r = http_req(
      "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token",
      true, b, "application/x-www-form-urlencoded", "", st);
  if (st != 200 || r.empty()) {
    ESP_LOGW("radar_bg", "token failed: %d", st);
    t_token.clear();
    g_auth_fail = true;
    return false;
  }
  bool ok = esphome::json::parse_json(r, [](JsonObject root) -> bool {
    t_token = (const char *) (root["access_token"] | "");
    uint32_t exp = root["expires_in"] | 1800;
    t_token_exp = millis() / 1000 + exp - 120;
    return true;
  });
  if (!ok || t_token.empty()) { g_auth_fail = true; return false; }
  ESP_LOGI("radar_bg", "token OK");
  return true;
}

inline void publish_states(std::vector<AcInfo> &&acs, int src) {
  std::sort(acs.begin(), acs.end(),
            [](const AcInfo &a, const AcInfo &b) { return a.dist < b.dist; });
  xSemaphoreTake(mtx(), portMAX_DELAY);
  g_result = std::move(acs);
  g_states_ready = true;
  g_last_src = src;
  xSemaphoreGive(mtx());
}

// 依 ICAO24(hex)優先、否則呼號+近距離 合併;較新/較完整的欄位覆蓋空欄
inline void merge_into(std::vector<AcInfo> &dst, std::vector<AcInfo> &&src) {
  for (auto &a : src) {
    int found = -1;
    for (size_t i = 0; i < dst.size(); i++) {
      if (a.hex != 0 && dst[i].hex != 0 && a.hex == dst[i].hex) { found = (int) i; break; }
      if (!a.cs.empty() && a.cs != "?" && a.cs == dst[i].cs) {
        float dlat = a.lat - dst[i].lat, dlon = a.lon - dst[i].lon;
        if (dlat * dlat + dlon * dlon < 0.02f * 0.02f) { found = (int) i; break; }
      }
    }
    if (found < 0) { dst.push_back(std::move(a)); continue; }
    AcInfo &t = dst[(size_t) found];
    bool newer = a.lc >= t.lc;
    if (newer) {
      t.lat = a.lat; t.lon = a.lon; t.trk = a.trk; t.vel = a.vel;
      t.alt = a.alt; t.vr = a.vr; t.dist = a.dist; t.lc = a.lc;
    }
    if (t.hex == 0 && a.hex != 0) t.hex = a.hex;
    if (t.sq.empty() && !a.sq.empty()) t.sq = std::move(a.sq);
    if (t.ty.empty() && !a.ty.empty()) t.ty = std::move(a.ty);
    if (t.reg.empty() && !a.reg.empty()) t.reg = std::move(a.reg);
    if (t.desc.empty() && !a.desc.empty()) t.desc = std::move(a.desc);
    if (t.cat.empty() && !a.cat.empty()) t.cat = std::move(a.cat);
    if ((t.cs.empty() || t.cs == "?") && !a.cs.empty()) t.cs = std::move(a.cs);
  }
}

inline bool fetch_opensky(const Job &j, std::vector<AcInfo> &out) {
  // 有憑證走 OAuth;無憑證改匿名 bbox(額度更緊,但青島一帶常比免費聚合密)
  const bool want_auth = !j.cid.empty() && !j.sec.empty();
  if (want_auth && !ensure_token(j)) return false;
  float coslat = cosf(j.lat * 3.14159265f / 180.0f);
  float dlat = j.range / 110.574f;
  float dlon = j.range / (111.320f * coslat);
  char url[192];
  snprintf(url, sizeof(url),
           "https://opensky-network.org/api/states/all?lamin=%.4f&lomin=%.4f&lamax=%.4f&lomax=%.4f",
           j.lat - dlat, j.lon - dlon, j.lat + dlat, j.lon + dlon);
  int st = 0;
  g_want_rl = want_auth;
  std::string r = http_req(url, false, "", nullptr, want_auth ? t_token : "", st, 131072);
  g_want_rl = false;
  if (st == 401) { t_token.clear(); return false; }
  if (st != 200 || r.empty()) {
    ESP_LOGW("radar_bg", "opensky states failed: %d (%u bytes)", st, (unsigned) r.size());
    return false;
  }
  float lat0 = j.lat, lon0 = j.lon;
  esphome::json::parse_json(r, [&](JsonObject root) -> bool {
    JsonArray sts = root["states"].as<JsonArray>();
    if (sts.isNull()) return true;
    for (JsonVariant v : sts) {
      JsonArray a = v.as<JsonArray>();
      if (a.isNull()) continue;
      if (a[8] | false) continue;                 // on_ground
      float alon = a[5] | NAN, alat = a[6] | NAN;
      if (isnan(alon) || isnan(alat)) continue;
      AcInfo ac;
      ac.lat = alat; ac.lon = alon;
      ac.alt = a[7] | 0.0f;
      ac.vel = a[9] | 0.0f;
      ac.trk = a[10] | 0.0f;
      ac.vr  = a[11] | 0.0f;
      ac.lc  = a[4] | 0u;
      ac.sq  = a[14] | "";     // squawk;OpenSky 不提供機型,ty 由 hex 事後查 adsbdb 補
      ac.hex = (uint32_t) strtoul(a[0] | "0", nullptr, 16);   // states[0] = ICAO24
      const char *c = a[1] | "";
      ac.cs = c;
      while (!ac.cs.empty() && ac.cs.back() == ' ') ac.cs.pop_back();
      if (ac.cs.empty()) ac.cs = "?";
      float e = (alon - lon0) * 111.320f * coslat;
      float n = (alat - lat0) * 110.574f;
      ac.dist = sqrtf(e * e + n * n);
      out.push_back(std::move(ac));
    }
    return true;
  });
  return true;
}

// readsb 相容 /v2/point 或 adsb.fi /v3/lat/.../dist/...
inline bool fetch_readsb_json(const std::string &url, float lat0, float lon0, float /*range*/,
                              std::vector<AcInfo> &out) {
  int st = 0;
  std::string r = http_req(url, false, "", nullptr, "", st, 131072);
  if (st != 200 || r.empty()) {
    ESP_LOGW("radar_bg", "readsb failed %d (%u) %s", st, (unsigned) r.size(), url.c_str());
    return false;
  }
  float coslat = cosf(lat0 * 3.14159265f / 180.0f);
  esphome::json::parse_json(r, [&](JsonObject root) -> bool {
    uint32_t now_s = (uint32_t) ((root["now"] | 0.0) / 1000.0);
    if (now_s == 0) now_s = (uint32_t) (millis() / 1000);  // 部分源 now 單位不同時的後備
    JsonArray arr = root["ac"].as<JsonArray>();
    if (arr.isNull()) return true;
    for (JsonVariant v : arr) {
      JsonObject a = v.as<JsonObject>();
      if (a.isNull()) continue;
      if (a["alt_baro"].is<const char *>()) continue;   // "ground"
      float alat = a["lat"] | NAN, alon = a["lon"] | NAN;
      if (isnan(alat) || isnan(alon)) continue;
      AcInfo ac;
      ac.lat = alat; ac.lon = alon;
      ac.alt = (a["alt_baro"] | 0.0f) * 0.3048f;
      ac.vel = (a["gs"] | 0.0f) * 0.514444f;
      ac.trk = a["track"] | 0.0f;
      ac.vr  = (a["baro_rate"] | 0.0f) * 0.00508f;
      float seen = a["seen"] | 0.0f;
      ac.lc = now_s > (uint32_t) seen ? now_s - (uint32_t) seen : 0;
      ac.sq = a["squawk"] | "";
      ac.ty = a["t"] | "";
      ac.reg = a["r"] | "";
      ac.desc = a["desc"] | "";
      ac.cat = a["category"] | "";
      ac.hex = (uint32_t) strtoul(a["hex"] | "0", nullptr, 16);
      const char *c = a["flight"] | "";
      ac.cs = c;
      while (!ac.cs.empty() && ac.cs.back() == ' ') ac.cs.pop_back();
      if (ac.cs.empty()) ac.cs = "?";
      float e = (alon - lon0) * 111.320f * coslat;
      float n = (alat - lat0) * 110.574f;
      ac.dist = sqrtf(e * e + n * n);
      out.push_back(std::move(ac));
    }
    return true;
  });
  return true;
}

inline bool fetch_v2_host(const Job &j, const char *host, std::vector<AcInfo> &out) {
  float r_nm = j.range / 1.852f;
  if (r_nm > 250.0f) r_nm = 250.0f;
  char url[160];
  snprintf(url, sizeof(url), "https://%s/v2/point/%.4f/%.4f/%.0f", host, j.lat, j.lon, r_nm);
  return fetch_readsb_json(url, j.lat, j.lon, j.range, out);
}

// airplanes.live 常對本機 UA 回 403;失敗後冷卻,避免每輪白打
inline bool fetch_airplanes_live(const Job &j, std::vector<AcInfo> &out) {
  uint32_t now = millis() / 1000;
  if (now < g_alive_cooldown_until) return false;
  if (fetch_v2_host(j, "api.airplanes.live", out)) {
    g_alive_cooldown_until = 0;
    return true;
  }
  g_alive_cooldown_until = now + 60;  // 1 分鐘
  ESP_LOGW("radar_bg", "airplanes.live cooling down 60s");
  return false;
}

inline bool fetch_adsb_fi(const Job &j, std::vector<AcInfo> &out) {
  float r_nm = j.range / 1.852f;
  if (r_nm > 250.0f) r_nm = 250.0f;
  char url[192];
  snprintf(url, sizeof(url),
           "https://opendata.adsb.fi/api/v3/lat/%.4f/lon/%.4f/dist/%.0f",
           j.lat, j.lon, r_nm);
  return fetch_readsb_json(url, j.lat, j.lon, j.range, out);
}

inline bool do_states_opensky(const Job &j) {
  std::vector<AcInfo> acs;
  if (!fetch_opensky(j, acs)) return false;
  publish_states(std::move(acs), 0);
  return true;
}

inline bool do_states_v2(const Job &j, int src) {
  std::vector<AcInfo> acs;
  bool ok = false;
  if (src == 3) ok = fetch_adsb_fi(j, acs);
  else if (src == 2) ok = fetch_v2_host(j, "api.adsb.lol", acs);
  else {
    ok = fetch_airplanes_live(j, acs);
    if (!ok) ok = fetch_v2_host(j, "api.adsb.lol", acs);  // A.LIVE 冷卻/失敗 → LOL
    if (ok && src == 1) src = 2;
  }
  if (!ok) return false;
  publish_states(std::move(acs), src);
  return true;
}

// 多源合併:OpenSky(可匿名)+ADSB.LOL+ADSB.FI(+A.LIVE 未冷卻時),按 hex 去重補欄
inline void do_states_merge(const Job &j) {
  std::vector<AcInfo> all;
  std::vector<AcInfo> part;
  int ok_n = 0;
  part.clear();
  if (fetch_opensky(j, part)) { merge_into(all, std::move(part)); ok_n++; }
  part.clear();
  if (fetch_v2_host(j, "api.adsb.lol", part)) { merge_into(all, std::move(part)); ok_n++; }
  part.clear();
  if (fetch_adsb_fi(j, part)) { merge_into(all, std::move(part)); ok_n++; }
  part.clear();
  if (fetch_airplanes_live(j, part)) { merge_into(all, std::move(part)); ok_n++; }
  if (ok_n == 0) {
    ESP_LOGW("radar_bg", "merge: all sources failed");
    return;
  }
  ESP_LOGI("radar_bg", "merge: %d sources ok, %u aircraft", ok_n, (unsigned) all.size());
  publish_states(std::move(all), 4);
}

inline void do_states(const Job &j) {
  if (j.src == 4) { do_states_merge(j); return; }
  if (j.src == 1 || j.src == 2 || j.src == 3) { do_states_v2(j, j.src); return; }
  // OpenSky 主線:冷卻中直接走合併免費源;失敗設 10 分鐘冷卻
  uint32_t now = millis() / 1000;
  if (now >= g_os_cooldown_until) {
    if (do_states_opensky(j)) { g_os_cooldown_until = 0; return; }
    g_os_cooldown_until = now + 600;
    ESP_LOGW("radar_bg", "opensky failed, fallback merge for 600s");
  }
  // adsb.lol 先試,airplanes.live 當第二順位:後者自 2026-08-13 起關閉公開 API,
  // 對所有人一律 403(不是我們被封)。原本的順序等於每一輪都先浪費一次必定失敗
  // 的 TLS 連線,才輪到真正能用的來源 —— 也讓 adsb.lol 更容易撞到它的速率限制。
  // 保留它當第二順位:如果哪天恢復開放,不必改碼就會自己回來。
  // airplanes.live 路徑仍走 fetch_airplanes_live() 的 g_alive_cooldown_until。
  if (!do_states_v2(j, 2)) do_states_v2(j, 1);   // adsb.lol → airplanes.live
}

inline void do_route(const Job &j) {
  int st = 0;
  std::string r = http_req("https://api.adsbdb.com/v0/callsign/" + j.callsign,
                           false, "", nullptr, "", st);
  std::string rt = "ROUTE N/A";
  if (st == 200 && !r.empty()) {
    esphome::json::parse_json(r, [&](JsonObject root) -> bool {
      JsonObject fr = root["response"]["flightroute"];
      if (!fr.isNull()) {
        const char *o = fr["origin"]["iata_code"] | "";
        const char *d = fr["destination"]["iata_code"] | "";
        if (o[0] != 0 && d[0] != 0)
          rt = std::string("ROUTE  ") + o + " - " + d;
      }
      return true;
    });
  }
  xSemaphoreTake(mtx(), portMAX_DELAY);
  g_route = rt;
  g_route_ready = true;
  xSemaphoreGive(mtx());
}

// ---- 全機隊起訖站快取(ATC ROUTE 標籤行)----
// callsign → "KHH-KIX";"" = 查詢中,"-" = 查無資料(不再重查)。
// 與 g_route 同一把 mtx 保護;背景 task 逐架查 adsbdb,查過即快取。
inline std::map<std::string, std::string> g_rcache;

inline void do_route_cache(const Job &j) {
  int st = 0;
  std::string r = http_req("https://api.adsbdb.com/v0/callsign/" + j.callsign,
                           false, "", nullptr, "", st);
  std::string rt = "-";
  if (st == 200 && !r.empty()) {
    esphome::json::parse_json(r, [&](JsonObject root) -> bool {
      JsonObject fr = root["response"]["flightroute"];
      if (!fr.isNull()) {
        const char *o = fr["origin"]["iata_code"] | "";
        const char *d = fr["destination"]["iata_code"] | "";
        if (o[0] != 0 && d[0] != 0) rt = std::string(o) + "-" + d;
      }
      return true;
    });
  } else if (st != 200 && st != 404) {
    // 非 404 的失敗(網路/限流)不寫入快取,之後重試
    xSemaphoreTake(mtx(), portMAX_DELAY);
    g_rcache.erase(j.callsign);
    xSemaphoreGive(mtx());
    return;
  }
  xSemaphoreTake(mtx(), portMAX_DELAY);
  g_rcache[j.callsign] = rt;
  xSemaphoreGive(mtx());
}

// ---- 機型快取(OpenSky 用;ICAO24 → 機型代碼 / 註冊號 / 全名)----
// readsb 來源(airplanes.live / adsb.lol)本來就給 t/r/desc,這條路只給 OpenSky 走:
// OpenSky 的 states 只有 ICAO24,機型要另外查 adsbdb(查航線用的同一個免金鑰 API)。
// done=false 表示「查詢中」,用來去重;done=true 而 ty 空 = 查無資料,不再重查。
struct AcMeta { std::string ty, reg, desc; bool done = false; };
inline std::map<uint32_t, AcMeta> g_tcache;

inline void do_type_cache(const Job &j) {
  int st = 0;
  std::string r = http_req("https://api.adsbdb.com/v0/aircraft/" + j.callsign,
                           false, "", nullptr, "", st);
  AcMeta m;
  m.done = true;
  if (st == 200 && !r.empty()) {
    esphome::json::parse_json(r, [&](JsonObject root) -> bool {
      JsonObject a = root["response"]["aircraft"];
      if (a.isNull()) return true;
      m.ty  = a["icao_type"] | "";      // 4 碼 ICAO 代碼,正是 ac_spec_find() 要的
      m.reg = a["registration"] | "";
      std::string mfr = a["manufacturer"] | "";
      std::string mdl = a["type"] | "";
      m.desc = mfr.empty() ? mdl : (mdl.empty() ? mfr : mfr + " " + mdl);
      for (char &c : m.desc) c = toupper((unsigned char) c);   // 面板其餘欄位都是大寫
      return true;
    });
  } else if (st != 200 && st != 404) {
    // 非 404 的失敗(網路/限流)不寫入快取,之後重試
    xSemaphoreTake(mtx(), portMAX_DELAY);
    g_tcache.erase((uint32_t) strtoul(j.callsign.c_str(), nullptr, 16));
    xSemaphoreGive(mtx());
    return;
  }
  xSemaphoreTake(mtx(), portMAX_DELAY);
  g_tcache[(uint32_t) strtoul(j.callsign.c_str(), nullptr, 16)] = m;
  xSemaphoreGive(mtx());
}

// Open-Meteo 目前天氣(免金鑰、開源資料)→ g_wx
inline void do_weather(const Job &j) {
  char url[224];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,wind_speed_10m,wind_direction_10m",
           j.lat, j.lon);
  int st = 0;
  std::string r = http_req(url, false, "", nullptr, "", st);
  if (st != 200 || r.empty()) {
    ESP_LOGW("radar_bg", "weather failed: %d", st);
    return;
  }
  WxInfo w{};
  bool got = false;
  esphome::json::parse_json(r, [&](JsonObject root) -> bool {
    JsonObject cur = root["current"];
    if (cur.isNull()) return true;
    w.temp = cur["temperature_2m"] | NAN;
    w.hum  = cur["relative_humidity_2m"] | NAN;
    w.wspd = cur["wind_speed_10m"] | NAN;
    w.wdir = cur["wind_direction_10m"] | NAN;
    got = !isnan(w.temp);
    return true;
  });
  if (!got) { ESP_LOGW("radar_bg", "weather parse fail"); return; }
  xSemaphoreTake(mtx(), portMAX_DELAY);
  g_wx = w;
  g_wx_valid = true;
  g_wx_ready = true;
  xSemaphoreGive(mtx());
}

// 用 HA REST /api/template 列出所有 media_player(j.cid=HA URL、j.sec=長期 token)
inline void do_speakers(const Job &j) {
  std::string url = j.cid;
  while (!url.empty() && url.back() == '/') url.pop_back();
  url += "/api/template";
  // Jinja 模板在 HA 端渲染,回應為純文字:每行 entity_id|friendly_name
  std::string body =
      "{\"template\":\"{% for e in states.media_player %}"
      "{{ e.entity_id }}|{{ e.name }}\\n{% endfor %}\"}";
  int st = 0;
  std::string r = http_req(url, true, body, "application/json", j.sec, st);
  ESP_LOGI("radar_bg", "speakers http %d (%u bytes)", st, (unsigned) r.size());
  xSemaphoreTake(mtx(), portMAX_DELAY);
  g_spk_status = st;
  g_speakers = (st == 200) ? r : "";
  g_speakers_ready = true;
  xSemaphoreGive(mtx());
}

// ---- pngle 解碼 context:把 tile 像素寫進 512x512 RGBA 暫存 ----
struct PngCtx { uint8_t *rgba; int w, h; };

inline void png_on_draw(pngle_t *p, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        const uint8_t rgba[4]) {
  PngCtx *c = (PngCtx *) pngle_get_user_data(p);
  for (uint32_t yy = y; yy < y + h && (int) yy < c->h; yy++) {
    for (uint32_t xx = x; xx < x + w && (int) xx < c->w; xx++) {
      uint8_t *d = c->rgba + ((size_t) yy * c->w + xx) * 4;
      d[0] = rgba[0]; d[1] = rgba[1]; d[2] = rgba[2]; d[3] = rgba[3];
    }
  }
}

// 下載單一 tile PNG → pngle 解碼進 tmp RGBA → 取樣合成進 g_echo_buf 的子矩形
// canvas 3 bytes/px:[color_lo][color_hi][alpha](= LVGL TRUE_COLOR_ALPHA 16bpp)
inline void echo_composite_tile(const std::string &url, uint8_t *tmp, int tw, int th,
                                int i, int j, float bx, float by,
                                float tile_km, float range) {
  int st = 0;
  std::string png = http_req(url, false, "", nullptr, "", st, 32768);
  if (st != 200 || png.empty()) { ESP_LOGW("radar_bg", "tile http %d", st); return; }
  memset(tmp, 0, (size_t) tw * th * 4);
  pngle_t *p = pngle_new();
  if (!p) return;
  PngCtx ctx = { tmp, tw, th };
  pngle_set_user_data(p, &ctx);
  pngle_set_draw_callback(p, png_on_draw);
  int fed = pngle_feed(p, png.data(), png.size());
  int iw = pngle_get_width(p);
  pngle_destroy(p);
  if (iw <= 0) iw = tw;
  if (fed < 0) { ESP_LOGW("radar_bg", "png decode err"); return; }

  float kmpp = 2.0f * range / (float) RADAR_CANVAS;
  float inv = tile_km / kmpp;
  int x_lo = (int) floorf(RADAR_CX + ((float) i - bx) * inv);
  int x_hi = (int) ceilf (RADAR_CX + ((float) (i + 1) - bx) * inv);
  int y_lo = (int) floorf(RADAR_CX + ((float) j - by) * inv);
  int y_hi = (int) ceilf (RADAR_CX + ((float) (j + 1) - by) * inv);
  if (x_lo < 0) x_lo = 0; if (x_hi > RADAR_CANVAS) x_hi = RADAR_CANVAS;
  if (y_lo < 0) y_lo = 0; if (y_hi > RADAR_CANVAS) y_hi = RADAR_CANVAS;
  // 每像素原本要做 2 次浮點除法(ESP32-S3 的 FPU 沒有除法指令,編譯器會展開成
  // 倒數近似 + 牛頓迭代)。tile_km 在整個迴圈裡是常數,先取倒數改成乘法,kx/ky
  // 也改成沿著掃描線累加,內圈就只剩乘加。~32 萬像素下省下的是幾十毫秒等級,
  // 但這段本來就跑在背景 task,實際感受有限。
  const float inv_tile = 1.0f / tile_km;
  const float inv_tile_tw = inv_tile * (float) tw;
  const float inv_tile_th = inv_tile * (float) th;
  const float i_tile = (float) i * tile_km;
  const float j_tile = (float) j * tile_km;
  const float kx0 = bx * tile_km + (float) (x_lo - RADAR_CX) * kmpp;
  for (int y = y_lo; y < y_hi; y++) {
    float ky = by * tile_km + (float) (y - RADAR_CX) * kmpp;
    int tj = (int) floorf(ky * inv_tile);
    if (tj != j) continue;
    int ty = (int) ((ky - j_tile) * inv_tile_th);
    if (ty < 0 || ty >= th) continue;
    int dy2 = (y - RADAR_CX) * (y - RADAR_CX);
    uint8_t *orow = g_echo_buf + (size_t) y * RADAR_CANVAS * 3;
    float kx = kx0;
    for (int x = x_lo; x < x_hi; x++, kx += kmpp) {
      int ti = (int) floorf(kx * inv_tile);
      if (ti != i) continue;
      if ((x - RADAR_CX) * (x - RADAR_CX) + dy2 > RADAR_R * RADAR_R) continue;   // 圓外(留透明)
      int tx = (int) ((kx - i_tile) * inv_tile_tw);
      if (tx < 0 || tx >= tw) continue;
      uint8_t *sp = tmp + ((size_t) ty * tw + tx) * 4;
      if (sp[3] < 32) continue;   // 無降雨=透明
      // LVGL 9 的 lv_color_t 一律是 24-bit,畫布緩衝才是 RGB565 → 存前先轉
      uint16_t col = lv_color_to_u16(lv_color_make(sp[0], sp[1], sp[2]));
      uint8_t *op = orow + (size_t) x * 3;
      op[0] = col & 0xFF;
      op[1] = (col >> 8) & 0xFF;
      op[2] = sp[3];
      if (x < g_echo_x0[y]) g_echo_x0[y] = (int16_t) x;   // 這列有降雨的左右界,
      if (x > g_echo_x1[y]) g_echo_x1[y] = (int16_t) x;   // 供底圖混合跳過空白區
    }
  }
}

// RainViewer:抓最新圖層 → 2x2 拼磚,全程背景下載+解碼+合成到 g_echo_buf
inline void do_echo(const Job &j) {
  int st = 0;
  std::string r = http_req("https://api.rainviewer.com/public/weather-maps.json",
                           false, "", nullptr, "", st);
  if (st != 200 || r.empty()) { ESP_LOGW("radar_bg", "rainviewer meta %d", st); return; }
  std::string host, path;
  esphome::json::parse_json(r, [&](JsonObject root) -> bool {
    host = (const char *) (root["host"] | "");
    JsonArray past = root["radar"]["past"].as<JsonArray>();
    if (!past.isNull() && past.size() > 0)
      path = (const char *) (past[past.size() - 1]["path"] | "");
    return true;
  });
  if (host.empty() || path.empty()) return;
  float latr = j.lat * 3.14159265f / 180.0f;
  int zbest = -1; float tkm = 0;
  for (int z = 7; z >= 2; z--) {
    float tile_km = 40075.017f * cosf(latr) / (float) (1 << z);
    if (tile_km >= 2.0f * j.range) { zbest = z; tkm = tile_km; break; }
  }
  if (zbest < 0) { ESP_LOGW("radar_bg", "range too large"); return; }
  float n = (float) (1 << zbest);
  float xf = (j.lon + 180.0f) / 360.0f * n;
  float yf = (1.0f - asinhf(tanf(latr)) / 3.14159265f) / 2.0f * n;
  long x0 = (long) floorf(xf - 0.5f);
  long y0 = (long) floorf(yf - 0.5f);
  float bx = xf - x0, by = yf - y0;

  // 緩衝(PSRAM):離屏 456*456*3 + tile 暫存 512*512*4
  const int TW = 512, TH = 512;
  if (!g_echo_buf)
    g_echo_buf = (uint8_t *) heap_caps_malloc((size_t) RADAR_CANVAS * RADAR_CANVAS * 3, MALLOC_CAP_SPIRAM);
  static uint8_t *tmp = nullptr;
  if (!tmp) tmp = (uint8_t *) heap_caps_malloc((size_t) TW * TH * 4, MALLOC_CAP_SPIRAM);
  if (!g_echo_buf || !tmp) { ESP_LOGE("radar_bg", "echo buf alloc fail"); return; }

  memset(g_echo_buf, 0, (size_t) RADAR_CANVAS * RADAR_CANVAS * 3);   // 先清成透明(離屏,不影響畫面)
  echo_spans_clear();   // 逐列範圍跟著歸零,不然會留著上一幀的降雨區
  for (int k = 0; k < 4; k++) {
    char url[200];
    snprintf(url, sizeof(url), "%s%s/512/%d/%ld/%ld/2/1_1.png",
             host.c_str(), path.c_str(), zbest, x0 + k % 2, y0 + k / 2);
    echo_composite_tile(url, tmp, TW, TH, k % 2, k / 2, bx, by, tkm, j.range);
    vTaskDelay(pdMS_TO_TICKS(2500));   // 每塊間隔,分散 PSRAM/網路壓力
  }
  xSemaphoreTake(mtx(), portMAX_DELAY);
  g_echo_ready = true;   // 整幀就緒,等主迴圈 memcpy 換上
  xSemaphoreGive(mtx());
  ESP_LOGI("radar_bg", "echo frame ready z=%d tile=%.0fkm", zbest, tkm);
}

// ---- 地圖圖磚下載(job 8)-------------------------------------------------
// 圖磚在 flight-radar-maps 這個 repo,由 GitHub Pages 供應,10 度一格。
// 詳見 PLAN_MAPTILES.md 與該 repo 的 README。
inline const char MAPS_BASE[] =
#ifndef RADAR_MAPS_BASE
    "https://delphicchen.github.io/flight-radar-maps/v1"
#else
    RADAR_MAPS_BASE
#endif
    ;

inline volatile bool g_maps_ready = false;   // true=分割區有新地圖待主迴圈載入
inline volatile bool g_maps_busy = false;
// 給 UI 顯示進度用。分母只用「格數」而不是位元組總量 —— 每格大小不一、海上的格
// 子甚至不存在(404),總量要全部抓完才知道。先發 HEAD 問大小會讓請求數翻倍,
// 為了一個進度條不划算。格數則是一開始就算得出來的,不會出現假進度。
inline volatile int g_maps_progress = 0;     // 已成功寫入的圖磚數
inline volatile int g_maps_total = 0;        // 這一輪要抓的格數
inline volatile uint32_t g_maps_bytes = 0;   // 累計已寫入的位元組

// 半徑決定細節層:容差是照「約一個雷達像素」算的,所以半徑越小要越細的那一層。
inline int maps_level_for(float range_km) {
  if (range_km > 250.0f) return 1;
  if (range_km > 100.0f) return 2;
  return 3;
}

inline std::string maps_cell_name(int lat0, int lon0) {
  char b[8];
  snprintf(b, sizeof(b), "%c%02d%c%03d", lat0 >= 0 ? 'N' : 'S', abs(lat0),
           lon0 >= 0 ? 'E' : 'W', abs(lon0));
  return std::string(b);
}

inline int floor10(float v) {
  int f = (int) floorf(v / 10.0f) * 10;
  return f;
}

inline void do_maps(const Job &j) {
  g_maps_busy = true;
  g_maps_progress = 0;
  g_maps_total = 0;
  g_maps_bytes = 0;
  const int level = maps_level_for(j.range);
  const esp_partition_t *part = maptiles::partition();
  if (!part) {
    ESP_LOGE("radar_bg", "maps: no partition -- reflash over USB for the new table");
    g_maps_busy = false;
    return;
  }

  // 需要涵蓋的格子:以半徑換成度數往外擴,再對齊 10 度網格。
  // 經度每度的公里數隨緯度縮短,高緯度同樣的半徑會橫跨更多格。
  const float coslat = fmaxf(cosf(j.lat * 3.14159265f / 180.0f), 0.05f);
  const float dlat = j.range / 110.574f;
  const float dlon = j.range / (111.320f * coslat);
  std::vector<std::pair<int, int>> cells;
  for (int la = floor10(j.lat - dlat); la <= floor10(j.lat + dlat); la += 10)
    for (int lo = floor10(j.lon - dlon); lo <= floor10(j.lon + dlon); lo += 10) {
      if (la < -80 || la >= 80) continue;                  // 產生器不出極區
      int wlo = lo;
      while (wlo < -180) wlo += 360;                       // 跨換日線時繞回
      while (wlo >= 180) wlo -= 360;
      cells.push_back({la, wlo});
      if (cells.size() >= 12) break;                       // 保險上限
    }

  g_maps_total = (int) cells.size();
  if (!maptiles::store_begin(part, j.lat, j.lon, (uint16_t) j.range, (uint8_t) level,
                             part->size - sizeof(maptiles::StoreHeader))) {
    ESP_LOGE("radar_bg", "maps: erase/begin failed");
    g_maps_busy = false;
    return;
  }

  size_t at = 0;
  int n = 0, http_fail = 0;
  for (auto &c : cells) {
    std::string url = std::string(MAPS_BASE) + "/L" + std::to_string(level) + "/" +
                      maps_cell_name(c.first, c.second) + ".bin";
    int status = -1;
    std::string blob = http_req(url, false, "", nullptr, "", status, 40000);
    if (status == 404) continue;      // 海上的格子沒有檔案,是正常狀況不是錯誤
    if (status != 200) { http_fail++; continue; }
    // 三道關卡缺一不可。GitHub Pages 的 404 會回一頁 9KB 的 HTML,只看「有沒有
    // 拿到位元組」的話就會把網頁當成地圖寫進 flash。
    if (!maptiles::validate((const uint8_t *) blob.data(), blob.size())) {
      ESP_LOGW("radar_bg", "maps: %s failed validation (%d bytes)",
               url.c_str(), (int) blob.size());
      continue;
    }
    if (at + blob.size() + sizeof(maptiles::StoreHeader) > part->size) break;
    if (!maptiles::store_append(part, at, blob.data(), blob.size())) {
      ESP_LOGE("radar_bg", "maps: flash write failed");
      g_maps_busy = false;
      return;                          // complete 旗標沒補上,開機會當成沒地圖
    }
    at += blob.size();
    n++;
    g_maps_progress = n;
    g_maps_bytes = (uint32_t) at;
  }

  if (n == 0) {
    // 一格都沒拿到就不要蓋掉舊地圖 —— 分割區已經抹掉了,但不補 complete 旗標,
    // 讀取端會當成沒有地圖,下次連上網再重試。
    ESP_LOGW("radar_bg", "maps: nothing downloaded (%d http failures)", http_fail);
    g_maps_busy = false;
    return;
  }
  if (!maptiles::store_finalize(part, (uint8_t) n, (uint32_t) at)) {
    ESP_LOGE("radar_bg", "maps: finalize failed");
    g_maps_busy = false;
    return;
  }
  ESP_LOGI("radar_bg", "maps: stored %d tiles (%u bytes) L%d for %.3f,%.3f r=%.0fkm",
           n, (unsigned) at, level, j.lat, j.lon, j.range);
  xSemaphoreTake(mtx(), portMAX_DELAY);
  g_maps_ready = true;
  xSemaphoreGive(mtx());
  g_maps_busy = false;
}

inline void task_fn(void *arg) {
  for (;;) {
    Job *j = nullptr;
    if (xQueueReceive(queue(), &j, portMAX_DELAY) == pdTRUE && j != nullptr) {
      if (j->type == 1) do_states(*j);
      else if (j->type == 2) do_route(*j);
      else if (j->type == 6) do_route_cache(*j);
      else if (j->type == 7) do_type_cache(*j);
      else if (j->type == 3) do_echo(*j);
      else if (j->type == 4) do_weather(*j);
      else if (j->type == 5) do_speakers(*j);
      else if (j->type == 8) do_maps(*j);
      delete j;
    }
  }
}

inline void ensure_task() {
  static bool created = false;
  if (!created) {
    created = true;
    xTaskCreatePinnedToCore(task_fn, "radar_fetch", 12288, nullptr, 1, nullptr, 1);
  }
}

inline void request_states(const std::string &cid, const std::string &sec,
                           float lat, float lon, float range, int src) {
  ensure_task();
  Job *j = new Job{1, cid, sec, "", lat, lon, range, src};
  if (xQueueSend(queue(), &j, 0) != pdTRUE) delete j;
}

inline void request_echo(float lat, float lon, float range) {
  ensure_task();
  Job *j = new Job{3, "", "", "", lat, lon, range};
  if (xQueueSend(queue(), &j, 0) != pdTRUE) delete j;
}

inline void request_maps(float lat, float lon, float range) {
  if (g_maps_busy) return;            // 一次只跑一輪,避免重複下載
  ensure_task();
  Job *j = new Job{8, "", "", "", lat, lon, range};
  if (xQueueSend(queue(), &j, 0) != pdTRUE) delete j;
}

inline void request_weather(float lat, float lon) {
  ensure_task();
  Job *j = new Job{4, "", "", "", lat, lon, 0};
  if (xQueueSend(queue(), &j, 0) != pdTRUE) delete j;
}

inline void request_speakers(const std::string &url, const std::string &token) {
  ensure_task();
  Job *j = new Job{5, url, token, "", 0, 0, 0};
  if (xQueueSend(queue(), &j, 0) != pdTRUE) delete j;
}

inline void request_route(const std::string &cs) {
  if (cs.empty()) return;
  ensure_task();
  Job *j = new Job{2, "", "", cs, 0, 0, 0};
  if (xQueueSend(queue(), &j, 0) != pdTRUE) delete j;
}

// 查起訖站快取;未知就排一次背景查詢。回傳 "" = 還不知道,"-" = 查無資料。
// 佇列滿(深度 4)時清掉「查詢中」標記,下一幀重試,路線會分幾秒陸續補齊。
inline std::string route_cache_get(const std::string &cs) {
  if (cs.empty()) return "";
  ensure_task();
  xSemaphoreTake(mtx(), portMAX_DELAY);
  auto it = g_rcache.find(cs);
  if (it != g_rcache.end()) {
    std::string v = it->second;
    xSemaphoreGive(mtx());
    return v;
  }
  if (g_rcache.size() > 120) g_rcache.clear();   // 防跨日航班累積吃 PSRAM
  g_rcache[cs] = "";                             // 標記查詢中(去重)
  xSemaphoreGive(mtx());
  Job *j = new Job{6, "", "", cs, 0, 0, 0};
  if (xQueueSend(queue(), &j, 0) != pdTRUE) {
    delete j;
    xSemaphoreTake(mtx(), portMAX_DELAY);
    g_rcache.erase(cs);
    xSemaphoreGive(mtx());
  }
  return "";
}

// 查機型快取;未知就排一次背景查詢(只給 OpenSky 用,見 g_tcache)。
// 回傳 true 表示 ty/reg/desc 已填(可能是空字串 = 查無此機),false = 還不知道。
// 只在「使用者選中的那一架」呼叫,不要整個機隊掃 —— adsbdb 是免費 API,
// 一輪十幾架會撞上限流,而規格面板本來也只看得到一架。
inline bool type_cache_get(uint32_t hex, std::string &ty, std::string &reg,
                           std::string &desc) {
  if (hex == 0) return false;
  ensure_task();
  xSemaphoreTake(mtx(), portMAX_DELAY);
  auto it = g_tcache.find(hex);
  if (it != g_tcache.end()) {
    bool done = it->second.done;
    if (done) { ty = it->second.ty; reg = it->second.reg; desc = it->second.desc; }
    xSemaphoreGive(mtx());
    return done;
  }
  if (g_tcache.size() > 120) g_tcache.clear();   // 同 g_rcache:防長時間累積吃 PSRAM
  g_tcache[hex] = AcMeta{};                      // 標記查詢中(去重)
  xSemaphoreGive(mtx());
  char h[8];
  snprintf(h, sizeof(h), "%06X", (unsigned) hex);
  Job *j = new Job{7, "", "", h, 0, 0, 0};
  if (xQueueSend(queue(), &j, 0) != pdTRUE) {
    delete j;
    xSemaphoreTake(mtx(), portMAX_DELAY);
    g_tcache.erase(hex);   // 佇列滿:下一幀重試
    xSemaphoreGive(mtx());
  }
  return false;
}

// 選中那一架若沒有機型(= OpenSky 來源),用 ICAO24 補查並寫回這一輪的向量。
// 第一次呼叫只是排隊,回來前 ty 維持空字串(徽章仍隱藏),下一次刷新才補上。
inline void type_fill(std::string &ty, std::string &reg, std::string &desc, uint32_t hex) {
  if (!ty.empty() || hex == 0) return;
  std::string t, r, d;
  if (!type_cache_get(hex, t, r, d)) return;
  ty = t;
  if (reg.empty()) reg = r;
  if (desc.empty()) desc = d;
}

// ---- 時區:下拉選單 index → POSIX TZ 字串(供 time.set_timezone 執行期切換)----
// 顯示名清單在 ui/ 的 dropdown options,兩邊 index 必須對齊。
inline const char *tz_posix(int i) {
  static const char *const T[] = {
      "CST-8",                            // 0 Taipei
      "JST-9",                            // 1 Tokyo
      "CST-8",                            // 2 Shanghai
      "HKT-8",                            // 3 Hong Kong
      "KST-9",                            // 4 Seoul
      "ICT-7",                            // 5 Bangkok
      "GMT0BST,M3.5.0/1,M10.5.0",         // 6 London
      "CET-1CEST,M3.5.0,M10.5.0/3",       // 7 Paris
      "CET-1CEST,M3.5.0,M10.5.0/3",       // 8 Berlin
      "EST5EDT,M3.2.0,M11.1.0",           // 9 New York
      "PST8PDT,M3.2.0,M11.1.0",           // 10 Los Angeles
      "UTC0",                             // 11 UTC
  };
  const int n = sizeof(T) / sizeof(T[0]);
  return (i >= 0 && i < n) ? T[i] : T[0];
}

}  // namespace radar_bg

// ---- 底圖層:底色+輪廓(快取)+ 預混合回波 + 距離環/十字線 → 單一不透明 canvas ----
// 貴的輪廓重畫(逐段 draw context,~40ms)只在座標/範圍/MAP 開關變更時做並存入快取;
// 平時重建 = memcpy 快取 + 回波混色 + 畫環,~15ms。距離環/十字線內容不變但位於
// map/echo 之上,烤進底圖後每幀連這幾個 widget 的圓弧邊框繪製也省掉。
// alpha 混色只在資料更新時做這一次,之後每幀渲染對這層只剩不透明 blit(省 PSRAM 頻寬)。
// ATC 圖層 bitmask:bit0 空域 / bit1 跑道 / bit2 機場 / bit3 導航點;ATC 模式關閉 = 0
inline uint8_t atc_layer_mask(bool atc, bool asp, bool rwy, bool apt, bool fix) {
  return atc ? (uint8_t) ((asp ? 1 : 0) | (rwy ? 2 : 0) | (apt ? 4 : 0) | (fix ? 8 : 0)) : 0;
}

// 航班標記/向量/軌跡本來掛在整頁上,標籤會畫出雷達圓溢到下方控制列。
// 建一個與底圖對齊的圓形裁剪層,把它們掛進去;座標改用 RADAR_CX(層内原點)。
inline void radar_install_ac_clip(lv_obj_t *page, lv_obj_t *marks[], lv_obj_t *vecs[],
                                  lv_obj_t *trs[][6]) {
  static bool done = false;
  if (done || !page) return;
  lv_obj_t *clip = lv_obj_create(page);
  lv_obj_remove_style_all(clip);
  lv_obj_set_pos(clip, RS(12), RS(12));
  lv_obj_set_size(clip, RADAR_CANVAS, RADAR_CANVAS);
  lv_obj_set_style_bg_opa(clip, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(clip, 0, 0);
  lv_obj_set_style_pad_all(clip, 0, 0);
  lv_obj_set_style_radius(clip, RADAR_CANVAS / 2, 0);
  lv_obj_set_style_clip_corner(clip, true, 0);
  lv_obj_clear_flag(clip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(clip, LV_OBJ_FLAG_CLICKABLE);
  for (int i = 0; i < AC_SLOTS; i++) {
    if (marks[i]) lv_obj_set_parent(marks[i], clip);
    if (vecs[i]) lv_obj_set_parent(vecs[i], clip);
    for (int k = 0; k < 6; k++)
      if (trs[i][k]) lv_obj_set_parent(trs[i][k], clip);
  }
  done = true;
}

// ---- 掃描線/尾巴 ----
// ARGB 自繪在 MIPI 合成下易殘影、外觀差;改回 LVGL line(原扇形餘暉)。
// 角度用牆鐘錨點:斜線幀變貴時跳格跟上,避免「半圈明顯變慢」。
inline lv_obj_t *g_sweep_cv = nullptr;
inline uint32_t *g_sweep_px = nullptr;

inline float radar_sweep_tick(float period_ms, bool lit, float hold_angle,
                              lv_obj_t *page, lv_obj_t *base, lv_obj_t *lv_lines[5]) {
  static float a0 = 0.0f;
  static uint32_t t0 = 0;
  static float per = -1.0f;
  static bool anchored = false;
  uint32_t now = millis();
  if (!anchored) {
    a0 = hold_angle;
    t0 = now;
    per = period_ms;
    anchored = true;
  } else if (fabsf(period_ms - per) > 0.5f) {
    a0 = fmodf(a0 + (float) (now - t0) / per * 360.0f, 360.0f);
    if (a0 < 0.0f) a0 += 360.0f;
    t0 = now;
    per = period_ms;
  }
  float cur = fmodf(a0 + (float) (now - t0) / per * 360.0f, 360.0f);
  if (cur < 0.0f) cur += 360.0f;

  // 清掉舊版自繪 canvas(若還在),恢復 LVGL 線
  static bool cleaned = false;
  if (!cleaned) {
    if (g_sweep_cv) {
      lv_obj_del(g_sweep_cv);
      g_sweep_cv = nullptr;
    }
    if (g_sweep_px) {
      heap_caps_free(g_sweep_px);
      g_sweep_px = nullptr;
    }
    cleaned = true;
  }
  (void) page;
  (void) base;

  if (!lit || !lv_lines) return cur;

  const float DEG = 3.14159265f / 180.0f;
  static const float TAIL_DEG[5] = { 0.0f, 1.75f, 3.75f, 5.75f, 7.75f };
  static lv_point_precise_t lp[5][2];
  static bool lines_shown = false;
  for (int k = 0; k < 5; k++) {
    if (!lv_lines[k]) continue;
    if (!lines_shown) lv_obj_clear_flag(lv_lines[k], LV_OBJ_FLAG_HIDDEN);
    float a = (cur - TAIL_DEG[k]) * DEG;
    lp[k][0].x = RS(240);
    lp[k][0].y = RS(240);
    lp[k][1].x = RS(240) + (lv_coord_t) ((float) RADAR_R * sinf(a));
    lp[k][1].y = RS(240) - (lv_coord_t) ((float) RADAR_R * cosf(a));
    lv_line_set_points(lv_lines[k], lp[k], 2);
  }
  lines_shown = true;
  return cur;
}

namespace radar_bg {

// LVGL 9 把 canvas 的 lv_canvas_draw_* 全砍掉,改成「開一個 layer → 送 lv_draw_*
// 任務 → finish_layer 才真正繪製」。任務會一路排隊到 finish_layer,而底圖動輒
// 上千段線,全堆在佇列裡會吃掉大量 LVGL 堆積(每個任務都含一份 dsc 複本),
// 所以每累積 FLUSH_EVERY 個任務就收一次 layer 再開新的,把記憶體壓在常數級。
// 收 layer 也是「繪圖任務真正執行」的時機——任何指向區域變數的 dsc 內容
// (例如 points 陣列)都必須活到那時,故本檔一律只用 p1/p2 與靜態字串。
// 直接寫 canvas 的 RGB565 緩衝,完全繞開 LVGL 的繪圖任務。
// 動機:LVGL 9 的每一筆 lv_draw_* 都要配一個 draw task、複製一份 dsc、排隊,再等
// finish_layer 派送執行;LVGL 8 的 lv_canvas_draw_line 則是當場畫完。底圖的地圖
// 輪廓上千段、ATC 空域再上百段,全是 1px 純色線,管理成本遠大於畫線本身
//(實機上底圖重建的 lvgl 慢操作曾到 631~708ms)。這些線不需要抗鋸齒也不需要
// 混色,自己畫最省。文字與距離環仍走 LVGL(數量少,且要保留字形/圓弧的抗鋸齒)。
class PixCanvas {
 public:
  PixCanvas(uint16_t *buf, int w, int h) : buf_(buf), w_(w), h_(h) {}

  inline void px(int x, int y, uint16_t c) {
    if ((unsigned) x < (unsigned) w_ && (unsigned) y < (unsigned) h_)
      buf_[(size_t) y * w_ + x] = c;
  }

  // Cohen–Sutherland 裁切 + Bresenham。先裁切是必要的:地圖點投影後可能落在畫布
  // 外好幾萬像素,直接 Bresenham 會空跑一大堆迴圈。
  void line(int x0, int y0, int x1, int y1, uint16_t c) {
    if (!clip_(x0, y0, x1, y1)) return;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
      px(x0, y0, c);
      if (x0 == x1 && y0 == y1) break;
      int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }

  // 2px 線:沿著「較平的那個軸」再畫一條平移 1px 的線,視覺上與 LVGL 的 width:2 相當
  void line2(int x0, int y0, int x1, int y1, uint16_t c) {
    line(x0, y0, x1, y1, c);
    if (abs(x1 - x0) >= abs(y1 - y0))
      line(x0, y0 + 1, x1, y1 + 1, c);
    else
      line(x0 + 1, y0, x1 + 1, y1, c);
  }

  void fill_rect(int x, int y, int w, int h, uint16_t c) {
    for (int j = y; j < y + h; j++)
      for (int i = x; i < x + w; i++) px(i, j, c);
  }

 private:
  // 回傳 false = 整段在畫布外
  bool clip_(int &x0, int &y0, int &x1, int &y1) const {
    auto code = [this](int x, int y) {
      int c = 0;
      if (x < 0) c |= 1; else if (x >= w_) c |= 2;
      if (y < 0) c |= 4; else if (y >= h_) c |= 8;
      return c;
    };
    int c0 = code(x0, y0), c1 = code(x1, y1);
    for (int guard = 0; guard < 8; guard++) {
      if (!(c0 | c1)) return true;      // 兩端都在內
      if (c0 & c1) return false;        // 同一側外面
      int c = c0 ? c0 : c1;
      int x = 0, y = 0;
      // 用 double 算交點:座標可能很大,float 的精度不足以避免差一格
      double ddx = (double) x1 - x0, ddy = (double) y1 - y0;
      if (c & 8) {        y = h_ - 1; x = x0 + (int) (ddx * (y - y0) / ddy); }
      else if (c & 4) {   y = 0;      x = x0 + (int) (ddx * (y - y0) / ddy); }
      else if (c & 2) {   x = w_ - 1; y = y0 + (int) (ddy * (x - x0) / ddx); }
      else {              x = 0;      y = y0 + (int) (ddy * (x - x0) / ddx); }
      if (c == c0) { x0 = x; y0 = y; c0 = code(x0, y0); }
      else         { x1 = x; y1 = y; c1 = code(x1, y1); }
    }
    return false;
  }

  uint16_t *buf_;
  int w_, h_;
};

class CanvasPainter {
 public:
  explicit CanvasPainter(lv_obj_t *canvas) : cv_(canvas) { lv_canvas_init_layer(cv_, &layer_); }
  ~CanvasPainter() { lv_canvas_finish_layer(cv_, &layer_); }
  CanvasPainter(const CanvasPainter &) = delete;
  CanvasPainter &operator=(const CanvasPainter &) = delete;
  lv_layer_t *layer() { return &layer_; }
  void tick() {
    if (++pending_ >= FLUSH_EVERY) {
      lv_canvas_finish_layer(cv_, &layer_);
      lv_canvas_init_layer(cv_, &layer_);
      pending_ = 0;
    }
  }

 private:
  static constexpr int FLUSH_EVERY = 128;
  lv_obj_t *cv_;
  lv_layer_t layer_;
  int pending_ = 0;
};

}  // namespace radar_bg

inline void radar_rebuild_base(lv_obj_t *cv, float lat0, float lon0, float rng,
                               bool map_show, bool echo_show, uint8_t atc_layers) {
  lv_draw_buf_t *db = lv_canvas_get_draw_buf(cv);   // LVGL 9:canvas 緩衝改由 draw_buf 描述
  if (!db || !db->data) return;
  // MIPI-DSI + LVGL 180°(PPA)下,重建若:
  //   • 長時間關 invalidation(整 UI 卡住後一次全屏 flush),或
  //   • 把 canvas HIDDEN 再顯示(髒區變大、PPA 整幀旋轉),
  // 偶發整屏青/色塊閃一下(PSRAM 與 DSI/PPA 搶頻寬時更明顯)。
  // 作法:輪廓先畫進離屏 cache(不碰顯示);僅在「拷貝+回波+環/ATC」這段短
  // 臨界區關 invalidation,且絕不 HIDDEN——面板暫留上一幀已 flush 的畫面。
  lv_display_t *disp = lv_obj_get_display(cv);
  // canvas 建成 LV_COLOR_FORMAT_NATIVE(= RGB565,2 bytes/px);LVGL 9 的
  // lv_color_t 是 24-bit,不能再拿來當畫布像素型別,直接用 uint16_t。
  // ESPHome 設 LV_DRAW_BUF_STRIDE_ALIGN=1,所以 stride 就是寬 x 2、可平坦定址。
  uint16_t *px = (uint16_t *) (void *) db->data;
  const size_t NPX = (size_t) RADAR_CANVAS * RADAR_CANVAS;
  const size_t BYTES = NPX * sizeof(uint16_t);
  // 地圖資料以前是 map_data.h 裡的編譯期陣列,現在是 maptiles 從 maps 分割區
  // 載入的 vector。這幾個別名讓底下的繪製迴圈完全不用改寫。
  // 取 .data() 是安全的:圖磚只在載入時 append,繪製期間不會再動到 vector。
  const float *const MAP_OUTLINE = maptiles::OUTLINE.data();
  const int MAP_OUTLINE_LEN = (int) maptiles::OUTLINE.size();
  const MapAirport *const AIRPORTS = maptiles::AIRPORTS.data();
  const int AIRPORTS_LEN = (int) maptiles::AIRPORTS.size();
  const MapRunway *const RUNWAYS = maptiles::RUNWAYS.data();
  const int RUNWAYS_LEN = (int) maptiles::RUNWAYS.size();
  const MapFix *const FIXES = maptiles::FIXES.data();
  const int FIXES_LEN = (int) maptiles::FIXES.size();
  const MapAirspace *const AIRSPACES = maptiles::AIRSPACES.data();
  const int AIRSPACES_LEN = (int) maptiles::AIRSPACES.size();
  const float *const AIRSPACE_PTS = maptiles::AIRSPACE_PTS.data();
  static uint8_t *cache = nullptr;   // 底色+輪廓快取(PSRAM,416KB)
  if (!cache) cache = (uint8_t *) heap_caps_malloc(BYTES, MALLOC_CAP_SPIRAM);
  static float c_lat = NAN, c_lon = NAN, c_rng = NAN;
  static bool c_map = false;
  static uint32_t c_gen = 0;
  // c_gen 不能省:座標/半徑/開關都沒變,但地圖下載完成時資料換了一整套,
  // 沒有它畫面會停在舊的(或空的)底圖,直到使用者剛好去動座標才更新。
  bool fresh = cache && c_lat == lat0 && c_lon == lon0 && c_rng == rng &&
               c_map == map_show && c_gen == maptiles::generation;
  // Phase 1:只動離屏 cache。無 cache 時只能直接畫 px,臨界區會變長。
  if (!fresh) {
    uint16_t *build = cache ? (uint16_t *) (void *) cache : px;
    // 無離屏緩衝時必須鎖住顯示,否則半成品會被 flush 出去。
    if (!cache && disp) lv_display_enable_invalidation(disp, false);
    radar_bg::PixCanvas pc_build(build, RADAR_CANVAS, RADAR_CANVAS);
    // 不要用 lv_canvas_fill_bg:它逐像素呼叫 lv_canvas_set_px,每次都重算 offset。
    // 1024x600 的 RGB 面板 GDMA 同時在吃 PSRAM 頻寬,兩者相撞會慢到每像素
    // ~230us,開機直接被 task watchdog 打死。canvas 無 alpha 且此處為不透明
    // 填滿,直接對緩衝區做 16-bit 填充(LVGL 9 已無 lv_color_fill,自己寫迴圈)。
    const uint16_t bg = lv_color_to_u16(lv_color_hex(0x040C08));
    for (size_t i = 0; i < NPX; i++) build[i] = bg;
    if (map_show) {
      float coslat = cosf(lat0 * 3.14159265f / 180.0f);
      // 輪廓分層(outline NaN,kind):
      //   0 海岸線  1 國界  2 省/州界  3 市界  4 河流  5 公路  6 鐵路
      // 官方 CDN 舊圖磚通常只有 0/1;省/市界要自建帶 --states/--cities 的圖磚。
      static const uint32_t MAP_KIND_COLOR[7] = {
          0xD8C878,  // coast
          0x9A8B54,  // country
          0x685E38,  // province / state
          0x3A3A3A,  // city / admin_2(淡灰,低對比)
          0x3A6E8A,  // river
          0x5A5A5A,  // road
          0x4A3A5A,  // rail
      };
      uint16_t col = lv_color_to_u16(lv_color_hex(MAP_KIND_COLOR[0]));
      float r2 = rng * rng;
      bool have_prev = false;
      bool skip_kind = false;   // 預設畫到市界;河/路/鐵太密略過
      float pe = 0.f, pn = 0.f;
      // 線段裁進雷達圓:先前「一端在圓內就整段畫」會讓線條穿出距離環到畫布四角。
      auto clip_and_draw = [&](float e0, float n0, float e1, float n1) {
        const float d0 = e0 * e0 + n0 * n0, d1 = e1 * e1 + n1 * n1;
        const bool in0 = d0 <= r2, in1 = d1 <= r2;
        float ce0, cn0, ce1, cn1;
        if (in0 && in1) {
          ce0 = e0; cn0 = n0; ce1 = e1; cn1 = n1;
        } else {
          const float de = e1 - e0, dn = n1 - n0;
          const float a = de * de + dn * dn;
          if (a < 1e-12f) return;
          const float b = 2.f * (e0 * de + n0 * dn);
          const float c = d0 - r2;
          const float disc = b * b - 4.f * a * c;
          if (disc < 0.f) return;
          const float s = sqrtf(disc);
          const float inv = 0.5f / a;
          float u0 = (-b - s) * inv, u1 = (-b + s) * inv;
          if (u0 > u1) { float t = u0; u0 = u1; u1 = t; }
          float lo, hi;
          if (in0 && !in1) {
            lo = 0.f;
            hi = (u0 > 0.f && u0 < 1.f) ? u0 : u1;
            if (hi < 0.f || hi > 1.f) return;
          } else if (!in0 && in1) {
            lo = (u0 > 0.f && u0 < 1.f) ? u0 : u1;
            hi = 1.f;
            if (lo < 0.f || lo > 1.f) return;
          } else {
            lo = u0 > 0.f ? u0 : 0.f;
            hi = u1 < 1.f ? u1 : 1.f;
            if (lo >= hi) return;
            const float tm = 0.5f * (lo + hi);
            const float em = e0 + tm * de, nm = n0 + tm * dn;
            if (em * em + nm * nm > r2) return;
          }
          ce0 = e0 + lo * de; cn0 = n0 + lo * dn;
          ce1 = e0 + hi * de; cn1 = n0 + hi * dn;
        }
        const float s = (float) RADAR_R / rng;
        pc_build.line((int) (RADAR_CX + ce0 * s), (int) (RADAR_CX - cn0 * s),
                      (int) (RADAR_CX + ce1 * s), (int) (RADAR_CX - cn1 * s), col);
      };
      for (int i = 0; i + 1 < MAP_OUTLINE_LEN; i += 2) {
        float la = MAP_OUTLINE[i], lo = MAP_OUTLINE[i + 1];
        if (isnan(la)) {
          have_prev = false;
          uint8_t kind = isnan(lo) ? 0 : (uint8_t) lo;
          if (kind > 6) kind = 2;
          skip_kind = (kind > 3);   // 4 河 5 路 6 鐵 — 略過
          if (!skip_kind)
            col = lv_color_to_u16(lv_color_hex(MAP_KIND_COLOR[kind]));
          continue;
        }
        if (skip_kind) continue;
        float e = (lo - lon0) * 111.320f * coslat;
        float n = (la - lat0) * 110.574f;
        if (have_prev)
          clip_and_draw(pe, pn, e, n);
        pe = e;
        pn = n;
        have_prev = true;
      }
    }
    if (cache) {
      c_lat = lat0; c_lon = lon0; c_rng = rng; c_map = map_show;
      c_gen = maptiles::generation;
    }
  }
  // Phase 2:短臨界區——一次拷貝可見緩衝,再疊回波/環/ATC,最後一次 invalidate。
  if (disp) lv_display_enable_invalidation(disp, false);
  if (cache) memcpy((void *) px, cache, BYTES);
  radar_bg::PixCanvas pc(px, RADAR_CANVAS, RADAR_CANVAS);   // 線條走這條路,不進 LVGL 佇列
  // 回波預混合:g_echo_buf 為 [color_lo][color_hi][alpha],逐像素混進底圖。
  // 只走 g_echo_x0/x1 記下的逐列範圍——降雨通常只佔畫面一小塊,沒下雨時整張
  // 全空,這樣就從「必掃 324,900 像素、讀 950KB PSRAM」變成只碰真的有資料的
  // 區段。底圖重建每次都做這一步(不進快取),所以省下的是每次的固定成本。
  // LVGL 9 的 lv_color_mix 走 24-bit,這裡改用 RGB565 專用的 lv_color_16_16_mix
  //(標了 LV_ATTRIBUTE_FAST_MEM,與舊版一樣是 IRAM 內的快速路徑)
  if (echo_show && radar_bg::g_echo_buf) {
    for (int y = 0; y < RADAR_CANVAS; y++) {
      const int xa = radar_bg::g_echo_x0[y], xb = radar_bg::g_echo_x1[y];
      if (xa > xb) continue;   // 整列無降雨:一次 PSRAM 讀取都不必
      const uint8_t *sp = radar_bg::g_echo_buf + ((size_t) y * RADAR_CANVAS + xa) * 3;
      uint16_t *dst = px + (size_t) y * RADAR_CANVAS + xa;
      for (int x = xa; x <= xb; x++, sp += 3, dst++) {
        if (sp[2] < 8) continue;   // 區段內的空隙,保留底圖
        uint16_t fg = (uint16_t) sp[0] | ((uint16_t) sp[1] << 8);
        *dst = lv_color_16_16_mix(fg, *dst, sp[2]);
      }
    }
  }
  // 十字線與 4 圈距離環:蓋在回波上,顏色/位置與原 widget 版一致。
  // 十字線是正交直線,自己畫與 LVGL 畫的結果完全相同;距離環仍交給 LVGL,
  // 圓弧的抗鋸齒自己補不划算,而且只有 4 個任務、成本可以忽略。
  pc.line(0, RADAR_CX, RADAR_CANVAS, RADAR_CX, lv_color_to_u16(lv_color_hex(0x003820)));
  pc.line(RADAR_CX, 0, RADAR_CX, RADAR_CANVAS, lv_color_to_u16(lv_color_hex(0x003820)));
  {
    // 這個 scope 很重要:離開時 finish_layer 會把距離環真的畫進緩衝,之後
    // 底下 ATC 圖層的直接寫入才會蓋在環之上——與改版前的疊圖順序一致。
    radar_bg::CanvasPainter pt(cv);
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.color = lv_color_hex(0x006030);
    ad.width = 1;
    ad.center = {RADAR_CX, RADAR_CX};
    ad.start_angle = 0;
    ad.end_angle = 360;
    const lv_coord_t RINGS[4] = {RADAR_CX, (lv_coord_t)(RADAR_CX*3/4), (lv_coord_t)(RADAR_CX/2), (lv_coord_t)(RADAR_CX/4)};
    for (int k = 0; k < 4; k++) {
      ad.radius = (uint16_t) RINGS[k];
      lv_draw_arc(pt.layer(), &ad);
    }
  }
  // ---- ATC 靜態圖層(僅 ATC 模式,依 bitmask 逐層開關):空域/跑道+延伸線/機場/導航點 ----
  // 跟距離環一樣每次重建時畫、不進快取;重建只在切換或座標變更時發生,成本無感
  if (atc_layers) {
    float coslat = cosf(lat0 * 3.14159265f / 180.0f);
    float r2 = rng * rng;
    auto prj = [&](float la, float lo, float &e, float &n, lv_point_t &p) {
      e = (lo - lon0) * 111.320f * coslat;
      n = (la - lat0) * 110.574f;
      p.x = (lv_coord_t) (RADAR_CX + e / rng * (float) RADAR_R);
      p.y = (lv_coord_t) (RADAR_CX - n / rng * (float) RADAR_R);
    };
    // 空域邊界(最底):TMA/CTA 暗藍、CTR 類亮一階
    if (atc_layers & 1) {
    for (int a = 0; a < AIRSPACES_LEN; a++) {
      const MapAirspace &as = AIRSPACES[a];
      uint16_t col = lv_color_to_u16(lv_color_hex(as.cls == 0 ? 0x3A7A9A : 0x2A5070));
      bool hp = false;
      lv_point_t prev{0, 0};
      float pd2 = 1e18f;
      for (int k = 0; k < as.npts; k++) {
        float e, n;
        lv_point_t p;
        prj(AIRSPACE_PTS[as.off + 2 * k], AIRSPACE_PTS[as.off + 2 * k + 1], e, n, p);
        float d2 = e * e + n * n;
        if (hp && (d2 <= r2 || pd2 <= r2))
          pc.line(prev.x, prev.y, p.x, p.y, col);
        prev = p; pd2 = d2; hp = true;
      }
    }
    }
    // 跑道延伸中線(虛線)+ 跑道本體(亮實線)
    if (atc_layers & 2) {
    const uint16_t ex_col = lv_color_to_u16(lv_color_hex(0x4A7A8A));   // 延伸中線
    const uint16_t rw_col = lv_color_to_u16(lv_color_hex(0xD0E4EE));   // 跑道本體
    for (int i = 0; i < RUNWAYS_LEN; i++) {
      const MapRunway &r = RUNWAYS[i];
      float e1, n1, e2, n2;
      lv_point_t p1, p2, x1, x2;
      prj(r.xlat1, r.xlon1, e1, n1, x1);
      prj(r.xlat2, r.xlon2, e2, n2, x2);
      if (e1 * e1 + n1 * n1 > r2 && e2 * e2 + n2 * n2 > r2) continue;
      // 虛線一律手動切段(5px 畫 4px 空):LVGL sw 渲染器的 dash 不支援斜線,
      // 而且切出來的每一小段本來就是直接寫像素最划算。
      float ddx = (float) (x2.x - x1.x), ddy = (float) (x2.y - x1.y);
      float dl = sqrtf(ddx * ddx + ddy * ddy);
      if (dl >= 2.0f) {
        for (float s = 0; s < dl; s += 9.0f) {
          float t0 = s / dl, t1 = (s + 5.0f) / dl;
          if (t1 > 1.0f) t1 = 1.0f;
          pc.line((int) (x1.x + ddx * t0), (int) (x1.y + ddy * t0),
                  (int) (x1.x + ddx * t1), (int) (x1.y + ddy * t1), ex_col);
        }
      }
      prj(r.lat1, r.lon1, e1, n1, p1);
      prj(r.lat2, r.lon2, e2, n2, p2);
      pc.line2(p1.x, p1.y, p2.x, p2.y, rw_col);   // 跑道是 2px 亮線
    }
    }
    // 導航點:小空心三角 + 名稱(暗色,避免壓過航機)
    // 圖形自己畫,文字仍走 LVGL(字形點陣要抗鋸齒與字型度量,自己補不划算)。
    // 兩者分開兩趟:圖形先全部寫進緩衝,再開一個 layer 一次把所有標籤畫上去。
    if (atc_layers & 8) {
    const uint16_t fx_col = lv_color_to_u16(lv_color_hex(0x4A7A5A));
    for (int i = 0; i < FIXES_LEN; i++) {
      float e, n;
      lv_point_t p;
      prj(FIXES[i].lat, FIXES[i].lon, e, n, p);
      if (e * e + n * n > r2) continue;
      // 小空心三角
      pc.line(p.x, p.y - 3, p.x + 3, p.y + 3, fx_col);
      pc.line(p.x + 3, p.y + 3, p.x - 3, p.y + 3, fx_col);
      pc.line(p.x - 3, p.y + 3, p.x, p.y - 3, fx_col);
    }
    radar_bg::CanvasPainter pt(cv);
    lv_draw_label_dsc_t fld;
    lv_draw_label_dsc_init(&fld);
    fld.font = lv_font_get_default();   // LVGL 9 改名(舊:lv_font_default)
    fld.color = lv_color_hex(0x50805E);
    for (int i = 0; i < FIXES_LEN; i++) {
      float e, n;
      lv_point_t p;
      prj(FIXES[i].lat, FIXES[i].lon, e, n, p);
      if (e * e + n * n > r2) continue;
      fld.text = FIXES[i].name;   // 靜態字串,撐得到繪圖任務派送
      lv_area_t fa = {p.x + 5, p.y - 6, p.x + 5 + 60 - 1, p.y - 6 + 40};
      lv_draw_label(pt.layer(), &fld, &fa);
      pt.tick();
    }
    }
    // 機場:實心小方塊 + ICAO 代碼(最上層)
    if (atc_layers & 4) {
    const uint16_t ap_col = lv_color_to_u16(lv_color_hex(0xE0ECF4));
    for (int i = 0; i < AIRPORTS_LEN; i++) {
      float e, n;
      lv_point_t p;
      prj(AIRPORTS[i].lat, AIRPORTS[i].lon, e, n, p);
      if (e * e + n * n > r2) continue;
      pc.fill_rect(p.x - 2, p.y - 2, 5, 5, ap_col);   // 實心小方塊
    }
    radar_bg::CanvasPainter pt(cv);
    lv_draw_label_dsc_t ald;
    lv_draw_label_dsc_init(&ald);
    ald.font = lv_font_get_default();
    ald.color = lv_color_hex(0x9AC8E0);
    for (int i = 0; i < AIRPORTS_LEN; i++) {
      float e, n;
      lv_point_t p;
      prj(AIRPORTS[i].lat, AIRPORTS[i].lon, e, n, p);
      if (e * e + n * n > r2) continue;
      ald.text = AIRPORTS[i].icao;
      lv_area_t aa = {p.x + 5, p.y - 14, p.x + 5 + 60 - 1, p.y - 14 + 40};
      lv_draw_label(pt.layer(), &ald, &aa);
      pt.tick();
    }
    }
  }
  if (disp) lv_display_enable_invalidation(disp, true);
  lv_obj_invalidate(cv);
}

// ---- 三指下滑截圖:抓 RGB 面板 framebuffer → BMP,經 HTTP(:8081)供 HA downloader 下載 ----
// framebuffer 裡的 RGB565 是不是位元組交換過的,取決於 LVGL 的 byte_order:
//   S3 板:LVGL 預設 big_endian(LV_COLOR_16_SWAP=1)→ 要交換 → 1
//   P4  :板檔把 display 與 lvgl 都設 little_endian → 原生序 → 用 build_flags 設 0
// (實測:P4 上開著交換時,背景 0x040C08 的 RGB565 0x0061 被讀成 0x6100,
//  深綠變成深橘褐 (96,32,0)。)
#ifndef SHOT_SWAP_BYTES
#define SHOT_SWAP_BYTES 1
#endif
// 截圖是否要轉 180°。LVGL 的軟體旋轉是「先把 UI 轉好再送進 framebuffer」,面板
// 再以相差 180° 的方向掃出來,所以使用者看到的是正的、framebuffer 裡存的是顛倒的。
// 直接讀回來會得到顛倒的畫面,需要轉回來。跟著板檔的 lvgl: rotation: 一起設。
#ifndef RADAR_SHOT_ROT180
#define RADAR_SHOT_ROT180 0
#endif
inline uint8_t *g_shot_buf = nullptr;        // 800*480*2 快照(PSRAM,首次截圖才配置)
inline char g_shot_path[40] = "";            // 寫進 SD 的檔名(不含目錄);空=沒寫到卡
                                             // 宣告在 RADAR_DISPLAY_RGB 守衛外:截圖停用的
                                             // 板子(P4 DSI)其狀態列 lambda 仍會讀這個變數
inline volatile bool g_shot_valid = false;

#if RADAR_DISPLAY_RGB
// 三個驅動的 panel handle 都叫 handle_ 且都是 protected,只有類別不同。
#if RADAR_DISPLAY_RGB == 1
using RadarRgbDisplay = esphome::rpi_dpi_rgb::RpiDpiRgb;
#elif RADAR_DISPLAY_RGB == 3
using RadarRgbDisplay = esphome::mipi_dsi::MipiDsi;
#else
using RadarRgbDisplay = esphome::mipi_rgb::MipiRgb;
#endif
// esp_lcd panel handle 在 ESPHome 元件裡是 protected,用衍生類取用(單 FB,拿到即當前畫面)
struct RpiSpy : public RadarRgbDisplay {
  static esp_lcd_panel_handle_t handle(RadarRgbDisplay *d) {
    return static_cast<RpiSpy *>(d)->handle_;
  }
};

// 24-bit BMP 標頭(row 3*W 對 800/1024 都是 4 的倍數,免 padding)。HTTP 與 SD 共用。
// 標頭墊到 512 B(一個磁區)而不是緊湊的 54 B。BMP 的 bfOffBits 欄位本來就允許
// 像素資料放在任意偏移,所有讀取器都會遵守,所以這是合規做法而非 hack。
// 為什麼要這樣:像素資料若從第 54 B 開始,後面每一批寫入都跨在 512 B 磁區邊界上,
// FATFS 只能對每個磁區做「讀-改-寫」,無法把整磁區直接串流給卡片。實機量到
// 1.8MB 花 8.2 秒(約 225 KB/s),對 4-bit 40MHz 的 SDMMC 慢得離譜。
// 對齊之後每批 48KB 剛好是 96 個完整磁區。
#define SHOT_HDR_BYTES 512
inline void shot_bmp_header(uint8_t hdr[SHOT_HDR_BYTES]) {
  memset(hdr, 0, SHOT_HDR_BYTES);
  uint32_t img = (uint32_t) SCREEN_W * SCREEN_H * 3, sz = SHOT_HDR_BYTES + img,
           off = SHOT_HDR_BYTES, ihsz = 40;
  int32_t w = SCREEN_W, h = SCREEN_H;
  uint16_t planes = 1, bpp = 24;
  hdr[0] = 'B'; hdr[1] = 'M';
  memcpy(hdr + 2, &sz, 4);  memcpy(hdr + 10, &off, 4);  memcpy(hdr + 14, &ihsz, 4);
  memcpy(hdr + 18, &w, 4);  memcpy(hdr + 22, &h, 4);
  memcpy(hdr + 26, &planes, 2);  memcpy(hdr + 28, &bpp, 2);  memcpy(hdr + 34, &img, 4);
}

// 把 g_shot_buf 的第 y 列(RGB565)轉成 BMP 的一列(24-bit BGR)。
inline void shot_bmp_row(int y, uint8_t *row) {
  // 呼叫端從 SCREEN_H-1 往下數(BMP 由下往上存)。ROT180 時把來源的列與欄都反向,
  // 合起來就是 180° 旋轉;#if 是編譯期的,內圈不會多出分支。
#if RADAR_SHOT_ROT180
  const uint16_t *src = (const uint16_t *) g_shot_buf + (size_t) (SCREEN_H - 1 - y) * SCREEN_W;
#else
  const uint16_t *src = (const uint16_t *) g_shot_buf + (size_t) y * SCREEN_W;
#endif
  for (int x = 0; x < SCREEN_W; x++) {
#if RADAR_SHOT_ROT180
    uint16_t v = src[SCREEN_W - 1 - x];
#else
    uint16_t v = src[x];
#endif
#if SHOT_SWAP_BYTES
    v = (uint16_t) ((v >> 8) | (v << 8));
#endif
    row[x * 3 + 0] = (uint8_t) ((v & 0x1F) << 3);          // B
    row[x * 3 + 1] = (uint8_t) (((v >> 5) & 0x3F) << 2);   // G
    row[x * 3 + 2] = (uint8_t) (((v >> 11) & 0x1F) << 3);  // R
  }
}

// ---- microSD(SPI):三指截圖另存一份到 TF 卡 ----
// 檔名 shot_YYYYMMDD_HHMMSS.bmp;SNTP 還沒對到時間就退回流水號。
// 掛載只嘗試一次(g_sd_tried),失敗就永遠走 HTTP 那條路,不每次重試拖慢截圖。
#if RADAR_SD_EXT
// 卡已經由 ESPHome 的 sd_storage 元件掛在 /sdcard(P4 走這條),我們不碰掛載。
// 沒插卡時元件的 setup() 會 mark_failed,這裡的 fopen 自然失敗 → 退回 HTTP 那條路,
// 行為和 SPI 模式一致。
inline bool sd_mount() { return true; }
#elif RADAR_SD_SPI
inline bool g_sd_ready = false;
inline bool g_sd_tried = false;

inline bool sd_mount() {
  if (g_sd_tried) return g_sd_ready;
  g_sd_tried = true;
  spi_bus_config_t bus = {};
  bus.mosi_io_num = RADAR_SD_MOSI;
  bus.miso_io_num = RADAR_SD_MISO;
  bus.sclk_io_num = RADAR_SD_CLK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 4096;
  esp_err_t e = spi_bus_initialize((spi_host_device_t) RADAR_SD_HOST, &bus, SPI_DMA_CH_AUTO);
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {   // INVALID_STATE = 這條 bus 已被初始化過
    ESP_LOGW("radar_sd", "spi_bus_initialize: %s", esp_err_to_name(e));
    return false;
  }
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = RADAR_SD_HOST;
  sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
  dev.host_id = (spi_host_device_t) RADAR_SD_HOST;
  dev.gpio_cs = SDSPI_SLOT_NO_CS;    // CS 在 CH422G EXIO4,由板檔的 switch 常拉低
  esp_vfs_fat_sdmmc_mount_config_t mnt = {};
  mnt.format_if_mount_failed = false;   // 不要動使用者的卡
  mnt.max_files = 2;
  mnt.allocation_unit_size = 16 * 1024;
  sdmmc_card_t *card = nullptr;
  e = esp_vfs_fat_sdspi_mount("/sdcard", &host, &dev, &mnt, &card);
  if (e != ESP_OK) {
    ESP_LOGW("radar_sd", "mount failed: %s (no card?)", esp_err_to_name(e));
    return false;
  }
  ESP_LOGI("radar_sd", "mounted %lluMB", ((uint64_t) card->csd.capacity * card->csd.sector_size) >> 20);
  g_sd_ready = true;
  return true;
}
#endif  // RADAR_SD_EXT / RADAR_SD_SPI

#if RADAR_SD_ANY
// 寫檔這半段兩種掛載模式完全共用:掛好之後就只是標準 POSIX。
inline bool sd_save_shot() {
  g_shot_path[0] = 0;
  if (!sd_mount()) return false;
  char path[64];
  time_t t = ::time(nullptr);   // ::  = 避開 esphome::time 命名空間
  struct tm tv;
  localtime_r(&t, &tv);
  if (tv.tm_year + 1900 >= 2024) {
    snprintf(path, sizeof(path), "/sdcard/shot_%04d%02d%02d_%02d%02d%02d.bmp",
             tv.tm_year + 1900, tv.tm_mon + 1, tv.tm_mday, tv.tm_hour, tv.tm_min, tv.tm_sec);
  } else {
    static int seq = 0;    // 時間還沒同步
    snprintf(path, sizeof(path), "/sdcard/shot_%03d.bmp", ++seq);
  }
  FILE *f = fopen(path, "wb");
  if (!f) { ESP_LOGW("radar_sd", "fopen %s failed", path); return false; }
  uint8_t hdr[SHOT_HDR_BYTES];
  shot_bmp_header(hdr);
  bool ok = fwrite(hdr, 1, SHOT_HDR_BYTES, f) == SHOT_HDR_BYTES;
  // 一次湊滿 CHUNK_ROWS 列再寫。原本一列一列寫(1024x600 = 600 次 × 3KB)在 P4 上
  // 慢到把主迴圈卡死、觸發 task watchdog 重開機:整張圖是 1.8MB,而每次 fwrite 都
  // 要穿過 FATFS 的 512B 磁區快取。加大到 48KB 一批,交易次數少 16 倍。
  // 這仍然是主迴圈裡的同步阻塞操作(截圖是使用者主動觸發、可以接受頓一下),
  // 所以每批之後餵一次看門狗——不然寫得再快也只是把爆掉的門檻往後推。
  const int CHUNK_ROWS = 16;
  const size_t ROW_BYTES = (size_t) SCREEN_W * 3;
  uint8_t *buf = (uint8_t *) heap_caps_malloc(ROW_BYTES * CHUNK_ROWS, MALLOC_CAP_SPIRAM);
  if (buf == nullptr) buf = (uint8_t *) malloc(ROW_BYTES * CHUNK_ROWS);
  if (buf == nullptr) {
    ok = false;
  } else {
    int n = 0;                                    // 緩衝內已累積的列數
    for (int y = SCREEN_H - 1; ok && y >= 0; y--) {
      shot_bmp_row(y, buf + (size_t) n * ROW_BYTES);
      if (++n == CHUNK_ROWS || y == 0) {
        ok = fwrite(buf, 1, ROW_BYTES * n, f) == ROW_BYTES * n;
        n = 0;
        esphome::App.feed_wdt();
      }
    }
    free(buf);
  }
  fclose(f);
  esphome::App.feed_wdt();                        // fclose 會 flush + 更新目錄項目
  if (!ok) { remove(path); ESP_LOGW("radar_sd", "write failed (card full?)"); return false; }
  snprintf(g_shot_path, sizeof(g_shot_path), "%s", strrchr(path, '/') + 1);
  ESP_LOGI("radar_sd", "saved %s", path);
  return true;
}
#else
inline bool sd_save_shot() { return false; }
#endif  // RADAR_SD_ANY

inline esp_err_t shot_http_get(httpd_req_t *req) {
  if (!g_shot_valid || !g_shot_buf) { httpd_resp_send_404(req); return ESP_OK; }
  const int H = SCREEN_H;
  uint8_t hdr[SHOT_HDR_BYTES];
  shot_bmp_header(hdr);
  httpd_resp_set_type(req, "image/bmp");
  httpd_resp_send_chunk(req, (const char *) hdr, SHOT_HDR_BYTES);
  static uint8_t row[SCREEN_W * 3];
  for (int y = H - 1; y >= 0; y--) {         // BMP 由下往上
    shot_bmp_row(y, row);
    if (httpd_resp_send_chunk(req, (const char *) row, sizeof(row)) != ESP_OK)
      return ESP_FAIL;
  }
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

inline bool screenshot_capture(RadarRgbDisplay *disp) {
  const size_t BYTES = (size_t) SCREEN_W * SCREEN_H * 2;
  if (!g_shot_buf) g_shot_buf = (uint8_t *) heap_caps_malloc(BYTES, MALLOC_CAP_SPIRAM);
  if (!g_shot_buf) return false;
  void *fb = nullptr;
#if RADAR_DISPLAY_RGB == 3
  // MIPI-DSI:面板由 esp_lcd_new_panel_dpi() 建立(num_fbs = 1),像素經
  // esp_lcd_panel_draw_bitmap() 寫進那份 DPI framebuffer,所以讀回來就是當前畫面。
  // 板檔的 rotation: 180° 是 LVGL 的軟體旋轉,framebuffer 裡已經是轉正後的內容。
  if (esp_lcd_dpi_panel_get_frame_buffer(RpiSpy::handle(disp), 1, &fb) != ESP_OK || !fb)
    return false;
#else
  if (esp_lcd_rgb_panel_get_frame_buffer(RpiSpy::handle(disp), 1, &fb) != ESP_OK || !fb)
    return false;
#endif
  g_shot_valid = false;                      // 服務端讀到一半時避免撕裂判定
  memcpy(g_shot_buf, fb, BYTES);
  g_shot_valid = true;
  sd_save_shot();                            // 有卡就另存一份;失敗不影響 HTTP 這條路
  static httpd_handle_t srv = nullptr;       // 首次截圖才啟動 HTTP 服務
  if (!srv) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 8081;
    cfg.ctrl_port = 32780;                   // 避開 ESPHome web_server 的 httpd ctrl port
    cfg.max_open_sockets = 2;                // 預設 7 太浪費,一次只服務一個下載
    cfg.lru_purge_enable = true;             // 閒置連線自動回收,不佔 socket
    if (httpd_start(&srv, &cfg) == ESP_OK) {
      static const httpd_uri_t uri = {"/screenshot.bmp", HTTP_GET, shot_http_get, nullptr};
      httpd_register_uri_handler(srv, &uri);
    } else {
      srv = nullptr;
      return false;
    }
  }
  return true;
}
#else   // RADAR_DISPLAY_RGB == 0 : non-RGB bus (e.g. ESP32-P4 MIPI-DSI)
// Framebuffer screenshot relies on the parallel-RGB panel API; disable it on
// other display buses so the rest of the firmware still builds. Returns false
// so the caller reports "screenshot unavailable" instead of crashing.
inline bool screenshot_capture(void *disp) { (void) disp; return false; }
#endif  // RADAR_DISPLAY_RGB

// ---- 航班詳情:SQUAWK 一列 + 機型徽章 ----
// SQUAWK 三家來源都有;7500 劫機 / 7600 通訊失效 / 7700 一般緊急 → 轉紅,
// 呼應 ATC 告警配色。無值顯示 ----(例如剛出現、還沒收到 mode-A 碼)。
inline void radar_sq_line(lv_obj_t *lbl, const char *sq) {
  if (sq == nullptr || *sq == 0) sq = "----";
  char b[32];
  snprintf(b, sizeof(b), "SQUAWK %s", sq);
  lv_label_set_text(lbl, b);
  bool emerg = (strcmp(sq, "7500") == 0 || strcmp(sq, "7600") == 0 ||
                strcmp(sq, "7700") == 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(emerg ? 0xFF5030 : 0xD2E6D7), 0);
}

// ICAO 機型代碼的反白徽章。只有 airplanes.live / adsb.lol 提供機型,OpenSky 沒有,
// 所以無值時整個隱藏——否則會在呼號旁留一塊空的綠底。
inline void radar_type_badge(lv_obj_t *lbl, const char *ty) {
  if (ty == nullptr || *ty == 0) {
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_label_set_text(lbl, ty);
  lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
}

// ---- 機型輪廓 + 技術規格(按下機型徽章後蓋在航班資訊上的 spec_panel)----
// aircraft_db.h 只放純資料(A8 點陣 + AC_SPECS 表),LVGL 的型別隔在這一小段:
// main 是 LVGL 8.4、lvgl9 分支是 9.x,descriptor 的欄位名不同。
//
// 縮放為什麼自己算,不交給 lv_img_set_zoom():
//   LVGL 9 的 lv_draw_sw_img.c 只有「未經 transform 的 A8」有專用快路徑
//   (:240,blend_dsc.color = draw_dsc->recolor,正是我們要的上色方式)。
//   一旦設了 zoom,transformed 為真,A8 就掉進通用的 transform_and_recolor,
//   那條路按彩色點陣圖的位元組寬度走 stride,把 1 byte/px 的 alpha 當成
//   2~4 bytes/px 讀 → 每列漂移、整塊變雜訊(P4 實測)。LVGL 8 反而支援,
//   所以這是兩個分支唯一會表現不同的地方。
//   自己做盒式平均縮放:兩個版本都只走那條已知正確的快路徑,邊緣品質也更好,
//   而且只在按下徽章時算一次(最多 96x96 次迴圈)。
#if defined(LVGL_VERSION_MAJOR) && LVGL_VERSION_MAJOR >= 9
// LVGL 9 的兩個快取都以「來源指標」為 key,而我們是同一個 descriptor 換內容(尺寸也會變),
// 所以資料與 header 兩份都要丟掉。這兩支函式實作在 misc/cache/instance/,但 lvgl.h
// 沒有把它們的標頭轉出來(9.5 實測),所以在這裡自己宣告。
extern "C" void lv_image_cache_drop(const void *src);
extern "C" void lv_image_header_cache_drop(const void *src);
#define RADAR_ANIM_TIME(a, ms) lv_anim_set_duration((a), (ms))
#define RADAR_ANIM_DEL(var, cb) lv_anim_delete((var), (cb))
#else
#define RADAR_ANIM_TIME(a, ms) lv_anim_set_time((a), (ms))
#define RADAR_ANIM_DEL(var, cb) lv_anim_del((var), (cb))
#endif

inline constexpr int RADAR_SIL_MIN_PX = 32;   // 太小會糊成一團,翼展數字仍照實印

// 輪廓依「真實翼展」等比縮放:A380(79.8 m)佔滿整格、C172(11 m)只有一小塊。
inline int radar_sil_px(uint16_t span_dm) {
  const float span_m = span_dm ? span_dm * 0.1f : 30.0f;   // 未知翼展 → 中等大小
  int px = (int) (AC_SIL_W * span_m / 80.0f + 0.5f);       // 80 m ≈ A380,對到滿格
  if (px < RADAR_SIL_MIN_PX) px = RADAR_SIL_MIN_PX;
  if (px > AC_SIL_W) px = AC_SIL_W;
  return px;
}

// 把第 idx 張輪廓縮到 px×px 後餵給 image widget。只有一個 descriptor 與一塊緩衝,
// 因為同一時間只顯示一台飛機;但 LVGL 的 image cache 以來源指標為 key,內容換了
// 指標沒換 → 必須先把快取丟掉,否則會畫到上一個機型。
inline void radar_sil_set(lv_obj_t *img, int idx, uint16_t span_dm) {
  static uint8_t buf[AC_SIL_W * AC_SIL_H];
  static lv_img_dsc_t dsc;
  if (idx < 0 || idx >= AC_SIL_N) {
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  const uint8_t *src = AC_SIL_A8 + (size_t) idx * AC_SIL_W * AC_SIL_H;
  const int px = radar_sil_px(span_dm);

  // 面板每秒會重填一次(機型碼可能後補、單位可能被改),圖沒變就不要重算重畫:
  // 省掉每秒一次的縮放與 invalidate,也不會打斷正在跑的掃描動畫。
  static int last_idx = -1, last_px = -1;
  if (idx == last_idx && px == last_px && !lv_obj_has_flag(img, LV_OBJ_FLAG_HIDDEN)) return;
  last_idx = idx;
  last_px = px;

  // 盒式平均(px == AC_SIL_W 時每格只有一個來源像素,等於直接複製)
  for (int y = 0; y < px; y++) {
    const int sy0 = y * AC_SIL_H / px, sy1 = (y + 1) * AC_SIL_H / px;
    for (int x = 0; x < px; x++) {
      const int sx0 = x * AC_SIL_W / px, sx1 = (x + 1) * AC_SIL_W / px;
      uint32_t sum = 0, n = 0;
      for (int sy = sy0; sy < sy1; sy++)
        for (int sx = sx0; sx < sx1; sx++) { sum += src[sy * AC_SIL_W + sx]; n++; }
      buf[y * px + x] = n ? (uint8_t) (sum / n) : 0;
    }
  }

#if defined(LVGL_VERSION_MAJOR) && LVGL_VERSION_MAJOR >= 9
  lv_image_cache_drop(&dsc);
  lv_image_header_cache_drop(&dsc);
  dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc.header.cf = LV_COLOR_FORMAT_A8;
  dsc.header.stride = px;
#else
  lv_img_cache_invalidate_src(&dsc);
  dsc.header.always_zero = 0;
  dsc.header.cf = LV_IMG_CF_ALPHA_8BIT;
#endif
  dsc.header.w = px;
  dsc.header.h = px;
  dsc.data_size = (uint32_t) px * px;
  dsc.data = buf;

  lv_img_set_src(img, &dsc);
  lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
  lv_obj_invalidate(img);
}


// ---- 輪廓的掃描式出現 ----
// 一條亮黃掃描線由上而下掠過,線走過的地方才露出輪廓。遮罩與掃描線是 spec_img_box 裡
// 的兩個 obj(宣告在 image 之後,所以疊在圖上面),動畫只驅動它們的 y / height。
// 只在按下徽章時放一次;面板每秒的重填不會重放,否則會一直閃。
inline lv_obj_t *g_scan_cover = nullptr;
inline lv_obj_t *g_scan_line = nullptr;
inline int32_t g_scan_h = 0;

inline void radar_scan_exec(void *, int32_t v) {
  if (g_scan_cover == nullptr) return;
  lv_obj_set_y(g_scan_cover, v);              // 遮罩上緣往下退 = 由上而下露出
  lv_obj_set_height(g_scan_cover, g_scan_h - v);
  lv_obj_set_y(g_scan_line, v);               // 掃描線停在露出邊界上
}

inline void radar_scan_done(lv_anim_t *) {
  if (g_scan_cover == nullptr) return;
  lv_obj_add_flag(g_scan_cover, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_scan_line, LV_OBJ_FLAG_HIDDEN);
}

inline void radar_sil_scan(lv_obj_t *img, lv_obj_t *cover, lv_obj_t *line) {
  if (lv_obj_has_flag(img, LV_OBJ_FLAG_HIDDEN)) return;   // 這個機型沒有圖,不用掃
  g_scan_cover = cover;
  g_scan_line = line;
  // 高度取自外框 spec_img_box:800x480 是 96,1024x600 被 scale_layout 放成 120
  g_scan_h = lv_obj_get_height(lv_obj_get_parent(cover));
  if (g_scan_h <= 0) return;

  RADAR_ANIM_DEL(cover, radar_scan_exec);   // 連點時重來,不要兩個動畫疊著跑
  lv_obj_clear_flag(cover, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(line, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_y(cover, 0);
  lv_obj_set_height(cover, g_scan_h);
  lv_obj_set_y(line, 0);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, cover);
  lv_anim_set_exec_cb(&a, radar_scan_exec);
  lv_anim_set_values(&a, 0, g_scan_h);
  RADAR_ANIM_TIME(&a, 420);
  lv_anim_set_ready_cb(&a, radar_scan_done);
  lv_anim_start(&a);
}

// 八個 label + 一個 image。分兩層:
//   上半(l0~l4 + l7)是「機型」——查 AC_SPECS,查不到就退到 API 給的 desc。
//     發動機(l7)屬於機型的性能參數,所以跟 SPAN/LEN/MTOW/CRZ 同區同字級。
//   下半(l5/l6)是「這一台」——註冊號、註冊國、營運者,不需要機型資料庫也印得出來;
//     字級與上半一致(font_mono),只用顏色分層次。
// 數值 0 代表資料庫沒這一項 → 印 "--",不要印出 0.0 m 這種假數字。
inline void radar_fill_spec(lv_obj_t *img, lv_obj_t *l0, lv_obj_t *l1, lv_obj_t *l2,
                            lv_obj_t *l3, lv_obj_t *l4, lv_obj_t *l5, lv_obj_t *l6,
                            lv_obj_t *l7,
                            const char *ty, const char *reg, const char *desc,
                            const char *cat, uint32_t hex, const char *cs,
                            bool imperial) {
  const AcSpec *s = ac_spec_find(ty);
  char b[64];

  // ---- 標題:資料庫的 製造商+型號 優先,其次 API 的 desc,再其次只印代碼 ----
  if (s && *s->mfr)        snprintf(b, sizeof(b), "%s %s", s->mfr, s->model);
  else if (desc && *desc)  snprintf(b, sizeof(b), "%s", desc);
  else                     snprintf(b, sizeof(b), "%s  -  NO SPEC DATA",
                                    ty && *ty ? ty : "UNKNOWN TYPE");
  lv_label_set_text(l0, b);

  // ---- 尺寸四行 ----
  // 直升機的 span_dm 存的是主旋翼直徑,標題跟著換,免得看成「翼展」。
  const bool heli = s && (strcmp(s->cls, "Helicopter") == 0 ||
                          strcmp(s->cls, "Gyrocopter") == 0);
  const char *span_tag = heli ? "ROTOR" : "SPAN ";
  if (s && s->span_dm)
    snprintf(b, sizeof(b), imperial ? "%s %5.0f ft" : "%s %5.1f m",
             span_tag, s->span_dm * (imperial ? 0.32808f : 0.1f));
  else
    snprintf(b, sizeof(b), "%s    --", span_tag);
  lv_label_set_text(l1, b);

  if (s && s->len_dm)
    snprintf(b, sizeof(b), imperial ? "LEN   %5.0f ft" : "LEN   %5.1f m",
             s->len_dm * (imperial ? 0.32808f : 0.1f));
  else
    snprintf(b, sizeof(b), "LEN      --");
  lv_label_set_text(l2, b);

  if (s && s->mtow_100kg)
    snprintf(b, sizeof(b), imperial ? "MTOW %6.0f lb" : "MTOW  %5.1f t",
             s->mtow_100kg * (imperial ? 220.462f : 0.1f));
  else
    snprintf(b, sizeof(b), "MTOW     --");
  lv_label_set_text(l3, b);

  if (s && s->cruise_kt)
    snprintf(b, sizeof(b), imperial ? "CRZ   %5.0f mph" : "CRZ   %5.0f km/h",
             s->cruise_kt * (imperial ? 1.1508f : 1.852f));
  else
    snprintf(b, sizeof(b), "CRZ      --");
  lv_label_set_text(l4, b);

  // ---- 發動機:型號與數量,屬於機型規格,與上面四行同區 ----
  static const char *const ENG[] = {"", "PISTON", "TURBOPROP", "JET", "ELECTRIC"};
  char eng[32] = "";
  if (s) {
    const char *et = s->eng_t <= 4 ? ENG[s->eng_t] : "";
    if (s->eng_n && *s->eng)      snprintf(eng, sizeof(eng), "%u x %s", (unsigned) s->eng_n, s->eng);
    else if (s->eng_n && *et)     snprintf(eng, sizeof(eng), "%u x %s", (unsigned) s->eng_n, et);
    else if (*s->eng)             snprintf(eng, sizeof(eng), "%s", s->eng);
  }
  // 對齊上面四行的 "TAG   value" 排版;沒資料就跟它們一樣印 "--"
  if (*eng) snprintf(b, sizeof(b), "ENG   %s", eng);
  else      snprintf(b, sizeof(b), "ENG      --");
  lv_label_set_text(l7, b);

  // ---- 這一台飛機:註冊號 / 註冊國(由 ICAO24 位址反查)----
  const char *country = ac_country_find(hex);
  snprintf(b, sizeof(b), "%s%s%s",
           reg && *reg ? reg : "",
           (reg && *reg && country) ? "  -  " : "",       // 分隔號用 ASCII;字型的預設 glyph 集沒有 U+00B7
           country ? country : "");
  lv_label_set_text(l5, *b ? b : " ");

  // ---- 營運者:呼號前三碼查 ICAO Doc 8585 三字代碼 ----
  const AcOperator *op = ac_operator_find(cs);
  if (op) {
    // 電台呼號常常就等於公司名(EMIRATES / RYANAIR),那就只印一次
    const bool same = strcasecmp(op->name, op->radio) == 0;
    if (*op->radio && !same) snprintf(b, sizeof(b), "%s  -  %s", op->name, op->radio);
    else                     snprintf(b, sizeof(b), "%s", op->name);
    lv_label_set_text(l6, b);
  } else {
    lv_label_set_text(l6, " ");
  }

  // ---- 輪廓:先查機型,查不到退到 ADS-B 發射器類別的通用剪影 ----
  int16_t sil = s ? s->sil : -1;
  if (sil < 0) sil = ac_cat_sil(cat);
  radar_sil_set(img, sil, s ? s->span_dm : 0);
}

// ---- 系統資訊(SYS 鈕):CPU / RAM / PSRAM / FLASH / 運行時間 / API 額度 填入右下角 label ----
// sq(SQUAWK 列)在系統資訊模式下挪給 MAP 狀態專用:800x480 右下面板只有
// 292px 寬(font_mono 一字 11.3px ≈ 26 字),「FLASH .. APP .. MAP ..」塞同一行
// 會被螢幕右緣切掉——切掉的偏偏就是要使用者回報的 MAP 欄位(issue #7)。
// 拆成兩行後各 ≤ 24 字都放得下,且 MAP 提到第二列更醒目。
inline void radar_show_sysinfo(lv_obj_t *cs, lv_obj_t *route, lv_obj_t *sq,
                               lv_obj_t *l1, lv_obj_t *l2, lv_obj_t *l3,
                               lv_obj_t *l4, int rssi) {
  char b[48];
  lv_label_set_text(cs, "SYSTEM");
#if defined(USE_ESP32_VARIANT_ESP32P4)
  const char *chip = "ESP32-P4";
#elif defined(USE_ESP32_VARIANT_ESP32S3)
  const char *chip = "ESP32-S3";
#elif defined(USE_ESP32_VARIANT_ESP32)
  const char *chip = "ESP32";
#else
  const char *chip = "ESP32";
#endif
#ifdef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
  const unsigned mhz = (unsigned) CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
#else
  const unsigned mhz = (unsigned) esp_rom_get_cpu_ticks_per_us();
#endif
  snprintf(b, sizeof(b), "%s %uMHz   RSSI %d", chip, mhz, rssi);
  lv_label_set_text(route, b);
  // 地圖狀態:**這是使用者唯一看得到它的地方**。Guition 那塊板的 I2C 佔用
  // GPIO19/20(原生 USB 資料腳),app 一啟動 USB serial 就死,拿不到開機 log;
  // 而地圖在 Wi-Fi 之前就載入,web UI 的 log 也接不到。沒有這一行,「地圖不顯示」
  // 只能靠猜是下載、儲存還是繪製出問題(issue #7)。
  //   MAP 2t 3075p = 2 張圖磚、3075 個輪廓點 → 資料在,問題在繪製
  //   MAP none     = 分割區裡沒有有效地圖 → 問題在下載或儲存
  if (maptiles::loaded)
    snprintf(b, sizeof(b), "MAP %dt %up",
             maptiles::stored_tiles, (unsigned) (maptiles::OUTLINE.size() / 2));
  else
    snprintf(b, sizeof(b), "MAP none");
  lv_label_set_text(sq, b);
  snprintf(b, sizeof(b), "RAM   %4u / %4u KB",
           (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned) (heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024));
  lv_label_set_text(l1, b);
  snprintf(b, sizeof(b), "PSRAM %4u / %4u KB",
           (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
           (unsigned) (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));
  lv_label_set_text(l2, b);
  uint32_t fsz = 0;
  esp_flash_get_size(nullptr, &fsz);
  const esp_partition_t *ap = esp_ota_get_running_partition();
  snprintf(b, sizeof(b), "FLASH %u MB  APP %.1f MB",
           (unsigned) (fsz >> 20), ap ? ap->size / 1048576.0f : 0.0f);
  lv_label_set_text(l3, b);
  uint32_t up = (uint32_t) (esp_timer_get_time() / 1000000LL);
  if (radar_bg::g_last_src > 0)   // 免費/合併來源:無額度,顯示來源名
    snprintf(b, sizeof(b), "UP %ud %02u:%02u   SRC %s", (unsigned) (up / 86400),
             (unsigned) (up / 3600 % 24), (unsigned) (up / 60 % 60),
             radar_bg::src_name(radar_bg::g_last_src));
  else if (radar_bg::g_os_remaining >= 0)
    snprintf(b, sizeof(b), "UP %ud %02u:%02u   API %d", (unsigned) (up / 86400),
             (unsigned) (up / 3600 % 24), (unsigned) (up / 60 % 60),
             radar_bg::g_os_remaining);   // OpenSky 當日剩餘呼叫額度
  else
    snprintf(b, sizeof(b), "UP %ud %02u:%02u   API ----", (unsigned) (up / 86400),
             (unsigned) (up / 3600 % 24), (unsigned) (up / 60 % 60));
  lv_label_set_text(l4, b);
}
