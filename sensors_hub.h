#pragma once
// #24 時鐘/感測器頁:通用數值登記 + 24 小時曲線 + 卡片排版
// 資料來源(網路/I2C/UART/HA)只管呼叫 shub::set(key, value);
// 主迴圈每 1 分鐘 shub::sample() 寫進 ring buffer;頁面顯示時 shub::refresh() 更新畫面。
// 卡片全部在 C++ 依螢幕寬度建立(800 → 1.0 倍,1024 → 1.25 倍),格數可自動或手動。
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "esp_heap_caps.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/core/time.h"

namespace shub {

// 曲線點座標:LVGL 9 的 lv_line 改用 lv_point_precise_t(main 是 LVGL 8,lvgl9 分支是 9)
#if LVGL_VERSION_MAJOR >= 9
typedef lv_point_precise_t spark_pt_t;
#else
typedef lv_point_t spark_pt_t;
#endif

// 固定的數值種類
// OUT_P 加在最後:網頁 Sensor Slot 下拉與 NVS 存的是 key+1,插在中間會讓舊設定錯位
enum Key : uint8_t { OUT_T = 0, OUT_H, IN_T, IN_H, PRES, CO2, HA1, HA2, HA3, HA4, OUT_P, KEY_N };
// AUTO 排列與齒輪選單的順序(網路 → 本地 → HA)
static const uint8_t ORDER[KEY_N] = {OUT_T, OUT_H, OUT_P, IN_T, IN_H, PRES, CO2, HA1, HA2, HA3, HA4};
static const int HIST_N = 1440;         // 每 1 分鐘一點 × 24 小時(每筆 5.6KB,放 PSRAM)
static const int SLOT_MAX = 6;
static const int SPARK_N = 96;          // 曲線畫 96 點(依時間窗平均)

struct Reading {
  char name[48];             // HA 名稱可能是中文(UTF-8 一字 3 bytes)
  char src[16];
  char unit[10];
  uint8_t decimals;
  bool temp;                // 溫度:英制時換算成 F
  bool registered;          // 曾經有過資料(AUTO 排列只看這個)
  float value;              // 最新值,NAN = 目前沒資料
  uint32_t value_ms;        // 最新值的 millis
  float *hist;              // HIST_N 點,NAN = 該時段沒資料
  int head;                 // 下一個寫入位置
  int count;                // 已寫入的點數(最多 HIST_N);曲線時間窗 = 有資料的這段
};

inline Reading g_rd[KEY_N];

// ---- 告警門檻與顏色:每個數值一組(跟著數值走,不跟格子),存 NVS ----
// 超過上限 / 低於下限時,數值、曲線、卡片外框換成指定顏色;不發聲、不亮屏。
static const uint32_t PAL[6] = {0x00E66E, 0x00D0E0, 0x4080FF, 0xE6B800, 0xFF7800, 0xFF4040};
struct Alert {
  float hi, lo;                 // NAN = OFF;溫度一律以 °C 存
  uint8_t c_ok, c_hi, c_lo, pad;  // PAL 的索引
};
struct AlertTab {
  uint32_t magic;
  Alert a[KEY_N];
};
static const uint32_t ALERT_MAGIC = 0x414C5431;   // "ALT1"
static const int KEY_N_V1 = 10;                    // v1.4.0 存的門檻表只有 10 個 key(沒有 OUT_P)
struct AlertTabV1 {
  uint32_t magic;
  Alert a[KEY_N_V1];
};
inline AlertTab g_al;
inline esphome::ESPPreferenceObject g_al_pref;

inline void alert_default(int k) {
  Alert &a = g_al.a[k];
  a.hi = NAN;
  a.lo = NAN;
  a.c_ok = 0;   // 綠
  a.c_hi = 5;   // 紅
  a.c_lo = 2;   // 藍
  a.pad = 0;
  if (k == CO2) a.hi = 1200;   // 取代原本寫死的 CO2 分級
}
inline void alert_load() {
  g_al_pref = esphome::global_preferences->make_preference<AlertTab>(esphome::fnv1_hash("shub_alert_v2"));
  if (!g_al_pref.load(&g_al) || g_al.magic != ALERT_MAGIC) {
    g_al.magic = ALERT_MAGIC;
    for (int k = 0; k < KEY_N; k++) alert_default(k);
    // 從 v1(10 個 key)搬過來,升級後不會弄丟已設定的門檻
    AlertTabV1 old;
    auto p1 = esphome::global_preferences->make_preference<AlertTabV1>(esphome::fnv1_hash("shub_alert_v1"));
    if (p1.load(&old) && old.magic == ALERT_MAGIC) {
      for (int k = 0; k < KEY_N_V1; k++) g_al.a[k] = old.a[k];
      g_al_pref.save(&g_al);
    }
  }
  for (int k = 0; k < KEY_N; k++) {   // 讀回的索引防呆
    Alert &a = g_al.a[k];
    if (a.c_ok > 5) a.c_ok = 0;
    if (a.c_hi > 5) a.c_hi = 5;
    if (a.c_lo > 5) a.c_lo = 2;
  }
}
inline void alert_save() { g_al_pref.save(&g_al); }
// HA 1~4 換了實體 → 那一格的門檻不再適用
inline void alert_reset(int k) {
  alert_default(k);
  alert_save();
}
// 0 = 正常,1 = 超過上限,-1 = 低於下限
inline int alert_state(int k, float v) {
  if (std::isnan(v)) return 0;
  const Alert &a = g_al.a[k];
  if (!std::isnan(a.hi) && v > a.hi) return 1;
  if (!std::isnan(a.lo) && v < a.lo) return -1;
  return 0;
}
inline uint32_t g_stale_ms = 30 * 60 * 1000;   // 超過 30 分鐘沒更新就當沒資料

inline void init() {
  static bool done = false;
  if (done) return;
  done = true;
  alert_load();
  static const struct { const char *n, *s, *u; uint8_t d; bool t; } DEF[KEY_N] = {
      {"TEMP", "OUTDOOR", "C", 1, true},   {"HUM", "OUTDOOR", "%", 0, false},
      {"TEMP", "INDOOR", "C", 1, true},    {"HUM", "INDOOR", "%", 0, false},
      {"PRESSURE", "INDOOR", "hPa", 0, false}, {"CO2", "INDOOR", "ppm", 0, false},
      {"HA 1", "HA", "", 1, false}, {"HA 2", "HA", "", 1, false},
      {"HA 3", "HA", "", 1, false}, {"HA 4", "HA", "", 1, false},
      {"PRESSURE", "OUTDOOR", "hPa", 0, false}};
  for (int k = 0; k < KEY_N; k++) {
    Reading &r = g_rd[k];
    snprintf(r.name, sizeof(r.name), "%s", DEF[k].n);
    snprintf(r.src, sizeof(r.src), "%s", DEF[k].s);
    snprintf(r.unit, sizeof(r.unit), "%s", DEF[k].u);
    r.decimals = DEF[k].d;
    r.temp = DEF[k].t;
    r.registered = false;
    r.value = NAN;
    r.value_ms = 0;
    r.hist = (float *) heap_caps_malloc(HIST_N * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!r.hist) r.hist = (float *) malloc(HIST_N * sizeof(float));
    if (r.hist) for (int i = 0; i < HIST_N; i++) r.hist[i] = NAN;
    r.head = 0;
    r.count = 0;
  }
}

// 字型只有 ASCII:把 HA 單位常見的 °、µ、²、³ 換成 ASCII
inline void ascii_unit(char *dst, size_t n, const char *src) {
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *) src; *p && o + 1 < n; p++) {
    if (*p < 0x80) { dst[o++] = (char) *p; continue; }
    if (p[0] == 0xC2 && p[1] == 0xB0) { p++; continue; }                 // ° 省略(沿用 "C")
    if (p[0] == 0xC2 && p[1] == 0xB5) { dst[o++] = 'u'; p++; continue; } // µ
    if (p[0] == 0xC2 && p[1] == 0xB2) { dst[o++] = '2'; p++; continue; } // ²
    if (p[0] == 0xC2 && p[1] == 0xB3) { dst[o++] = '3'; p++; continue; } // ³
    while (p[1] && (p[1] & 0xC0) == 0x80) p++;                           // 其他多位元組字元略過
  }
  dst[o] = 0;
}

