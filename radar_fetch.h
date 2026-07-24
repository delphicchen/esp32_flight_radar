// 背景航班抓取引擎:跑在 core 1 的 FreeRTOS task,主迴圈(UI)完全不阻塞。
// 主迴圈用 request_states()/request_route() 丟工作,
// 每秒輪詢 g_states_ready / g_route_ready 取結果。
#pragma once
// ---- Per-board geometry (override via build_flags in boards/*.yaml) ----
//   RADAR_CANVAS  square radar canvas side (px); must equal the LVGL
//                 base_canvas size in the matching ui/ package.
//   SCREEN_W/H    panel size, used by the screenshot framebuffer copy.
//   RADAR_DISPLAY_RGB  1 = parallel-RGB panel (rpi_dpi_rgb, framebuffer
//                 screenshot available); 0 = other bus (e.g. MIPI-DSI on
//                 ESP32-P4) where the RGB framebuffer grab does not apply.
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
#include <cmath>
#include <map>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#if RADAR_DISPLAY_RGB
#include "esp_lcd_panel_rgb.h"
#endif
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
extern "C" {
#include "pngle.h"
}
#include "esp_log.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/image/image.h"
#if RADAR_DISPLAY_RGB
#include "esphome/components/rpi_dpi_rgb/rpi_dpi_rgb.h"
#endif
#include "map_data.h"

namespace radar_bg {

struct AcInfo {
  float lat, lon, trk, vel, alt, vr, dist;
  uint32_t lc;   // last_contact (epoch 秒),ATC 模式判斷訊號延遲用
  std::string cs;
};

struct Job {
  int type;  // 1 = states, 2 = route, 3 = echo, 4 = weather, 5 = speakers, 6 = route cache
  std::string cid, sec, callsign;
  float lat, lon, range;
  int src;   // states 資料來源:0=OpenSky 1=airplanes.live 2=adsb.lol
};

// ---- task → main 的結果(g_states_ready/g_route_ready 當柵欄)----
inline std::vector<AcInfo> g_result;
inline volatile bool g_states_ready = false;
inline std::string g_route;
inline volatile bool g_route_ready = false;
inline uint8_t *g_echo_buf = nullptr;    // 離屏合成緩衝 456*456*3 (PSRAM)
inline volatile bool g_echo_ready = false;   // true=g_echo_buf 有整幀待主迴圈換上
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
inline volatile int g_last_src = -1;       // 最近一次成功抓取的來源(0/1/2,-1=尚未成功)

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
  std::string out;
  out.reserve(reserve_hint);   // >16KB 的配置會進 PSRAM(SPIRAM_USE_MALLOC)
  if (esp_http_client_open(c, body.size()) == ESP_OK) {
    if (!body.empty()) esp_http_client_write(c, body.c_str(), body.size());
    esp_http_client_fetch_headers(c);
    status = esp_http_client_get_status_code(c);
    char buf[2048];
    int n;
    while ((n = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
      out.append(buf, n);
      if (out.size() > 150000) break;
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

inline bool do_states_opensky(const Job &j) {
  if (!ensure_token(j)) return false;
  float coslat = cosf(j.lat * 3.14159265f / 180.0f);
  float dlat = j.range / 110.574f;
  float dlon = j.range / (111.320f * coslat);
  char url[192];
  snprintf(url, sizeof(url),
           "https://opensky-network.org/api/states/all?lamin=%.4f&lomin=%.4f&lamax=%.4f&lomax=%.4f",
           j.lat - dlat, j.lon - dlon, j.lat + dlat, j.lon + dlon);
  int st = 0;
  g_want_rl = true;
  std::string r = http_req(url, false, "", nullptr, t_token, st, 131072);
  g_want_rl = false;
  if (st == 401) { t_token.clear(); return false; }   // 下一輪重新換發
  if (st != 200 || r.empty()) {
    ESP_LOGW("radar_bg", "states failed: %d (%u bytes)", st, (unsigned) r.size());
    return false;
  }
  std::vector<AcInfo> acs;
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
      const char *c = a[1] | "";
      ac.cs = c;
      while (!ac.cs.empty() && ac.cs.back() == ' ') ac.cs.pop_back();
      if (ac.cs.empty()) ac.cs = "?";
      float e = (alon - lon0) * 111.320f * coslat;
      float n = (alat - lat0) * 110.574f;
      ac.dist = sqrtf(e * e + n * n);
      acs.push_back(ac);
    }
    return true;
  });
  std::sort(acs.begin(), acs.end(),
            [](const AcInfo &a, const AcInfo &b) { return a.dist < b.dist; });
  xSemaphoreTake(mtx(), portMAX_DELAY);
  g_result = std::move(acs);
  g_states_ready = true;
  g_last_src = 0;
  xSemaphoreGive(mtx());
  return true;
}

// airplanes.live / adsb.lol(readsb /v2/point,免金鑰):回傳英制,這裡換算回公制
// 使 UI/ATC 端與 OpenSky 完全同構;lc 由 now(epoch ms)- seen 還原成 last_contact
inline bool do_states_v2(const Job &j, int src) {
  float r_nm = j.range / 1.852f;
  if (r_nm > 250.0f) r_nm = 250.0f;   // v2 API 半徑上限 250 nm(463 km)
  char url[160];
  snprintf(url, sizeof(url), "https://%s/v2/point/%.4f/%.4f/%.0f",
           src == 2 ? "api.adsb.lol" : "api.airplanes.live", j.lat, j.lon, r_nm);
  int st = 0;
  std::string r = http_req(url, false, "", nullptr, "", st, 131072);
  if (st != 200 || r.empty()) {
    ESP_LOGW("radar_bg", "v2 states(src %d) failed: %d (%u bytes)", src, st, (unsigned) r.size());
    return false;
  }
  std::vector<AcInfo> acs;
  float lat0 = j.lat, lon0 = j.lon;
  float coslat = cosf(j.lat * 3.14159265f / 180.0f);
  esphome::json::parse_json(r, [&](JsonObject root) -> bool {
    uint32_t now_s = (uint32_t) ((root["now"] | 0.0) / 1000.0);
    JsonArray arr = root["ac"].as<JsonArray>();
    if (arr.isNull()) return true;
    for (JsonVariant v : arr) {
      JsonObject a = v.as<JsonObject>();
      if (a.isNull()) continue;
      if (a["alt_baro"].is<const char *>()) continue;   // "ground" = 地面,跳過
      float alat = a["lat"] | NAN, alon = a["lon"] | NAN;
      if (isnan(alat) || isnan(alon)) continue;
      AcInfo ac;
      ac.lat = alat; ac.lon = alon;
      ac.alt = (a["alt_baro"] | 0.0f) * 0.3048f;    // ft → m
      ac.vel = (a["gs"] | 0.0f) * 0.514444f;        // kt → m/s
      ac.trk = a["track"] | 0.0f;
      ac.vr  = (a["baro_rate"] | 0.0f) * 0.00508f;  // ft/min → m/s
      float seen = a["seen"] | 0.0f;
      ac.lc = now_s > (uint32_t) seen ? now_s - (uint32_t) seen : 0;
      const char *c = a["flight"] | "";
      ac.cs = c;
      while (!ac.cs.empty() && ac.cs.back() == ' ') ac.cs.pop_back();
      if (ac.cs.empty()) ac.cs = "?";
      float e = (alon - lon0) * 111.320f * coslat;
      float n = (alat - lat0) * 110.574f;
      ac.dist = sqrtf(e * e + n * n);
      acs.push_back(ac);
    }
    return true;
  });
  std::sort(acs.begin(), acs.end(),
            [](const AcInfo &a, const AcInfo &b) { return a.dist < b.dist; });
  xSemaphoreTake(mtx(), portMAX_DELAY);
  g_result = std::move(acs);
  g_states_ready = true;
  g_last_src = src;
  xSemaphoreGive(mtx());
  return true;
}

inline void do_states(const Job &j) {
  if (j.src == 1 || j.src == 2) { do_states_v2(j, j.src); return; }
  // OpenSky 主線:冷卻中直接走免費來源;失敗設 10 分鐘冷卻,到期自動回試
  uint32_t now = millis() / 1000;
  if (now >= g_os_cooldown_until) {
    if (do_states_opensky(j)) { g_os_cooldown_until = 0; return; }
    g_os_cooldown_until = now + 600;
    ESP_LOGW("radar_bg", "opensky failed, fallback to free sources for 600s");
  }
  if (!do_states_v2(j, 1)) do_states_v2(j, 2);   // airplanes.live → adsb.lol
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
  for (int y = y_lo; y < y_hi; y++) {
    float ky = by * tile_km + (y - RADAR_CX) * kmpp;
    int tj = (int) floorf(ky / tile_km);
    if (tj != j) continue;
    int ty = (int) ((ky - j * tile_km) / tile_km * th);
    if (ty < 0 || ty >= th) continue;
    int dy2 = (y - RADAR_CX) * (y - RADAR_CX);
    uint8_t *orow = g_echo_buf + (size_t) y * RADAR_CANVAS * 3;
    for (int x = x_lo; x < x_hi; x++) {
      float kx = bx * tile_km + (x - RADAR_CX) * kmpp;
      int ti = (int) floorf(kx / tile_km);
      if (ti != i) continue;
      if ((x - RADAR_CX) * (x - RADAR_CX) + dy2 > RADAR_R * RADAR_R) continue;   // 圓外(留透明)
      int tx = (int) ((kx - i * tile_km) / tile_km * tw);
      if (tx < 0 || tx >= tw) continue;
      uint8_t *sp = tmp + ((size_t) ty * tw + tx) * 4;
      if (sp[3] < 32) continue;   // 無降雨=透明
      lv_color_t col = lv_color_make(sp[0], sp[1], sp[2]);
      uint8_t *op = orow + (size_t) x * 3;
      op[0] = col.full & 0xFF;
      op[1] = (col.full >> 8) & 0xFF;
      op[2] = sp[3];
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

inline void task_fn(void *arg) {
  for (;;) {
    Job *j = nullptr;
    if (xQueueReceive(queue(), &j, portMAX_DELAY) == pdTRUE && j != nullptr) {
      if (j->type == 1) do_states(*j);
      else if (j->type == 2) do_route(*j);
      else if (j->type == 6) do_route_cache(*j);
      else if (j->type == 3) do_echo(*j);
      else if (j->type == 4) do_weather(*j);
      else if (j->type == 5) do_speakers(*j);
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

inline void radar_rebuild_base(lv_obj_t *cv, float lat0, float lon0, float rng,
                               bool map_show, bool echo_show, uint8_t atc_layers) {
  lv_img_dsc_t *img = lv_canvas_get_img(cv);
  if (!img || !img->data) return;
  const size_t BYTES = (size_t) RADAR_CANVAS * RADAR_CANVAS * sizeof(lv_color_t);
  static uint8_t *cache = nullptr;   // 底色+輪廓快取(PSRAM,416KB)
  if (!cache) cache = (uint8_t *) heap_caps_malloc(BYTES, MALLOC_CAP_SPIRAM);
  static float c_lat = NAN, c_lon = NAN, c_rng = NAN;
  static bool c_map = false;
  bool fresh = cache && c_lat == lat0 && c_lon == lon0 && c_rng == rng && c_map == map_show;
  if (fresh) {
    memcpy((void *) img->data, cache, BYTES);
  } else {
    lv_canvas_fill_bg(cv, lv_color_hex(0x040C08), LV_OPA_COVER);
    if (map_show) {
      float coslat = cosf(lat0 * 3.14159265f / 180.0f);
      lv_draw_line_dsc_t dsc;
      lv_draw_line_dsc_init(&dsc);
      dsc.color = lv_color_hex(0xD8C878);   // 淡黃色輪廓線
      dsc.width = 1;
      dsc.opa = LV_OPA_COVER;
      float r2 = rng * rng;
      bool have_prev = false;
      lv_point_t prev{0, 0};
      float pd2 = 1e18f;
      for (int i = 0; i + 1 < MAP_OUTLINE_LEN; i += 2) {
        float la = MAP_OUTLINE[i], lo = MAP_OUTLINE[i + 1];
        if (isnan(la)) { have_prev = false; continue; }
        float e = (lo - lon0) * 111.320f * coslat;
        float n = (la - lat0) * 110.574f;
        float d2 = e * e + n * n;
        lv_point_t p;
        p.x = (lv_coord_t) (RADAR_CX + e / rng * (float) RADAR_R);
        p.y = (lv_coord_t) (RADAR_CX - n / rng * (float) RADAR_R);
        if (have_prev && (d2 <= r2 || pd2 <= r2)) {
          lv_point_t seg[2] = {prev, p};
          lv_canvas_draw_line(cv, seg, 2, &dsc);
        }
        prev = p;
        pd2 = d2;
        have_prev = true;
      }
    }
    if (cache) {
      memcpy(cache, img->data, BYTES);
      c_lat = lat0; c_lon = lon0; c_rng = rng; c_map = map_show;
    }
  }
  // 回波預混合:g_echo_buf 為 [color_lo][color_hi][alpha],逐像素混進底圖
  if (echo_show && radar_bg::g_echo_buf) {
    lv_color_t *dst = (lv_color_t *) (void *) img->data;
    const uint8_t *sp = radar_bg::g_echo_buf;
    for (size_t px = 0; px < (size_t) RADAR_CANVAS * RADAR_CANVAS; px++, sp += 3) {
      if (sp[2] < 8) continue;   // 無降雨=透明,保留底圖
      lv_color_t fg;
      fg.full = (uint16_t) sp[0] | ((uint16_t) sp[1] << 8);
      dst[px] = lv_color_mix(fg, dst[px], sp[2]);
    }
  }
  // 十字線與 4 圈距離環:蓋在回波上,顏色/位置與原 widget 版一致
  lv_draw_line_dsc_t xd;
  lv_draw_line_dsc_init(&xd);
  xd.color = lv_color_hex(0x003820);
  xd.width = 1;
  lv_point_t xh[2] = {{0, RADAR_CX}, {RADAR_CANVAS, RADAR_CX}};
  lv_canvas_draw_line(cv, xh, 2, &xd);
  lv_point_t xv[2] = {{RADAR_CX, 0}, {RADAR_CX, RADAR_CANVAS}};
  lv_canvas_draw_line(cv, xv, 2, &xd);
  lv_draw_arc_dsc_t ad;
  lv_draw_arc_dsc_init(&ad);
  ad.color = lv_color_hex(0x006030);
  ad.width = 1;
  const lv_coord_t RINGS[4] = {RADAR_CX, (lv_coord_t)(RADAR_CX*3/4), (lv_coord_t)(RADAR_CX/2), (lv_coord_t)(RADAR_CX/4)};
  for (int k = 0; k < 4; k++)
    lv_canvas_draw_arc(cv, RADAR_CX, RADAR_CX, RINGS[k], 0, 360, &ad);
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
    lv_draw_line_dsc_t asd;
    lv_draw_line_dsc_init(&asd);
    asd.width = 1;
    for (int a = 0; a < AIRSPACES_LEN; a++) {
      const MapAirspace &as = AIRSPACES[a];
      asd.color = lv_color_hex(as.cls == 0 ? 0x3A7A9A : 0x2A5070);
      bool hp = false;
      lv_point_t prev{0, 0};
      float pd2 = 1e18f;
      for (int k = 0; k < as.npts; k++) {
        float e, n;
        lv_point_t p;
        prj(AIRSPACE_PTS[as.off + 2 * k], AIRSPACE_PTS[as.off + 2 * k + 1], e, n, p);
        float d2 = e * e + n * n;
        if (hp && (d2 <= r2 || pd2 <= r2)) {
          lv_point_t s[2] = {prev, p};
          lv_canvas_draw_line(cv, s, 2, &asd);
        }
        prev = p; pd2 = d2; hp = true;
      }
    }
    }
    // 跑道延伸中線(虛線)+ 跑道本體(亮實線)
    if (atc_layers & 2) {
    lv_draw_line_dsc_t exd;
    lv_draw_line_dsc_init(&exd);
    exd.color = lv_color_hex(0x4A7A8A);
    exd.width = 1;
    lv_draw_line_dsc_t rwd;
    lv_draw_line_dsc_init(&rwd);
    rwd.color = lv_color_hex(0xD0E4EE);
    rwd.width = 2;
    for (int i = 0; i < RUNWAYS_LEN; i++) {
      const MapRunway &r = RUNWAYS[i];
      float e1, n1, e2, n2;
      lv_point_t p1, p2, x1, x2;
      prj(r.xlat1, r.xlon1, e1, n1, x1);
      prj(r.xlat2, r.xlon2, e2, n2, x2);
      if (e1 * e1 + n1 * n1 > r2 && e2 * e2 + n2 * n2 > r2) continue;
      // LVGL sw 渲染器的 dash 不支援斜線,延伸中線手動切段畫虛線(5px 畫 4px 空)
      float ddx = (float) (x2.x - x1.x), ddy = (float) (x2.y - x1.y);
      float dl = sqrtf(ddx * ddx + ddy * ddy);
      if (dl >= 2.0f) {
        for (float s = 0; s < dl; s += 9.0f) {
          float t0 = s / dl, t1 = (s + 5.0f) / dl;
          if (t1 > 1.0f) t1 = 1.0f;
          lv_point_t sx[2] = {
              {(lv_coord_t) (x1.x + ddx * t0), (lv_coord_t) (x1.y + ddy * t0)},
              {(lv_coord_t) (x1.x + ddx * t1), (lv_coord_t) (x1.y + ddy * t1)}};
          lv_canvas_draw_line(cv, sx, 2, &exd);
        }
      }
      prj(r.lat1, r.lon1, e1, n1, p1);
      prj(r.lat2, r.lon2, e2, n2, p2);
      lv_point_t sr[2] = {p1, p2};
      lv_canvas_draw_line(cv, sr, 2, &rwd);
    }
    }
    // 導航點:小空心三角 + 名稱(暗色,避免壓過航機)
    if (atc_layers & 8) {
    lv_draw_line_dsc_t fxd;
    lv_draw_line_dsc_init(&fxd);
    fxd.color = lv_color_hex(0x4A7A5A);
    fxd.width = 1;
    lv_draw_label_dsc_t fld;
    lv_draw_label_dsc_init(&fld);
    fld.font = lv_font_default();
    fld.color = lv_color_hex(0x50805E);
    for (int i = 0; i < FIXES_LEN; i++) {
      float e, n;
      lv_point_t p;
      prj(FIXES[i].lat, FIXES[i].lon, e, n, p);
      if (e * e + n * n > r2) continue;
      lv_point_t tri[4] = {{(lv_coord_t)(p.x), (lv_coord_t)(p.y - 3)},
                           {(lv_coord_t)(p.x + 3), (lv_coord_t)(p.y + 3)},
                           {(lv_coord_t)(p.x - 3), (lv_coord_t)(p.y + 3)},
                           {(lv_coord_t)(p.x), (lv_coord_t)(p.y - 3)}};
      lv_canvas_draw_line(cv, tri, 4, &fxd);
      lv_canvas_draw_text(cv, p.x + 5, p.y - 6, 60, &fld, FIXES[i].name);
    }
    }
    // 機場:實心小方塊 + ICAO 代碼(最上層)
    if (atc_layers & 4) {
    lv_draw_rect_dsc_t apd;
    lv_draw_rect_dsc_init(&apd);
    apd.bg_color = lv_color_hex(0xE0ECF4);
    apd.bg_opa = LV_OPA_COVER;
    lv_draw_label_dsc_t ald;
    lv_draw_label_dsc_init(&ald);
    ald.font = lv_font_default();
    ald.color = lv_color_hex(0x9AC8E0);
    for (int i = 0; i < AIRPORTS_LEN; i++) {
      float e, n;
      lv_point_t p;
      prj(AIRPORTS[i].lat, AIRPORTS[i].lon, e, n, p);
      if (e * e + n * n > r2) continue;
      lv_canvas_draw_rect(cv, p.x - 2, p.y - 2, 5, 5, &apd);
      lv_canvas_draw_text(cv, p.x + 5, p.y - 14, 60, &ald, AIRPORTS[i].icao);
    }
    }
  }
  lv_obj_invalidate(cv);
}

// ---- 三指下滑截圖:抓 RGB 面板 framebuffer → BMP,經 HTTP(:8081)供 HA downloader 下載 ----
#define SHOT_SWAP_BYTES 1   // 若截圖紅藍對調/顏色錯亂,改 1 重編譯
inline uint8_t *g_shot_buf = nullptr;        // 800*480*2 快照(PSRAM,首次截圖才配置)
inline volatile bool g_shot_valid = false;

#if RADAR_DISPLAY_RGB
// esp_lcd panel handle 在 ESPHome 元件裡是 protected,用衍生類取用(單 FB,拿到即當前畫面)
struct RpiSpy : public esphome::rpi_dpi_rgb::RpiDpiRgb {
  static esp_lcd_panel_handle_t handle(esphome::rpi_dpi_rgb::RpiDpiRgb *d) {
    return static_cast<RpiSpy *>(d)->handle_;
  }
};

inline esp_err_t shot_http_get(httpd_req_t *req) {
  if (!g_shot_valid || !g_shot_buf) { httpd_resp_send_404(req); return ESP_OK; }
  const int W = SCREEN_W, H = SCREEN_H;
  // 24-bit BMP 標頭(row 3*800 是 4 的倍數,免 padding)
  uint8_t hdr[54] = {0};
  uint32_t img = (uint32_t) W * H * 3, sz = 54 + img, off = 54, ihsz = 40;
  int32_t w = W, h = H;
  uint16_t planes = 1, bpp = 24;
  hdr[0] = 'B'; hdr[1] = 'M';
  memcpy(hdr + 2, &sz, 4);  memcpy(hdr + 10, &off, 4);  memcpy(hdr + 14, &ihsz, 4);
  memcpy(hdr + 18, &w, 4);  memcpy(hdr + 22, &h, 4);
  memcpy(hdr + 26, &planes, 2);  memcpy(hdr + 28, &bpp, 2);  memcpy(hdr + 34, &img, 4);
  httpd_resp_set_type(req, "image/bmp");
  httpd_resp_send_chunk(req, (const char *) hdr, 54);
  static uint8_t row[SCREEN_W * 3];
  for (int y = H - 1; y >= 0; y--) {         // BMP 由下往上
    const uint16_t *src = (const uint16_t *) g_shot_buf + (size_t) y * W;
    for (int x = 0; x < W; x++) {
      uint16_t v = src[x];
#if SHOT_SWAP_BYTES
      v = (uint16_t) ((v >> 8) | (v << 8));
#endif
      row[x * 3 + 0] = (uint8_t) ((v & 0x1F) << 3);          // B
      row[x * 3 + 1] = (uint8_t) (((v >> 5) & 0x3F) << 2);   // G
      row[x * 3 + 2] = (uint8_t) (((v >> 11) & 0x1F) << 3);  // R
    }
    if (httpd_resp_send_chunk(req, (const char *) row, sizeof(row)) != ESP_OK)
      return ESP_FAIL;
  }
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

inline bool screenshot_capture(esphome::rpi_dpi_rgb::RpiDpiRgb *disp) {
  const size_t BYTES = (size_t) SCREEN_W * SCREEN_H * 2;
  if (!g_shot_buf) g_shot_buf = (uint8_t *) heap_caps_malloc(BYTES, MALLOC_CAP_SPIRAM);
  if (!g_shot_buf) return false;
  void *fb = nullptr;
  if (esp_lcd_rgb_panel_get_frame_buffer(RpiSpy::handle(disp), 1, &fb) != ESP_OK || !fb)
    return false;
  g_shot_valid = false;                      // 服務端讀到一半時避免撕裂判定
  memcpy(g_shot_buf, fb, BYTES);
  g_shot_valid = true;
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

// ---- 系統資訊(i 鈕):CPU / RAM / PSRAM / FLASH / 運行時間 / API 額度 填入右下角六個 label ----
inline void radar_show_sysinfo(lv_obj_t *cs, lv_obj_t *route, lv_obj_t *l1,
                               lv_obj_t *l2, lv_obj_t *l3, lv_obj_t *l4, int rssi) {
  char b[48];
  lv_label_set_text(cs, "SYSTEM");
  snprintf(b, sizeof(b), "ESP32-S3 %uMHz   RSSI %d",
           (unsigned) esp_rom_get_cpu_ticks_per_us(), rssi);
  lv_label_set_text(route, b);
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
  snprintf(b, sizeof(b), "FLASH %u MB   APP %.1f MB", (unsigned) (fsz >> 20),
           ap ? ap->size / 1048576.0f : 0.0f);
  lv_label_set_text(l3, b);
  uint32_t up = (uint32_t) (esp_timer_get_time() / 1000000LL);
  if (radar_bg::g_last_src > 0)   // 免費來源(手選或 fallback):無額度,顯示來源名
    snprintf(b, sizeof(b), "UP %ud %02u:%02u   SRC %s", (unsigned) (up / 86400),
             (unsigned) (up / 3600 % 24), (unsigned) (up / 60 % 60),
             radar_bg::g_last_src == 2 ? "ADSB.LOL" : "A.LIVE");
  else if (radar_bg::g_os_remaining >= 0)
    snprintf(b, sizeof(b), "UP %ud %02u:%02u   API %d", (unsigned) (up / 86400),
             (unsigned) (up / 3600 % 24), (unsigned) (up / 60 % 60),
             radar_bg::g_os_remaining);   // OpenSky 當日剩餘呼叫額度
  else
    snprintf(b, sizeof(b), "UP %ud %02u:%02u   API ----", (unsigned) (up / 86400),
             (unsigned) (up / 3600 % 24), (unsigned) (up / 60 % 60));
  lv_label_set_text(l4, b);
}