// 複製 UTF-8 字串,超長時在字元邊界截斷(不切壞中文)
inline void u8copy(char *dst, size_t n, const char *src) {
  size_t L = strlen(src);
  if (L >= n) {
    L = n - 1;
    while (L > 0 && ((unsigned char) src[L] & 0xC0) == 0x80) L--;
  }
  memcpy(dst, src, L);
  dst[L] = 0;
}

// HA / 選配感測器可改名稱、來源標籤、單位、小數位數
inline void describe(Key k, const char *name, const char *src, const char *unit, int decimals) {
  init();
  Reading &r = g_rd[k];
  if (name) u8copy(r.name, sizeof(r.name), name);
  if (src) snprintf(r.src, sizeof(r.src), "%s", src);
  if (unit) ascii_unit(r.unit, sizeof(r.unit), unit);
  if (decimals >= 0) r.decimals = (uint8_t) decimals;
}

inline void set(Key k, float v) {
  init();
  if (std::isnan(v)) return;
  Reading &r = g_rd[k];
  r.value = v;
  r.value_ms = millis();
  r.registered = true;
}

inline bool fresh(const Reading &r) {
  return !std::isnan(r.value) && millis() - r.value_ms < g_stale_ms;
}

// 每 1 分鐘呼叫一次:每個登記過的數值寫一點(沒資料寫 NAN,曲線上會斷開)
inline void sample() {
  init();
  for (int k = 0; k < KEY_N; k++) {
    Reading &r = g_rd[k];
    if (!r.registered || !r.hist) continue;
    r.hist[r.head] = fresh(r) ? r.value : NAN;
    r.head = (r.head + 1) % HIST_N;
    if (r.count < HIST_N) r.count++;
  }
}

// ---------------- 畫面 ----------------
struct Card {
  lv_obj_t *box, *name, *tag, *gear, *val, *unit, *hi, *lo, *line, *t0, *t1;
  spark_pt_t pts[SPARK_N];
  int np = -1;                    // 目前畫的點數(-1 = 還沒設過)
  uint32_t col = 0xFFFFFFFF;      // 目前的數值/曲線顏色
  uint32_t bcol = 0xFFFFFFFF;     // 目前的外框顏色
  lv_coord_t bw = -1;             // 目前的外框寬度
};
inline Card g_card[SLOT_MAX];

// 內容沒變就不要 set:LVGL 每次 set 文字/樣式/點陣列都會 invalidate 重繪,即使值一樣。
// 時鐘頁每 5 秒刷新一次,大部分時候數值沒變,這樣幾乎不用重繪。
inline void set_text(lv_obj_t *l, const char *t) {
  const char *o = lv_label_get_text(l);
  if (o && strcmp(o, t) == 0) return;
  lv_label_set_text(l, t);
}
inline void card_style(Card &c, uint32_t col, uint32_t bcol, lv_coord_t bw) {
  if (c.col != col) {
    lv_obj_set_style_text_color(c.val, lv_color_hex(col), 0);
    lv_obj_set_style_line_color(c.line, lv_color_hex(col), 0);
    c.col = col;
  }
  if (c.bcol != bcol || c.bw != bw) {
    lv_obj_set_style_border_color(c.box, lv_color_hex(bcol), 0);
    lv_obj_set_style_border_width(c.box, bw, 0);
    c.bcol = bcol;
    c.bw = bw;
  }
}
inline void set_line(Card &c, const spark_pt_t *p, int np) {
  if (np == c.np && memcmp(p, c.pts, np * sizeof(spark_pt_t)) == 0) return;
  if (p != c.pts) memcpy(c.pts, p, np * sizeof(spark_pt_t));
  c.np = np;
  lv_line_set_points(c.line, c.pts, np);
}
inline int g_slot_key[SLOT_MAX];        // 各格目前顯示的 key,-1 = 空格
inline int g_slot_cfg[SLOT_MAX];        // 各格的設定(0=AUTO、key+1),layout() 時記下,選單標示目前選項
inline int g_shown = -1;                // 目前排出的格數(-1 = 尚未排版)
inline bool g_built = false;
inline float g_s = 1.0f;                // 解析度倍率
inline int g_W = 800, g_H = 480;
// 齒輪選單:選定後呼叫 yaml 註冊的回呼(寫 NVS + 同步網頁下拉 + 重排);val 0=AUTO、1..KEY_N=key+1
inline void (*g_on_pick)(int slot, int val) = nullptr;
inline lv_obj_t *g_page = nullptr, *g_pick = nullptr;
inline const lv_font_t *g_f_name = nullptr;
// HA 感測器挑選:按「HA SENSORS」→ yaml 發背景請求 → 結果回來呼叫 show_ha_list()
struct HaEnt { std::string id, name, unit, state, dc; };   // dc = device_class(清單才有)
inline std::string g_ha_raw;                      // 清單原文(大於 16KB 時在 PSRAM)
inline std::vector<uint32_t> g_ha_off;            // 每一筆在 g_ha_raw 的起點
inline int g_ha_page = 0;                         // 清單分頁(每頁 HA_PAGE 筆,列數少才不吃內部 RAM)
inline std::vector<uint8_t> g_ha_catv;            // 每一筆的類別
inline std::vector<uint16_t> g_ha_idx;            // 目前類別篩出的筆(g_ha_off 的索引)
inline int g_ha_cat = -1;                         // 目前類別,-1 = ALL
static const int HA_PAGE = 30;
inline bool g_ha_enabled = false;                 // 有填 HA token 才顯示 HA SENSORS 鈕
inline int g_ha_pick_slot = -1;                   // 正在挑 HA 感測器的那一格(-1 = 沒在挑)
inline void (*g_on_ha_list_req)() = nullptr;      // yaml:發出清單請求
inline void (*g_on_ha_pick)(int slot, const HaEnt &e) = nullptr;   // yaml:寫入 HA 1~4 並指定該格

// 解析 "id|name|unit|state" 每行一筆
inline std::vector<HaEnt> parse_ha(const std::string &raw) {
  std::vector<HaEnt> out;
  size_t p = 0;
  while (p < raw.size()) {
    size_t e = raw.find('\n', p);
    if (e == std::string::npos) e = raw.size();
    std::string ln = raw.substr(p, e - p);
    p = e + 1;
    size_t a = ln.find('|'), b = ln.find('|', a + 1), c = ln.find('|', b + 1);
    if (a == std::string::npos || b == std::string::npos || c == std::string::npos) continue;
    size_t d = ln.find('|', c + 1);   // 清單多一欄 device_class
    HaEnt h{ln.substr(0, a), ln.substr(a + 1, b - a - 1), ln.substr(b + 1, c - b - 1),
            d == std::string::npos ? ln.substr(c + 1) : ln.substr(c + 1, d - c - 1),
            d == std::string::npos ? std::string() : ln.substr(d + 1)};
    while (!h.dc.empty() && (h.dc.back() == '\r' || h.dc.back() == ' ')) h.dc.pop_back();
    while (!h.state.empty() && (h.state.back() == '\r' || h.state.back() == ' ')) h.state.pop_back();
    if (!h.id.empty()) out.push_back(h);
  }
  return out;
}

inline HaEnt ha_at(size_t i) {
  size_t a = g_ha_off[i], e = g_ha_raw.find('\n', a);
  if (e == std::string::npos) e = g_ha_raw.size();
  auto v = parse_ha(g_ha_raw.substr(a, e - a));
  return v.empty() ? HaEnt{} : v[0];
}

// HA 字串的小數位數(最多 2 位)
inline int decimals_of(const std::string &v) {
  size_t d = v.find('.');
  if (d == std::string::npos) return 0;
  int n = (int) (v.size() - d - 1);
  return n > 2 ? 2 : n;
}

static inline lv_coord_t S(float v) { return (lv_coord_t) lroundf(v * g_s); }

inline lv_obj_t *mk_label(lv_obj_t *par, const lv_font_t *f, uint32_t col) {
  lv_obj_t *l = lv_label_create(par);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
  lv_label_set_text(l, "");
  return l;
}

inline float disp(const Reading &r, float v, bool imperial) {
  return (r.temp && imperial) ? v * 1.8f + 32.0f : v;
}

inline bool imperial_now = false;
inline void close_picker() {
  // 常在選單自己的按鈕事件裡關閉:同步刪除會釋放 LVGL 正在處理的物件,一律延後刪
  if (g_pick) { lv_obj_del_async(g_pick); g_pick = nullptr; }
}

// 選單共用:半透明遮罩 + 置中面板 + 標題;回傳面板,y 為第一列位置
inline lv_obj_t *picker_frame(const char *title, lv_coord_t &y) {
  close_picker();
  g_pick = lv_obj_create(g_page);
  lv_obj_remove_style_all(g_pick);
  lv_obj_set_size(g_pick, g_W, g_H);
  lv_obj_set_style_bg_color(g_pick, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(g_pick, LV_OPA_60, 0);
  lv_obj_add_flag(g_pick, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_pick, [](lv_event_t *e) {
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) close_picker();
  }, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *pn = lv_obj_create(g_pick);
  lv_obj_set_size(pn, S(460), g_H - S(60));   // HA 名稱較長
  lv_obj_center(pn);
  lv_obj_set_style_bg_color(pn, lv_color_hex(0x07140D), 0);
  lv_obj_set_style_border_color(pn, lv_color_hex(0x00A050), 0);
  lv_obj_set_style_border_width(pn, 1, 0);
  lv_obj_set_style_radius(pn, S(6), 0);
  lv_obj_set_style_pad_all(pn, S(8), 0);
  // 此 LVGL 建置沒開 flex,逐列手動定位;超出面板高度時自動可捲動
  lv_obj_set_scroll_dir(pn, LV_DIR_VER);

  lv_obj_t *tl = lv_label_create(pn);
  lv_obj_set_style_text_font(tl, g_f_name, 0);
  lv_obj_set_style_text_color(tl, lv_color_hex(0x00E66E), 0);
  lv_label_set_text(tl, title);
  y = S(36);
  return pn;
}

// 選單的一列按鈕;user 為事件資料
inline lv_obj_t *picker_row(lv_obj_t *pn, lv_coord_t &y, const char *text, bool cur,
                            lv_event_cb_t cb, intptr_t user) {
  lv_obj_t *bt = lv_btn_create(pn);
  lv_obj_set_size(bt, LV_PCT(100), S(40));
  lv_obj_set_pos(bt, 0, y);
  y += S(46);
  lv_obj_set_style_bg_color(bt, lv_color_hex(cur ? 0x00C060 : 0x0A2416), 0);
  lv_obj_set_style_border_color(bt, lv_color_hex(0x006030), 0);
  lv_obj_set_style_border_width(bt, 1, 0);
  lv_obj_set_style_radius(bt, S(4), 0);
  lv_obj_set_style_shadow_width(bt, 0, 0);
  lv_obj_t *l = lv_label_create(bt);
  lv_obj_set_style_text_font(l, g_f_name, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(cur ? 0x04180C : 0x9AE6B4), 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_width(l, LV_PCT(100));
  lv_obj_align(l, LV_ALIGN_LEFT_MID, S(4), 0);
  lv_label_set_text(l, text);
  if (cb) lv_obj_add_event_cb(bt, cb, LV_EVENT_CLICKED, (void *) user);
  return bt;
}

// ---- 告警設定面板:齒輪 → ALERT ... ----
inline void refresh(bool imperial);
inline int g_al_key = -1;
inline lv_obj_t *g_al_val[2];        // 上限 / 下限的數值標籤
inline lv_obj_t *g_al_sw[3][6];      // 正常 / 上限 / 下限 各 6 個色塊

// 每按一下的幅度:溫度 0.5°C(英制 1°F)、濕度/氣壓 1、CO2 50、HA 依數值大小
inline float alert_step(int k) {
  const Reading &r = g_rd[k];
  if (r.temp) return imperial_now ? 5.0f / 9.0f : 0.5f;
  if (k == OUT_H || k == IN_H || k == PRES || k == OUT_P) return 1.0f;
  if (k == CO2) return 50.0f;
  float m = powf(10.0f, -(float) r.decimals);
  float v = fresh(r) ? fabsf(r.value) : 0.0f;
  if (v <= 0) return m;
  float st = powf(10.0f, floorf(log10f(v)) - 1.0f);
  return st < m ? m : st;
}
inline int alert_dec(int k) {
  const Reading &r = g_rd[k];
  if (r.temp) return 1;
  float st = alert_step(k);
  int d = st >= 1 ? 0 : (int) ceilf(-log10f(st) - 1e-4f);
  if (d < r.decimals && k >= HA1) d = r.decimals;
  return d > 2 ? 2 : d;
}
inline void alert_show() {
  if (g_al_key < 0) return;
  const Reading &r = g_rd[g_al_key];
  const Alert &a = g_al.a[g_al_key];
  const char *u = r.temp ? (imperial_now ? "F" : "C") : r.unit;
  char b[32];
  float th[2] = {a.hi, a.lo};
  for (int i = 0; i < 2; i++) {
    if (std::isnan(th[i])) snprintf(b, sizeof(b), "OFF");
    else snprintf(b, sizeof(b), "%.*f %s", alert_dec(g_al_key), disp(r, th[i], imperial_now), u);
    lv_label_set_text(g_al_val[i], b);
  }
  uint8_t sel[3] = {a.c_ok, a.c_hi, a.c_lo};
  for (int row = 0; row < 3; row++)
    for (int i = 0; i < 6; i++)
      lv_obj_set_style_border_width(g_al_sw[row][i], i == sel[row] ? S(3) : 0, 0);
}
inline lv_obj_t *al_btn(lv_obj_t *pn, lv_coord_t x, lv_coord_t y, lv_coord_t w, const char *t,
                        lv_event_cb_t cb, intptr_t u, bool repeat) {
  lv_obj_t *bt = lv_btn_create(pn);
  lv_obj_set_size(bt, w, S(40));
  lv_obj_set_pos(bt, x, y);
  lv_obj_set_style_bg_color(bt, lv_color_hex(0x0A2416), 0);
  lv_obj_set_style_border_color(bt, lv_color_hex(0x006030), 0);
  lv_obj_set_style_border_width(bt, 1, 0);
  lv_obj_set_style_radius(bt, S(4), 0);
  lv_obj_set_style_shadow_width(bt, 0, 0);
  lv_obj_t *l = lv_label_create(bt);
  lv_obj_set_style_text_font(l, g_f_name, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(0x9AE6B4), 0);
  lv_label_set_text(l, t);
  lv_obj_center(l);
  // 按住 +/- 會連續調整:短按用 SHORT_CLICKED,避免放開時多算一次
  lv_obj_add_event_cb(bt, cb, repeat ? LV_EVENT_SHORT_CLICKED : LV_EVENT_CLICKED, (void *) u);
  if (repeat) lv_obj_add_event_cb(bt, cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *) u);
  return bt;
}
inline lv_obj_t *al_label(lv_obj_t *pn, lv_coord_t x, lv_coord_t y, const char *t, uint32_t col) {
  lv_obj_t *l = lv_label_create(pn);
  lv_obj_set_style_text_font(l, g_f_name, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
  lv_obj_set_pos(l, x, y);
  lv_label_set_text(l, t);
  return l;
}

inline void open_alert(int k) {
  g_al_key = k;
  const Reading &r = g_rd[k];
  char b[96];
  snprintf(b, sizeof(b), "ALERT  %s %s", r.src, r.name);
  lv_coord_t y;
  lv_obj_t *pn = picker_frame(b, y);
  lv_obj_t *tl = lv_obj_get_child(pn, 0);   // 標題可能很長(HA 名稱)
  lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(tl, LV_PCT(100));

  // 上限 / 下限:[-] [+] [OFF];code = which<<4 | op(0 減、1 加、2 關)
  auto adj = [](lv_event_t *e) {
    int code = (int) (intptr_t) lv_event_get_user_data(e);
    int which = code >> 4, op = code & 0xF;
    if (g_al_key < 0) return;
    Alert &a = g_al.a[g_al_key];
    const Reading &r = g_rd[g_al_key];
    float &t = which == 0 ? a.hi : a.lo;
    float st = alert_step(g_al_key);
    if (op == 2) {
      t = NAN;
    } else {
      if (std::isnan(t)) {   // 從 OFF 開始:以目前值為起點(對齊幅度)
        float base = fresh(r) ? r.value : 0.0f;
        t = roundf(base / st) * st;
      } else {
        t += op == 1 ? st : -st;
      }
      // 上限必須高於下限
      if (!std::isnan(a.hi) && !std::isnan(a.lo) && a.hi <= a.lo) {
        if (which == 0) a.hi = a.lo + st;
        else a.lo = a.hi - st;
      }
    }
    alert_save();
    alert_show();
    refresh(imperial_now);
  };
  static const char *const ROW[2] = {"HIGH", "LOW"};
  for (int w = 0; w < 2; w++) {
    al_label(pn, 0, y + S(8), ROW[w], 0x9FB8A6);
    g_al_val[w] = al_label(pn, S(70), y + S(8), "", 0x00E66E);
    al_btn(pn, S(220), y, S(60), "-", adj, (w << 4) | 0, true);
    al_btn(pn, S(288), y, S(60), "+", adj, (w << 4) | 1, true);
    al_btn(pn, S(356), y, S(64), "OFF", adj, (w << 4) | 2, false);
    y += S(50);
  }

  // 顏色:正常 / 超過上限 / 低於下限;code = row<<4 | 色號
  y += S(6);
  auto pick = [](lv_event_t *e) {
    int code = (int) (intptr_t) lv_event_get_user_data(e);
    int row = code >> 4, ci = code & 0xF;
    if (g_al_key < 0) return;
    Alert &a = g_al.a[g_al_key];
    (row == 0 ? a.c_ok : row == 1 ? a.c_hi : a.c_lo) = (uint8_t) ci;
    alert_save();
    alert_show();
    refresh(imperial_now);
  };
  static const char *const CROW[3] = {"NORMAL", "HIGH", "LOW"};
  for (int row = 0; row < 3; row++) {
    al_label(pn, 0, y + S(6), CROW[row], 0x9FB8A6);
    for (int i = 0; i < 6; i++) {
      lv_obj_t *sw = lv_obj_create(pn);
      lv_obj_remove_style_all(sw);
      lv_obj_set_size(sw, S(40), S(32));
      lv_obj_set_pos(sw, S(110) + i * S(50), y);
      lv_obj_set_style_bg_color(sw, lv_color_hex(PAL[i]), 0);
      lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(sw, S(4), 0);
      lv_obj_set_style_border_color(sw, lv_color_hex(0xFFFFFF), 0);
      lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_ext_click_area(sw, S(4));
      lv_obj_add_event_cb(sw, pick, LV_EVENT_CLICKED, (void *) (intptr_t) ((row << 4) | i));
      g_al_sw[row][i] = sw;
    }
    y += S(44);
  }

  y += S(6);
  al_btn(pn, 0, y, S(120), "RESET", [](lv_event_t *e) {
    if (g_al_key < 0) return;
    alert_reset(g_al_key);
    alert_show();
    refresh(imperial_now);
  }, 0, false);
  al_btn(pn, S(300), y, S(120), "DONE", [](lv_event_t *e) {
    g_al_key = -1;
    close_picker();
  }, 0, false);
  alert_show();
}

// 齒輪 → 該格的選單:AUTO + 目前有資料的數值(附目前值)+ HA SENSORS;點外面關閉
inline void open_picker(int slot) {
  char b[96];
  snprintf(b, sizeof(b), "SLOT %d", slot + 1);
  lv_coord_t y;
  lv_obj_t *pn = picker_frame(b, y);
  auto pick_cb = [](lv_event_t *e) {
    intptr_t code = (intptr_t) lv_event_get_user_data(e);
    int sl = (int) (code >> 8), val = (int) (code & 0xFF);
    close_picker();
    if (g_on_pick) g_on_pick(sl, val);
  };

  for (int o = 0; o <= KEY_N; o++) {
    int v = o == 0 ? 0 : ORDER[o - 1] + 1;
    if (v > 0 && !g_rd[v - 1].registered) continue;   // 只列有資料的
    if (v == 0) {
      snprintf(b, sizeof(b), "AUTO");
    } else {
      const Reading &r = g_rd[v - 1];
      if (fresh(r))
        snprintf(b, sizeof(b), "%s %s   %.*f %s", r.src, r.name, (int) r.decimals,
                 disp(r, r.value, imperial_now), r.temp ? (imperial_now ? "F" : "C") : r.unit);
      else
        snprintf(b, sizeof(b), "%s %s   --", r.src, r.name);
    }
    picker_row(pn, y, b, g_slot_cfg[slot] == v, pick_cb, (slot << 8) | v);
  }
  if (g_slot_key[slot] >= 0) {
    picker_row(pn, y, "ALERT ...", false, [](lv_event_t *e) {
      int sl = (int) (intptr_t) lv_event_get_user_data(e);
      if (g_slot_key[sl] >= 0) open_alert(g_slot_key[sl]);
    }, slot);
  }
  if (g_ha_enabled) {
    picker_row(pn, y, "HA SENSORS ...", false, [](lv_event_t *e) {
      int sl = (int) (intptr_t) lv_event_get_user_data(e);
      g_ha_pick_slot = sl;
      lv_coord_t yy;
      lv_obj_t *p2 = picker_frame("HA SENSORS", yy);
      lv_obj_t *l = lv_label_create(p2);
      lv_obj_set_style_text_font(l, g_f_name, 0);
      lv_obj_set_style_text_color(l, lv_color_hex(0x6E8273), 0);
      lv_obj_set_pos(l, 0, yy);
      lv_label_set_text(l, "LOADING ...");
      if (g_on_ha_list_req) g_on_ha_list_req();
    }, slot);
  }
}

// HA 類別:先看 device_class,沒有就看單位(MIoT 等整合常沒填 device_class)
enum HaCat : uint8_t { HC_TEMP, HC_HUM, HC_CO2, HC_PM, HC_PRES, HC_POWER, HC_ENERGY, HC_BATT, HC_LIGHT, HC_OTHER, HC_N };
static const char *const HA_CAT_NAME[HC_N] = {"TEMPERATURE", "HUMIDITY", "CO2 / PPM", "PM2.5 / PM10", "PRESSURE",
                                              "POWER", "ENERGY", "BATTERY", "ILLUMINANCE", "OTHER"};
inline int ha_cat(const HaEnt &h) {
  const std::string &d = h.dc, &u = h.unit;
  if (d == "temperature" || u == "°C" || u == "°F") return HC_TEMP;
  if (d == "humidity") return HC_HUM;
  if (d == "carbon_dioxide" || d == "volatile_organic_compounds_parts" || u == "ppm") return HC_CO2;
  if (d == "pm25" || d == "pm10" || d == "pm1" || u == "µg/m³" || u == "μg/m³") return HC_PM;
  if (d == "pressure" || d == "atmospheric_pressure" || u == "hPa" || u == "kPa" || u == "Pa" ||
      u == "mbar" || u == "inHg" || u == "mmHg") return HC_PRES;
  if (d == "power" || u == "W" || u == "kW") return HC_POWER;
  if (d == "energy" || u == "kWh" || u == "Wh" || u == "MWh") return HC_ENERGY;
  if (d == "battery") return HC_BATT;
  if (d == "illuminance" || u == "lx") return HC_LIGHT;
  // 沒 device_class 的 %:名稱/實體 ID 看起來像濕度才歸濕度
  if (u == "%") {
    std::string s = h.id + " " + h.name;
    for (auto &ch : s) ch = (char) tolower((unsigned char) ch);
    if (s.find("humid") != std::string::npos || s.find("濕") != std::string::npos ||
        s.find("湿") != std::string::npos) return HC_HUM;
    if (s.find("battery") != std::string::npos || s.find("電量") != std::string::npos ||
        s.find("电量") != std::string::npos) return HC_BATT;
  }
  return HC_OTHER;
}

inline void ha_cat_menu();

// HA 清單的一頁(換頁按鈕在選單自己的事件裡重畫,picker_frame 會延後刪舊的)
inline void ha_list_page(int page) {
  int n = (int) g_ha_idx.size(), pages = (n + HA_PAGE - 1) / HA_PAGE;
  if (pages < 1) pages = 1;
  if (page < 0) page = 0;
  if (page >= pages) page = pages - 1;
  g_ha_page = page;
  int a = page * HA_PAGE, e = a + HA_PAGE < n ? a + HA_PAGE : n;
  char b[128];
  snprintf(b, sizeof(b), "SLOT %d  %s  %d-%d / %d", g_ha_pick_slot + 1,
           g_ha_cat < 0 ? "ALL" : HA_CAT_NAME[g_ha_cat], n ? a + 1 : 0, e, n);
  lv_coord_t y;
  lv_obj_t *pn = picker_frame(b, y);
  picker_row(pn, y, LV_SYMBOL_LEFT "  CATEGORIES", false, [](lv_event_t *ev) { ha_cat_menu(); }, 0);
  auto nav = [](lv_event_t *ev) { ha_list_page(g_ha_page + (int) (intptr_t) lv_event_get_user_data(ev)); };
  if (page > 0) picker_row(pn, y, LV_SYMBOL_UP "  PREV", false, nav, -1);
  for (int i = a; i < e; i++) {
    HaEnt h = ha_at(g_ha_idx[i]);
    char u[12];
    ascii_unit(u, sizeof(u), h.unit.c_str());
    snprintf(b, sizeof(b), "%s   %s %s", h.name.c_str(), h.state.c_str(), u);
    picker_row(pn, y, b, false, [](lv_event_t *ev) {
      size_t idx = (size_t) (intptr_t) lv_event_get_user_data(ev);
      int sl = g_ha_pick_slot;
      g_ha_pick_slot = -1;
      close_picker();
      if (sl >= 0 && idx < g_ha_off.size() && g_on_ha_pick) g_on_ha_pick(sl, ha_at(idx));
    }, (intptr_t) g_ha_idx[i]);
  }
  if (page < pages - 1) picker_row(pn, y, LV_SYMBOL_DOWN "  NEXT", false, nav, 1);
}

// 類別選單:只列有東西的類別,附筆數;ALL 在最上面
inline void ha_cat_menu() {
  int cnt[HC_N] = {0};
  for (uint8_t c : g_ha_catv) cnt[c]++;
  char b[64];
  snprintf(b, sizeof(b), "SLOT %d  HA SENSORS", g_ha_pick_slot + 1);
  lv_coord_t y;
  lv_obj_t *pn = picker_frame(b, y);
  auto go = [](lv_event_t *ev) {
    int c = (int) (intptr_t) lv_event_get_user_data(ev);
    g_ha_cat = c;
    g_ha_idx.clear();
    for (size_t i = 0; i < g_ha_catv.size(); i++)
      if (c < 0 || g_ha_catv[i] == c) g_ha_idx.push_back((uint16_t) i);
    ha_list_page(0);
  };
  snprintf(b, sizeof(b), "ALL   (%d)", (int) g_ha_catv.size());
  picker_row(pn, y, b, g_ha_cat < 0, go, -1);
  for (int c = 0; c < HC_N; c++) {
    if (!cnt[c]) continue;
    snprintf(b, sizeof(b), "%s   (%d)", HA_CAT_NAME[c], cnt[c]);
    picker_row(pn, y, b, g_ha_cat == c, go, c);
  }
}

// HA 清單回來(主迴圈呼叫):還在挑才顯示;status 非 200 顯示錯誤
inline void show_ha_list(const std::string &raw, int status) {
  if (g_ha_pick_slot < 0 || !g_pick) { g_ha_pick_slot = -1; return; }
  g_ha_raw = raw;
  g_ha_off.clear();
  for (size_t p = 0; p < g_ha_raw.size();) {
    size_t e = g_ha_raw.find('\n', p);
    if (e == std::string::npos) e = g_ha_raw.size();
    if (e > p && std::count(g_ha_raw.begin() + p, g_ha_raw.begin() + e, '|') >= 3) g_ha_off.push_back(p);
    p = e + 1;
  }
  g_ha_catv.clear();
  for (size_t i = 0; i < g_ha_off.size(); i++) g_ha_catv.push_back((uint8_t) ha_cat(ha_at(i)));
  if (status != 200 || g_ha_off.empty()) {
    char b[64];
    lv_coord_t y;
    lv_obj_t *pn = picker_frame("HA SENSORS", y);
    lv_obj_t *l = lv_label_create(pn);
    lv_obj_set_style_text_font(l, g_f_name, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFF7800), 0);
    lv_obj_set_pos(l, 0, y);
    if (status == 401) snprintf(b, sizeof(b), "HA TOKEN REJECTED (401)");
    else if (status != 200) snprintf(b, sizeof(b), "HA NOT REACHABLE (%d)", status);
    else snprintf(b, sizeof(b), "NO NUMERIC SENSORS");
    lv_label_set_text(l, b);
    g_ha_pick_slot = -1;
    return;
  }
  g_ha_cat = -1;
  ha_cat_menu();
}

// page:時鐘頁;f_name/f_val/f_tiny 從 yaml 隱藏標籤取得的字型
inline void build(lv_obj_t *page, int W, int H, const lv_font_t *f_name, const lv_font_t *f_val,
                  const lv_font_t *f_tiny, const lv_font_t *f_sym) {
  init();
  if (g_built) return;
  g_built = true;
  g_page = page;
  g_f_name = f_name;
  g_W = W; g_H = H;
  g_s = (float) W / 800.0f;
  for (int i = 0; i < SLOT_MAX; i++) {
    Card &c = g_card[i];
    c.box = lv_obj_create(page);
    lv_obj_clear_flag(c.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(c.box, LV_OBJ_FLAG_CLICKABLE);   // 點卡片 = 點頁面(回雷達)
    lv_obj_set_style_bg_color(c.box, lv_color_hex(0x07140D), 0);
    lv_obj_set_style_bg_opa(c.box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c.box, lv_color_hex(0x1E4A32), 0);
    lv_obj_set_style_border_width(c.box, 1, 0);
    lv_obj_set_style_radius(c.box, S(6), 0);
    lv_obj_set_style_pad_all(c.box, 0, 0);
    c.name = mk_label(c.box, f_name, 0x9FB8A6);
    c.tag = mk_label(c.box, f_tiny, 0x6E8273);
    lv_obj_set_style_border_color(c.tag, lv_color_hex(0x2E6A48), 0);
    lv_obj_set_style_border_width(c.tag, 1, 0);
    lv_obj_set_style_radius(c.tag, 3, 0);
    lv_obj_set_style_pad_hor(c.tag, S(5), 0);
    // 右上角齒輪:點了開這一格的選單(觸控範圍往外擴,小螢幕也好點)
    c.gear = mk_label(c.box, f_sym, 0x6E8273);
    lv_label_set_text(c.gear, LV_SYMBOL_SETTINGS);
    lv_obj_add_flag(c.gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(c.gear, S(14));
    lv_obj_add_event_cb(c.gear, [](lv_event_t *e) {
      open_picker((int) (intptr_t) lv_event_get_user_data(e));
    }, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    c.val = mk_label(c.box, f_val, 0x00E66E);
    c.unit = mk_label(c.box, f_name, 0x6E8273);
    c.hi = mk_label(c.box, f_tiny, 0x4E6A58);
    c.lo = mk_label(c.box, f_tiny, 0x4E6A58);
    c.line = lv_line_create(c.box);
    lv_obj_set_style_line_width(c.line, S(1.5f) < 1 ? 1 : S(1.5f), 0);
    lv_obj_set_style_line_color(c.line, lv_color_hex(0x00E66E), 0);
    lv_obj_set_style_line_rounded(c.line, true, 0);
    c.t0 = mk_label(c.box, f_tiny, 0x3E5A48);
    c.t1 = mk_label(c.box, f_tiny, 0x3E5A48);
    lv_label_set_text(c.t0, "-24h");
    lv_label_set_text(c.t1, "now");
    lv_obj_add_flag(c.box, LV_OBJ_FLAG_HIDDEN);
    g_slot_key[i] = -1;
  }
}

// ---- 頂端時鐘:一般模式 / 大時鐘模式(點時鐘或網頁 Large Clock 切換)----
// 一般:左上時間,日期放右邊按鈕下方的空位;大:時間置中放大,下方日期,卡片收成一列
inline bool g_big = false;
inline lv_coord_t g_top = 0;            // 卡片區起點 y(set_big 算好,layout 用)
inline lv_obj_t *g_time_l = nullptr, *g_date_l = nullptr, *g_sep = nullptr;
inline const lv_font_t *g_f_clk = nullptr, *g_f_xl = nullptr;

// 時鐘頁日期:Nov. 27 2026 (Fri)(日不補零;newlib 的 strftime 沒有 %-d)
inline std::string date_str(esphome::ESPTime t) {   // ESPTime::strftime 不是 const
  if (!t.is_valid()) return "";
  char b[32];
  snprintf(b, sizeof(b), "%s. %d %s", t.strftime("%b").c_str(), t.day_of_month, t.strftime("%Y (%a)").c_str());
  return b;
}

// ---- 卡片區模式:SENSOR(感測器曲線)/ CTRL(HA 裝置控制,ctrl_hub.h,PLAN_CTRL.md)----
// 兩顆互斥按鈕 + 卡片區左右滑動切換;格數共用 GRID。實際切換走 yaml 的 select(存 NVS、同步網頁)。
inline bool g_ctrl = false;
inline int g_ctl_n = 0;                                   // 已設定的控制卡片數(CTRL 的 AUTO 用)
inline int g_ctl_shown = -1;                              // 目前排出的控制卡片數(-1 = 要重排)
inline void (*g_on_mode)(bool ctrl) = nullptr;            // 按鈕/滑動 → yaml
inline void (*g_ctrl_place)(int n, const lv_area_t *r) = nullptr;   // ctrl_hub 排自己的卡片;n=0 全藏
inline lv_obj_t *g_mb[2] = {nullptr, nullptr};            // [0]=SENSOR [1]=CTRL
inline uint32_t g_gesture_ms = 0;                         // 最近一次滑動(卡片按鈕忽略緊接著的點擊)

inline void mode_style() {
  for (int i = 0; i < 2; i++) {
    if (!g_mb[i]) continue;
    bool on = (i == 1) == g_ctrl;
    lv_obj_set_style_bg_color(g_mb[i], lv_color_hex(on ? 0x00C060 : 0x040C08), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(g_mb[i], 0), lv_color_hex(on ? 0x04180C : 0x00C060), 0);
  }
}

inline void clock_init(lv_obj_t *time_l, lv_obj_t *date_l, lv_obj_t *sep, const lv_font_t *f_clk,
                       const lv_font_t *f_xl) {
  g_time_l = time_l; g_date_l = date_l; g_sep = sep;
  g_f_clk = f_clk; g_f_xl = f_xl;
  static const char *const TXT[2] = {"SENSOR", "CTRL"};
  for (int i = 0; i < 2; i++) {
    lv_obj_t *b = lv_btn_create(g_page);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x00A050), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, g_f_name, 0);
    lv_label_set_text(l, TXT[i]);
    lv_obj_center(l);
    lv_obj_set_ext_click_area(b, S(4));
    lv_obj_add_event_cb(b, [](lv_event_t *e) {
      bool want = (bool) (intptr_t) lv_event_get_user_data(e);
      if (want != g_ctrl && g_on_mode) g_on_mode(want);
    }, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    g_mb[i] = b;
  }
  mode_style();
  // 卡片區左右滑動:往左 → CTRL,往右 → SENSOR(只認卡片區,時鐘那一帶不算)
  lv_obj_add_event_cb(g_page, [](lv_event_t *e) {
    lv_indev_t *in = lv_indev_get_act();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (p.y < g_top) return;
    lv_dir_t d = lv_indev_get_gesture_dir(in);
    if (d != LV_DIR_LEFT && d != LV_DIR_RIGHT) return;
    g_gesture_ms = millis();
    bool want = (d == LV_DIR_LEFT);
    if (want != g_ctrl && g_on_mode) g_on_mode(want);
  }, LV_EVENT_GESTURE, nullptr);
}

// 切換卡片區模式(yaml 的 select 呼叫);另一組卡片馬上藏起來,下次 layout() 重排
inline void set_mode(bool ctrl) {
  g_ctrl = ctrl;
  mode_style();
  if (ctrl) {
    for (int i = 0; i < SLOT_MAX; i++) if (g_card[i].box) lv_obj_add_flag(g_card[i].box, LV_OBJ_FLAG_HIDDEN);
  } else if (g_ctrl_place) {
    g_ctrl_place(0, nullptr);
  }
  g_shown = -1;
  g_ctl_shown = -1;
}

inline void set_big(bool big) {
  g_big = big;
  g_top = S(122);
  if (!g_time_l || !g_built) return;
  const lv_font_t *fd = lv_obj_get_style_text_font(g_date_l, LV_PART_MAIN);
  if (big) {
    // 時間的字框上緣有一段留白,往上提一點才不會跟按鈕列距離太遠
    lv_obj_set_style_text_font(g_time_l, g_f_xl, 0);
    lv_obj_align(g_time_l, LV_ALIGN_TOP_MID, 0, S(40));
    lv_coord_t yd = S(40) + g_f_xl->line_height - S(14);
    lv_obj_align(g_date_l, LV_ALIGN_TOP_MID, 0, yd);
    g_top = yd + fd->line_height + S(10);
    lv_obj_add_flag(g_sep, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_set_style_text_font(g_time_l, g_f_clk, 0);
    lv_obj_align(g_time_l, LV_ALIGN_TOP_LEFT, S(22), S(6));
    lv_obj_align(g_date_l, LV_ALIGN_TOP_RIGHT, -S(16), S(62));   // 右緣對齊 RADAR 鈕
    lv_obj_clear_flag(g_sep, LV_OBJ_FLAG_HIDDEN);
  }
  // SENSOR / CTRL:一般模式在時間正下方(原本日期的位置);大時鐘時左上角是空的,移到按鈕列
  for (int i = 0; i < 2; i++) {
    if (!g_mb[i]) continue;
    lv_obj_set_size(g_mb[i], S(84), big ? S(32) : S(28));
    lv_obj_set_pos(g_mb[i], S(22) + i * S(92), big ? S(20) : S(84));
  }
  g_shown = -1;   // 卡片區高度變了,下次 layout() 一定重排
  g_ctl_shown = -1;
}

// 格數 → 欄列
inline void grid_of(int n, int &cols, int &rows) {
  switch (n) {
    case 1: cols = 1; rows = 1; break;
    case 2: cols = 2; rows = 1; break;
    case 3: cols = 3; rows = 1; break;
    case 4: cols = 2; rows = 2; break;
    default: cols = 3; rows = 2; break;
  }
}

// n 格的每格位置(SENSOR / CTRL 共用)
inline void slot_rects(int n, lv_area_t *r) {
  int cols, rows;
  grid_of(n, cols, rows);
  const lv_coord_t pad = S(8), top = g_top > 0 ? g_top : S(122);
  const lv_coord_t gw = g_W - 2 * pad, gh = g_H - top - pad;
  const lv_coord_t cw = (gw - (cols - 1) * pad) / cols;
  const lv_coord_t ch = (gh - (rows - 1) * pad) / rows;
  for (int i = 0; i < n; i++) {
    r[i].x1 = pad + (i % cols) * (cw + pad);
    r[i].y1 = top + (i / cols) * (ch + pad);
    r[i].x2 = r[i].x1 + cw - 1;
    r[i].y2 = r[i].y1 + ch - 1;
  }
}

// CTRL 模式:格數 = GRID 指定,AUTO = 已設定的控制卡片數(至少 1)
inline bool layout_ctrl(int mode) {
  int n = (mode >= 1 && mode <= SLOT_MAX) ? mode : (g_ctl_n < 1 ? 1 : g_ctl_n);
  if (n > SLOT_MAX) n = SLOT_MAX;
  if (n == 5) n = 6;
  if (g_big && n > 3) n = 3;
  if (n == g_ctl_shown || !g_ctrl_place) return false;
  lv_area_t r[SLOT_MAX];
  slot_rects(n, r);
  g_ctrl_place(n, r);
  g_ctl_shown = n;
  return true;
}

// mode:0=AUTO,其餘 = 指定格數(1/2/3/4/6);pin[i]:0=自動,1..KEY_N = 指定 key+1
// 回傳 true = 版面有變(呼叫端接著 refresh)
inline bool layout(int mode, const int *pin) {
  if (!g_built) return false;
  if (g_ctrl) return layout_ctrl(mode);
  // 自動清單:登記過、且沒被任何一格手動指定的 key,依 enum 順序
  int autos[KEY_N], na = 0;
  for (int o = 0; o < KEY_N; o++) {
    int k = ORDER[o];
    if (!g_rd[k].registered) continue;
    bool pinned = false;
    for (int i = 0; i < SLOT_MAX; i++) if (pin[i] == k + 1) pinned = true;
    if (!pinned) autos[na++] = k;
  }
  int npin = 0;
  for (int i = 0; i < SLOT_MAX; i++) if (pin[i] > 0 && pin[i] <= KEY_N) npin++;
  int n;
  if (mode >= 1 && mode <= SLOT_MAX) n = mode;
  else {
    // AUTO:手動指定的格 + 有資料的自動數值,最少 1 格、最多 6 格;5 格時排 3×2
    n = npin + na;
    if (n < 1) n = 1;
    if (n > SLOT_MAX) n = SLOT_MAX;
  }
  if (n == 5) n = 6;
  if (g_big && n > 3) n = 3;   // 大時鐘:只排一列
  int keys[SLOT_MAX], ai = 0;
  for (int i = 0; i < SLOT_MAX; i++) {
    if (i >= n) { keys[i] = -1; continue; }
    if (pin[i] > 0 && pin[i] <= KEY_N) keys[i] = pin[i] - 1;
    else keys[i] = ai < na ? autos[ai++] : -1;
  }
  for (int i = 0; i < SLOT_MAX; i++) g_slot_cfg[i] = (pin[i] > 0 && pin[i] <= KEY_N) ? pin[i] : 0;
  bool same = (n == g_shown);
  for (int i = 0; i < SLOT_MAX && same; i++) if (keys[i] != g_slot_key[i]) same = false;
  if (same) return false;

  int cols, rows;
  grid_of(n, cols, rows);
  const lv_coord_t pad = S(8), top = g_top > 0 ? g_top : S(122);
  const lv_coord_t gw = g_W - 2 * pad, gh = g_H - top - pad;
  const lv_coord_t cw = (gw - (cols - 1) * pad) / cols;
  const lv_coord_t ch = (gh - (rows - 1) * pad) / rows;
  for (int i = 0; i < SLOT_MAX; i++) {
    Card &c = g_card[i];
    g_slot_key[i] = keys[i];
    if (i >= n) { lv_obj_add_flag(c.box, LV_OBJ_FLAG_HIDDEN); continue; }
    lv_obj_clear_flag(c.box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(c.box, pad + (i % cols) * (cw + pad), top + (i / cols) * (ch + pad));
    lv_obj_set_size(c.box, cw, ch);
    lv_label_set_long_mode(c.name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(c.name, cw - S(110));   // 右邊留給來源標籤與齒輪
    lv_obj_align(c.name, LV_ALIGN_TOP_LEFT, S(10), S(6));
    lv_obj_align(c.gear, LV_ALIGN_TOP_RIGHT, -S(8), S(6));
    lv_obj_align(c.val, LV_ALIGN_TOP_LEFT, S(10), S(32));
    lv_obj_align(c.hi, LV_ALIGN_TOP_RIGHT, -S(8), S(36));
    lv_obj_align(c.lo, LV_ALIGN_TOP_RIGHT, -S(8), S(54));
    lv_obj_align(c.t0, LV_ALIGN_BOTTOM_LEFT, S(10), -S(2));
    lv_obj_align(c.t1, LV_ALIGN_BOTTOM_RIGHT, -S(8), -S(2));
    // 曲線區:卡片下半部,大卡片給更高的曲線
    lv_coord_t sh = ch - S(96);
    if (sh < S(40)) sh = S(40);
    lv_obj_set_pos(c.line, 0, ch - S(20) - sh);
    lv_obj_set_size(c.line, cw, sh);
  }
  g_shown = n;
  return true;
}

inline uint32_t color_of(int k, float v) {
  const Alert &a = g_al.a[k];
  int st = alert_state(k, v);
  return PAL[st > 0 ? a.c_hi : st < 0 ? a.c_lo : a.c_ok];
}

// 數值、標籤、曲線全部重畫(頁面顯示中才呼叫;不在畫面上時 LVGL 不會重繪)
inline void refresh(bool imperial) {
  if (!g_built || g_ctrl) return;   // CTRL 模式時感測卡片藏著
  imperial_now = imperial;
  char b[32];
  for (int i = 0; i < SLOT_MAX; i++) {
    Card &c = g_card[i];
    int k = g_slot_key[i];
    if (i >= g_shown) break;
    if (k < 0) {
      set_text(c.name, "");
      set_text(c.tag, "");
      set_text(c.val, "--");
      card_style(c, PAL[0], 0x1E4A32, 1);
      set_text(c.unit, "");
      set_text(c.hi, "");
      set_text(c.lo, "");
      set_line(c, c.pts, 0);
      continue;
    }
    const Reading &r = g_rd[k];
    set_text(c.name, r.name);
    set_text(c.tag, r.src);
    lv_obj_align_to(c.tag, c.gear, LV_ALIGN_OUT_LEFT_MID, -S(8), 0);
    bool ok = fresh(r);
    float v = ok ? disp(r, r.value, imperial) : NAN;
    if (ok) snprintf(b, sizeof(b), "%.*f", (int) r.decimals, v);
    else snprintf(b, sizeof(b), "--");
    set_text(c.val, b);
    uint32_t col = color_of(k, ok ? r.value : NAN);
    bool alarm = ok && alert_state(k, r.value) != 0;   // 超標:外框也換色、加粗
    card_style(c, col, alarm ? col : 0x1E4A32, alarm ? S(3) : 1);
    if (r.temp) set_text(c.unit, imperial ? "F" : "C");
    else set_text(c.unit, r.unit);
    lv_obj_align_to(c.unit, c.val, LV_ALIGN_OUT_RIGHT_BOTTOM, S(4), -S(4));

    // 曲線:時間窗 = 已有的資料(至少 1 小時、最多 24 小時),平均成 96 點,最後一點接目前值。
    // 剛開機或剛選的數值不必等滿 24 小時才看得到變化。
    int W = r.count < 60 ? 60 : r.count;
    if (W >= 120) snprintf(b, sizeof(b), "-%dh", (W + 30) / 60);
    else snprintf(b, sizeof(b), "-%dm", W);
    set_text(c.t0, b);
    float avg[SPARK_N];
    float lo = NAN, hi = NAN;
    for (int j = 0; j < SPARK_N; j++) {
      float s = 0; int n = 0;
      int qa = j * W / SPARK_N, qb = (j + 1) * W / SPARK_N;
      if (qb <= qa) qb = qa + 1;
      for (int q = qa; q < qb; q++) {
        int idx = (r.head - W + q + 2 * HIST_N) % HIST_N;
        float h = r.hist ? r.hist[idx] : NAN;
        if (!std::isnan(h)) { s += h; n++; }
      }
      avg[j] = n ? s / n : NAN;
    }
    if (ok) avg[SPARK_N - 1] = r.value;
    for (int j = 0; j < SPARK_N; j++) {
      if (std::isnan(avg[j])) continue;
      if (std::isnan(lo) || avg[j] < lo) lo = avg[j];
      if (std::isnan(hi) || avg[j] > hi) hi = avg[j];
    }
    if (std::isnan(lo)) {
      set_text(c.hi, "");
      set_text(c.lo, "");
      set_line(c, c.pts, 0);
      continue;
    }
    snprintf(b, sizeof(b), "MAX %.*f", (int) r.decimals, disp(r, hi, imperial));
    set_text(c.hi, b);
    snprintf(b, sizeof(b), "MIN %.*f", (int) r.decimals, disp(r, lo, imperial));
    set_text(c.lo, b);
    float span = hi - lo;
    if (span < 1e-3f) span = 1.0f;   // 平線:畫在中間
    lv_coord_t w = lv_obj_get_width(c.line), h = lv_obj_get_height(c.line) - 2;
    // lv_line 不能斷開:沒資料的時段直接跳過,前後兩點連起來
    spark_pt_t pts[SPARK_N];   // 先算到暫存,跟目前的比過有變才換
    int np = 0;
    for (int j = 0; j < SPARK_N; j++) {
      if (std::isnan(avg[j])) continue;
      pts[np].x = (lv_coord_t) (j * (w - 1) / (SPARK_N - 1));
      float t = (hi - lo < 1e-3f) ? 0.5f : (avg[j] - lo) / span;
      pts[np].y = (lv_coord_t) (1 + h - t * h);
      np++;
    }
    set_line(c, pts, np >= 2 ? np : 0);
  }
}

}  // namespace shub
