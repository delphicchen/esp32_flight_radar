#pragma once
// 時鐘頁 CTRL 模式:Home Assistant 裝置控制卡片(燈光 / 窗簾 / 空調 / 風扇),規劃見 PLAN_CTRL.md。
// 格子位置由 shub::layout_ctrl() 算好後交給 ctrl::place();卡片全部在 C++ 建立,
// 字型、縮放 S()、選單外框沿用 sensors_hub.h。兩個分支共用(LVGL 8 / 9)。
//
// 第 i 格固定對應網頁的 *Control 1..6*(text,存 entity_id)。網路一律走 radar_bg 背景 task:
// 清單 = Job 11,狀態 = Job 12(頁面顯示中每 5 秒);結果由主迴圈交給 show_list() / apply()。
// 操作 = Job 13(服務呼叫):act() 先改畫面(樂觀更新)、排進 g_pend,tick() 到時間才送(− / + 去抖動),
// 結果回來 call_done():成功 → 0.7 秒後回讀確認;失敗或 4 秒沒回應 → 恢復原狀並在卡片上顯示錯誤。
#include "sensors_hub.h"

namespace ctrl {

using shub::S;

enum Dom : uint8_t { D_NONE = 0, D_LIGHT, D_COVER, D_CLIMATE, D_FAN, D_N };
static const char *const DOM_PREFIX[D_N] = {"", "light.", "cover.", "climate.", "fan."};
static const char *const DOM_NAME[D_N] = {"", "LIGHT", "COVER", "CLIMATE", "FAN"};

inline Dom dom_of(const std::string &id) {
  for (int d = 1; d < D_N; d++)
    if (id.compare(0, strlen(DOM_PREFIX[d]), DOM_PREFIX[d]) == 0) return (Dom) d;
  return D_NONE;
}

// 一個實體目前的狀態(Job 12 的一行);數值沒有就是 NAN
struct Ent {
  std::string id, name, state, modes, cmodes;   // modes = hvac_modes,cmodes = supported_color_modes
  Dom dom = D_NONE;
  float bri = NAN, pos = NAN, cur_t = NAN, tgt_t = NAN, tmin = NAN, tmax = NAN, tstep = NAN;
  float pct = NAN, pstep = NAN;
  int osc = -1;          // -1 = 不支援 / 不知道,0 / 1
  uint32_t feat = 0;     // supported_features
  bool valid = false;    // 收過至少一次狀態
  float ct = NAN, ctmin = NAN, ctmax = NAN;   // 燈的色溫(K)與範圍
  std::string effects, effect;               // 燈的模式清單(; 分隔)與目前模式
  std::string err;       // 最近一次操作失敗的訊息(顯示 4 秒)
  uint32_t err_ms = 0;
};
inline Ent g_ent[shub::SLOT_MAX];

struct Tile {
  lv_obj_t *box, *add, *name, *tag, *gear, *state, *val, *unit, *sub;
  lv_obj_t *btn[6], *btl[6];   // 下方按鈕列:窗簾 OPEN STOP CLOSE、空調的模式
  std::string bcmd[6];          // 每顆按鈕代表的指令(窗簾 open/stop/close、空調 hvac_mode)
  lv_obj_t *sl;                 // 滑桿:燈亮度、風扇風速、窗簾位置、空調設定溫度(×10);拖動中預覽,放開才送
  bool drag = false;            // 拖動中:狀態回讀不要把滑桿拉回去
  uint32_t c_state = 0xFFFFFFFF, c_val = 0xFFFFFFFF, c_box = 0xFFFFFFFF, c_sub = 0xFFFFFFFF;
};
inline Tile g_tile[shub::SLOT_MAX];
inline bool g_built = false;
inline int g_n = 0;                          // 目前排出的格數
inline const lv_font_t *g_f_val = nullptr, *g_f_mid = nullptr, *g_f_tiny = nullptr;

inline bool g_ha_ok = false;                                 // 有 HA URL / token
inline void (*g_on_pick)(int slot, const std::string &id) = nullptr;   // yaml:寫 Control N(空字串 = 移除)
inline void (*g_on_list_req)() = nullptr;                    // yaml:發清單請求
inline void (*g_on_poll)() = nullptr;                        // yaml:立刻抓一次狀態

// AUTO 格數 = 最後一個有設定的格 + 1,再多留一格「+ ADD」(最多 6)
inline void recount() {
  int last = -1;
  for (int i = 0; i < shub::SLOT_MAX; i++) if (!g_ent[i].id.empty()) last = i;
  int n = last + 2;
  shub::g_ctl_n = n > shub::SLOT_MAX ? shub::SLOT_MAX : n;
  shub::g_ctl_shown = -1;
}

// 逗號分隔的已設定 entity_id(給 Job 12)
inline std::string ids_csv() {
  std::string s;
  for (auto &e : g_ent) {
    if (e.id.empty()) continue;
    if (!s.empty()) s += ",";
    s += e.id;
  }
  return s;
}

// entity_id 合法字元(模板裡直接用單引號包,不能放進引號或跳脫字元)
inline bool id_ok(const std::string &id) {
  if (id.empty() || id.find('.') == std::string::npos) return false;
  for (char c : id) if (!(isalnum((unsigned char) c) || c == '_' || c == '.')) return false;
  return true;
}

// ---------------- 功能旗標(HA supported_features)----------------
inline bool light_dim(const Ent &e) {   // 可調亮度:color modes 不只 onoff;舊整合看 feature bit 1 或有亮度值
  return !e.cmodes.empty() ? e.cmodes != "onoff" : ((e.feat & 1) || !std::isnan(e.bri));
}
inline bool light_ct(const Ent &e) { return e.cmodes.find("color_temp") != std::string::npos && !std::isnan(e.ctmin); }
inline bool fan_speed(const Ent &e) { return e.feat & 1; }    // FanEntityFeature.SET_SPEED
inline bool fan_osc(const Ent &e) { return e.feat & 2; }      // OSCILLATE
// 窗簾:有些整合不回報 supported_features(= 0),那就全部給;位置看有沒有 current_position
inline bool cov_open(const Ent &e) { return !e.feat || (e.feat & 1); }    // CoverEntityFeature.OPEN
inline bool cov_close(const Ent &e) { return !e.feat || (e.feat & 2); }   // CLOSE
inline bool cov_pos(const Ent &e) { return (e.feat & 4) || (!e.feat && !std::isnan(e.pos)); }   // SET_POSITION
inline bool cov_stop(const Ent &e) { return !e.feat || (e.feat & 8); }    // STOP

// ---------------- 操作(Job 13)----------------
struct Pend {
  bool active = false;     // 有操作還沒確認(期間狀態輪詢不覆寫這格)
  Ent snap;                // 操作前的狀態,失敗時還原
  std::string path, body;  // 最新一次要送的服務與 JSON
  uint32_t seq = 0, sent_seq = 0, due = 0, sent_ms = 0;
};
inline Pend g_pend[shub::SLOT_MAX];
inline uint32_t g_poll_due = 0;   // 操作成功後回讀狀態的時間(0 = 沒排)
// yaml:把呼叫排進背景 task(插隊);佇列滿回 false,tick() 下一輪重送
inline bool (*g_send)(int src, const std::string &path, const std::string &body) = nullptr;

inline void render(int i);

// 排一個操作:delay_ms 內再來一次只送最後一次(− / + 連按、滑桿)。呼叫端接著改 g_ent 做樂觀更新。
inline void act(int slot, const char *path, const std::string &body, uint32_t delay_ms = 0) {
  Pend &p = g_pend[slot];
  if (!p.active) { p.snap = g_ent[slot]; p.active = true; }
  p.path = path;
  p.body = body;
  p.seq++;
  p.due = millis() + delay_ms;
  g_ent[slot].err.clear();
}

inline void rollback(int slot, const char *why) {
  Pend &p = g_pend[slot];
  if (!p.active) return;
  g_ent[slot] = p.snap;
  g_ent[slot].err = why;
  g_ent[slot].err_ms = millis();
  p.active = false;
  render(slot);
}

// 背景 task 回報(主迴圈呼叫):src = 格號*1000 + 序號
inline std::string g_call_dbg;   // 給網頁 Control Last Call 看
inline void call_done(int src, int status) {
  int slot = src / 1000;
  if (slot < 0 || slot >= shub::SLOT_MAX) return;
  Pend &p = g_pend[slot];
  g_call_dbg = p.path + " " + p.body + " -> HTTP " + std::to_string(status);
  if (!p.active) return;
  if (status != 200 && status != 201) {
    char b[24];
    snprintf(b, sizeof(b), status == 401 ? "TOKEN REJECTED" : "FAILED (%d)", status);
    rollback(slot, b);
    return;
  }
  if ((uint32_t) (src % 1000) != p.seq % 1000) return;   // 之後還有更新的操作,等它
  p.active = false;
  g_poll_due = millis() + 700;   // HA 狀態更新需要一點時間
  render(slot);
}

// 主迴圈每 100ms:送到期的操作、逾時還原、錯誤訊息到期、回讀狀態
inline void tick() {
  uint32_t now = millis();
  for (int i = 0; i < shub::SLOT_MAX; i++) {
    Pend &p = g_pend[i];
    if (p.active && p.sent_seq != p.seq && (int32_t) (now - p.due) >= 0) {
      if (g_send && g_send(i * 1000 + (int) (p.seq % 1000), p.path, p.body)) {
        p.sent_seq = p.seq;
        p.sent_ms = now;
      } else if (now - p.due > 4000) {
        rollback(i, "NOT SENT");   // 沒網路 / 沒 token / 佇列一直滿
      }
    } else if (p.active && p.sent_seq == p.seq && now - p.sent_ms > 4000) {
      rollback(i, "NO RESPONSE");
    }
    Ent &e = g_ent[i];
    if (!e.err.empty() && now - e.err_ms > 4000) { e.err.clear(); render(i); }
  }
  if (g_poll_due && (int32_t) (now - g_poll_due) >= 0) {
    g_poll_due = 0;
    if (g_on_poll) g_on_poll();
  }
}

inline std::string body_id(const Ent &e, const char *extra = nullptr) {
  std::string b = "{\"entity_id\":\"" + e.id + "\"";
  if (extra) { b += ","; b += extra; }
  return b + "}";
}

// 這張卡片要不要滑桿(功能支援、有狀態、空調要在有設定溫度的模式)
inline bool has_slider(const Ent &e) {
  if (!e.valid || e.state == "unavailable" || e.state == "unknown") return false;
  if (e.dom == D_LIGHT) return light_dim(e);
  if (e.dom == D_FAN) return fan_speed(e);
  if (e.dom == D_COVER) return cov_pos(e);
  if (e.dom == D_CLIMATE) return !std::isnan(e.tgt_t);
  return false;
}
// 空調溫度的範圍與步進(HA 沒給就用常見值)
inline void clim_range(const Ent &e, float &lo, float &hi, float &step) {
  lo = std::isnan(e.tmin) ? 16.0f : e.tmin;
  hi = std::isnan(e.tmax) ? 30.0f : e.tmax;
  step = (!std::isnan(e.tstep) && e.tstep > 0) ? e.tstep : 0.5f;
}

// 空調模式:依這個順序挑(使用者最常用的在前),卡片放不下的收進齒輪 → MODE
static const char *const MODE_ORDER[] = {"off", "cool", "dry", "heat", "fan_only", "auto", "heat_cool"};
inline const char *mode_label(const std::string &m) {
  if (m == "off") return "OFF";
  if (m == "cool") return "COOL";
  if (m == "dry") return "DRY";
  if (m == "heat") return "HEAT";
  if (m == "fan_only") return "FAN";
  if (m == "auto") return "AUTO";
  if (m == "heat_cool") return "H/C";
  return nullptr;
}
inline std::vector<std::string> climate_modes(const Ent &e) {
  std::vector<std::string> all, out;
  size_t p = 0;
  while (p < e.modes.size()) {
    size_t q = e.modes.find(',', p);
    if (q == std::string::npos) q = e.modes.size();
    std::string m = e.modes.substr(p, q - p);
    while (!m.empty() && m[0] == ' ') m.erase(0, 1);
    if (!m.empty()) all.push_back(m);
    p = q + 1;
  }
  for (auto *o : MODE_ORDER)
    for (auto &m : all) if (m == o) out.push_back(m);
  for (auto &m : all) if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
  return out;
}

// ---------------- 卡片內容 ----------------
inline const char *upper_state(const std::string &s, char *b, size_t n) {
  size_t k = 0;
  for (; k + 1 < n && k < s.size(); k++) b[k] = s[k] == '_' ? ' ' : (char) toupper((unsigned char) s[k]);
  b[k] = 0;
  return b;
}

// 卡片內部排版(卡片大小變了才需要)
inline void arrange(int i) {
  Tile &t = g_tile[i];
  lv_coord_t w = lv_obj_get_width(t.box);
  lv_obj_center(t.add);
  lv_label_set_long_mode(t.name, LV_LABEL_LONG_DOT);
  lv_obj_set_width(t.name, w - S(110));
  lv_obj_align(t.name, LV_ALIGN_TOP_LEFT, S(10), S(6));
  lv_obj_align(t.gear, LV_ALIGN_TOP_RIGHT, -S(8), S(6));
  lv_obj_align(t.state, LV_ALIGN_TOP_LEFT, S(10), S(32));
  lv_obj_align(t.sub, LV_ALIGN_TOP_RIGHT, -S(10), S(38));
  // 滑桿:狀態那一行下面,左右留邊給圓鈕;窗簾按鈕在最下面,其他類型數值在左下
  const Ent &e = g_ent[i];
  lv_obj_set_size(t.sl, w - 2 * S(22), S(10));
  lv_obj_align(t.sl, LV_ALIGN_TOP_LEFT, S(22), S(86));
  // 窗簾 / 空調:數值併在狀態那一行,下方是按鈕列;燈 / 風扇:數值在左下
  lv_obj_align(t.val, LV_ALIGN_BOTTOM_LEFT, S(10), -S(6));
  (void) e;
}

// 下方按鈕列:n 顆等寬排滿
inline void place_buttons(int i, int n) {
  Tile &t = g_tile[i];
  lv_coord_t w = lv_obj_get_width(t.box), bh = S(36), gap = S(6);
  lv_coord_t bw = n ? (w - 2 * S(10) - (n - 1) * gap) / n : 0;
  for (int k = 0; k < 6; k++) {
    if (k >= n) { lv_obj_add_flag(t.btn[k], LV_OBJ_FLAG_HIDDEN); continue; }
    lv_obj_clear_flag(t.btn[k], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(t.btn[k], bw, bh);
    lv_obj_align(t.btn[k], LV_ALIGN_BOTTOM_LEFT, S(10) + k * (bw + gap), -S(8));
  }
}
// 卡片寬度放得下幾顆按鈕(每顆至少 64px:Montserrat 20 的 COOL / HEAT / AUTO 才不會貼邊)
inline int buttons_fit(int i) {
  lv_coord_t w = lv_obj_get_width(g_tile[i].box), gap = S(6);
  int n = (int) ((w - 2 * S(10) + gap) / (S(64) + gap));
  return n < 1 ? 1 : n > 6 ? 6 : n;
}

// 一張卡片依 g_ent 重畫(沒變的不 set,見 shub::set_text)
inline void render(int i) {
  if (!g_built) return;
  Tile &t = g_tile[i];
  const Ent &e = g_ent[i];
  bool empty = e.id.empty();
  lv_obj_t *parts[] = {t.name, t.tag, t.gear, t.state, t.val, t.unit, t.sub};
  for (auto *o : parts) {
    if (empty) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
  arrange(i);
  if (empty) {
    place_buttons(i, 0);
    lv_obj_add_flag(t.sl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(t.add, LV_OBJ_FLAG_HIDDEN);
    shub::set_text(t.add, g_ha_ok ? "+ ADD" : "SET HA URL / TOKEN");
    if (t.c_box != 0x1E4A32) { lv_obj_set_style_border_color(t.box, lv_color_hex(0x1E4A32), 0); t.c_box = 0x1E4A32; }
    return;
  }
  lv_obj_add_flag(t.add, LV_OBJ_FLAG_HIDDEN);
  shub::set_text(t.name, e.name.empty() ? e.id.c_str() : e.name.c_str());
  shub::set_text(t.tag, DOM_NAME[e.dom]);
  lv_obj_align_to(t.tag, t.gear, LV_ALIGN_OUT_LEFT_MID, -S(8), 0);

  char b[32], u[8] = "", sub[32] = "";
  b[0] = 0;
  bool on = false, na = !e.valid || e.state == "unavailable" || e.state == "unknown";
  char st[32];
  upper_state(e.valid ? e.state : std::string("..."), st, sizeof(st));
  switch (e.dom) {
    case D_LIGHT:
      on = e.state == "on";
      if (on && !std::isnan(e.bri)) { snprintf(b, sizeof(b), "%.0f", e.bri); strcpy(u, "%"); }
      if (on && !e.effect.empty()) snprintf(sub, sizeof(sub), "%s", e.effect.c_str());   // 目前模式優先
      else if (on && !std::isnan(e.ct)) snprintf(sub, sizeof(sub), "%.0fK", e.ct);
      break;
    case D_COVER:
      on = e.state == "open" || e.state == "opening";
      // 下方是按鈕:位置 % 接在狀態後面(同一行)
      if (!std::isnan(e.pos)) {
        size_t L = strlen(st);
        snprintf(st + L, sizeof(st) - L, "  %.0f%%", e.pos);
      }
      break;
    case D_CLIMATE: {
      on = !e.state.empty() && e.state != "off";
      bool half = !std::isnan(e.tstep) && e.tstep < 1.0f;
      // 下方是模式按鈕:設定溫度併在狀態那一行。字型沒有 ° 字形,單位照感測卡片寫 C / F
      if (!std::isnan(e.tgt_t)) {
        size_t L = strlen(st);
        snprintf(st + L, sizeof(st) - L, half ? "  %.1f%s" : "  %.0f%s", e.tgt_t, shub::imperial_now ? "F" : "C");
      }
      if (!std::isnan(e.cur_t)) snprintf(sub, sizeof(sub), "NOW %.1f", e.cur_t);
      break;
    }
    case D_FAN:
      on = e.state == "on";
      if (on && !std::isnan(e.pct)) { snprintf(b, sizeof(b), "%.0f", e.pct); strcpy(u, "%"); }
      if (e.osc == 1) strcpy(sub, "OSC");
      break;
    default: break;
  }
  if (na) b[0] = 0, u[0] = 0;
  if (g_pend[i].active) {   // 送出去了還沒確認
    size_t L = strlen(st);
    snprintf(st + L, sizeof(st) - L, " ...");
  }
  bool err = !e.err.empty();
  if (err) snprintf(sub, sizeof(sub), "%s", e.err.c_str());
  // 按鈕列:窗簾 OPEN / STOP / CLOSE(支援的才放);空調的模式(目前模式反白)
  int nb = 0;
  if (!na && e.dom == D_COVER) {
    if (cov_open(e)) { t.bcmd[nb] = "open_cover"; shub::set_text(t.btl[nb++], "OPEN"); }
    if (cov_stop(e)) { t.bcmd[nb] = "stop_cover"; shub::set_text(t.btl[nb++], "STOP"); }
    if (cov_close(e)) { t.bcmd[nb] = "close_cover"; shub::set_text(t.btl[nb++], "CLOSE"); }
  } else if (!na && e.dom == D_CLIMATE) {
    auto ms = climate_modes(e);
    int fit = buttons_fit(i);
    for (auto &m : ms) {
      if (nb >= fit) break;
      const char *lb = mode_label(m);
      char tmp[16];
      if (!lb) lb = upper_state(m, tmp, sizeof(tmp));
      t.bcmd[nb] = m;
      shub::set_text(t.btl[nb++], lb);
    }
  }
  for (int k = 0; k < 6; k++) {
    bool cur = e.dom == D_CLIMATE && k < nb && t.bcmd[k] == e.state;
    if (cur) lv_obj_add_state(t.btn[k], LV_STATE_CHECKED);
    else lv_obj_clear_state(t.btn[k], LV_STATE_CHECKED);
  }
  place_buttons(i, nb);
  if (!has_slider(e)) {
    lv_obj_add_flag(t.sl, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(t.sl, LV_OBJ_FLAG_HIDDEN);
    if (!t.drag) {   // 使用者正在拖:不要拉回去(set_range / set_value 值相同時 LVGL 不重繪)
      if (e.dom == D_CLIMATE) {
        float lo, hi, step;
        clim_range(e, lo, hi, step);
        lv_slider_set_range(t.sl, (int32_t) lroundf(lo * 10), (int32_t) lroundf(hi * 10));
        lv_slider_set_value(t.sl, (int32_t) lroundf(e.tgt_t * 10), LV_ANIM_OFF);
      } else {
        lv_slider_set_range(t.sl, 0, 100);
        float v = e.dom == D_COVER ? e.pos : e.state != "on" ? 0.0f : e.dom == D_LIGHT ? e.bri : e.pct;
        lv_slider_set_value(t.sl, std::isnan(v) ? 0 : (int32_t) lroundf(v), LV_ANIM_OFF);
      }
    }
  }
  shub::set_text(t.state, st);
  shub::set_text(t.val, b[0] ? b : (na ? "--" : ""));
  shub::set_text(t.unit, u);
  shub::set_text(t.sub, sub);
  uint32_t csub = err ? 0xFF4040 : 0x6E8273;
  if (t.c_sub != csub) { lv_obj_set_style_text_color(t.sub, lv_color_hex(csub), 0); t.c_sub = csub; }
  lv_obj_align_to(t.unit, t.val, LV_ALIGN_OUT_RIGHT_TOP, S(3), S(4));
  uint32_t cs = na ? 0x4E6A58 : on ? 0x00E66E : 0x6E8273, cv = on ? 0x00E66E : 0x6E8273,
           cb = on && !na ? 0x00A050 : 0x1E4A32;
  if (t.c_state != cs) { lv_obj_set_style_text_color(t.state, lv_color_hex(cs), 0); t.c_state = cs; }
  if (t.c_val != cv) { lv_obj_set_style_text_color(t.val, lv_color_hex(cv), 0); t.c_val = cv; }
  if (t.c_box != cb) { lv_obj_set_style_border_color(t.box, lv_color_hex(cb), 0); t.c_box = cb; }
}

// ---------------- 狀態(Job 12)----------------
inline float fnum(const std::string &s) {
  if (s.empty()) return NAN;
  char *end = nullptr;
  float v = strtof(s.c_str(), &end);
  return (end && end != s.c_str()) ? v : NAN;
}

// 每行:id|name|state|brightness|current_position|current_temperature|temperature|hvac_modes|
//       min_temp|max_temp|target_temp_step|percentage|percentage_step|oscillating|supported_features|
//       supported_color_modes|color_temp_kelvin|min_color_temp_kelvin|max_color_temp_kelvin|
//       effect_list(; 分隔)|effect
inline std::string g_raw_dbg;   // 給網頁 Control States 看的原文(查裝置回報了哪些屬性)
inline void apply(const std::string &raw) {
  g_raw_dbg = raw;
  size_t p = 0;
  while (p < raw.size()) {
    size_t e = raw.find('\n', p);
    if (e == std::string::npos) e = raw.size();
    std::string ln = raw.substr(p, e - p);
    p = e + 1;
    std::vector<std::string> f;
    size_t a = 0;
    for (;;) {
      size_t q = ln.find('|', a);
      f.push_back(ln.substr(a, q == std::string::npos ? std::string::npos : q - a));
      if (q == std::string::npos) break;
      a = q + 1;
    }
    if (f.size() < 21) continue;
    for (auto &s : f) while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
    {   // 每個實體的原始屬性,內容變了才印(查「某台裝置為什麼沒有滑桿 / 按鈕」用)
      static std::string last[shub::SLOT_MAX];
      for (int i = 0; i < shub::SLOT_MAX; i++)
        if (g_ent[i].id == f[0] && last[i] != ln) { last[i] = ln; ESP_LOGI("ctrl", "%s", ln.c_str()); }
    }
    for (int i = 0; i < shub::SLOT_MAX; i++) {
      Ent &x = g_ent[i];
      if (x.id != f[0] || g_pend[i].active) continue;   // 操作中:等確認後的回讀
      x.name = f[1];
      x.state = f[2];
      float br = fnum(f[3]);
      x.bri = std::isnan(br) ? NAN : br * 100.0f / 255.0f;
      x.pos = fnum(f[4]);
      x.cur_t = fnum(f[5]);
      x.tgt_t = fnum(f[6]);
      x.modes = f[7];
      x.tmin = fnum(f[8]);
      x.tmax = fnum(f[9]);
      x.tstep = fnum(f[10]);
      x.pct = fnum(f[11]);
      x.pstep = fnum(f[12]);
      x.osc = f[13] == "True" ? 1 : f[13] == "False" ? 0 : -1;
      // 新版 HA 的 supported_features 是 IntFlag,模板的 join 會輸出成 "1,2,4,8":全部 OR 起來
      x.feat = 0;
      for (size_t q = 0; q < f[14].size();) {
        char *end = nullptr;
        x.feat |= (uint32_t) strtoul(f[14].c_str() + q, &end, 10);
        size_t nq = end ? (size_t) (end - f[14].c_str()) : f[14].size();
        q = nq > q ? nq + 1 : f[14].size();
      }
      x.cmodes = f[15];
      x.ct = fnum(f[16]);
      x.ctmin = fnum(f[17]);
      x.ctmax = fnum(f[18]);
      x.effects = f[19];
      x.effect = f[20];
      x.valid = true;
      render(i);
    }
  }
}

// 第 slot 格換成 id(開機還原、網頁改 Control N、選單挑選都走這裡)
inline void set_ent(int slot, const std::string &id) {
  if (slot < 0 || slot >= shub::SLOT_MAX) return;
  Ent &e = g_ent[slot];
  std::string nid = id_ok(id) && dom_of(id) != D_NONE ? id : std::string();
  if (e.id == nid) return;
  e = Ent{};
  g_pend[slot] = Pend{};
  e.id = nid;
  e.dom = dom_of(nid);
  recount();
  render(slot);
}

// ---------------- 挑選選單 ----------------
inline int g_pick_slot = -1;
inline int g_pick_dom = D_NONE;
inline int g_pick_page = 0;
inline std::string g_list_raw;                 // id|name|state 每行一筆
inline std::vector<uint32_t> g_list_off;       // 每筆起點
inline std::vector<uint16_t> g_list_idx;       // 目前類型篩出的筆

inline void list_at(size_t k, std::string &id, std::string &name, std::string &st) {
  size_t a = g_list_off[k], e = g_list_raw.find('\n', a);
  if (e == std::string::npos) e = g_list_raw.size();
  std::string ln = g_list_raw.substr(a, e - a);
  size_t x = ln.find('|'), y = ln.find('|', x + 1);
  id = ln.substr(0, x);
  name = ln.substr(x + 1, y - x - 1);
  st = y == std::string::npos ? "" : ln.substr(y + 1);
  while (!st.empty() && (st.back() == '\r' || st.back() == ' ')) st.pop_back();
}

inline void dom_menu();

inline void list_page(int page) {
  const int PG = shub::HA_PAGE;
  int n = (int) g_list_idx.size(), pages = (n + PG - 1) / PG;
  if (pages < 1) pages = 1;
  if (page < 0) page = 0;
  if (page >= pages) page = pages - 1;
  g_pick_page = page;
  int a = page * PG, e = a + PG < n ? a + PG : n;
  char b[128];
  snprintf(b, sizeof(b), "CTRL %d  %s  %d-%d / %d", g_pick_slot + 1, DOM_NAME[g_pick_dom], n ? a + 1 : 0, e, n);
  lv_coord_t y;
  lv_obj_t *pn = shub::picker_frame(b, y);
  shub::picker_row(pn, y, LV_SYMBOL_LEFT "  TYPES", false, [](lv_event_t *ev) { dom_menu(); }, 0);
  auto nav = [](lv_event_t *ev) { list_page(g_pick_page + (int) (intptr_t) lv_event_get_user_data(ev)); };
  if (page > 0) shub::picker_row(pn, y, LV_SYMBOL_UP "  PREV", false, nav, -1);
  const std::string &cur = g_pick_slot >= 0 ? g_ent[g_pick_slot].id : std::string();
  for (int i = a; i < e; i++) {
    std::string id, name, st;
    list_at(g_list_idx[i], id, name, st);
    snprintf(b, sizeof(b), "%s   %s", name.c_str(), st.c_str());
    shub::picker_row(pn, y, b, id == cur, [](lv_event_t *ev) {
      size_t k = (size_t) (intptr_t) lv_event_get_user_data(ev);
      int sl = g_pick_slot;
      g_pick_slot = -1;
      shub::close_picker();
      std::string id, name, st;
      if (sl < 0 || k >= g_list_off.size()) return;
      list_at(k, id, name, st);
      if (g_on_pick) g_on_pick(sl, id);
      g_ent[sl].name = name;   // 先用清單裡的名稱,等狀態回來
      render(sl);
    }, (intptr_t) g_list_idx[i]);
  }
  if (page < pages - 1) shub::picker_row(pn, y, LV_SYMBOL_DOWN "  NEXT", false, nav, 1);
}

// 類型選單(附筆數)+ 移除
inline void dom_menu() {
  int cnt[D_N] = {0};
  for (size_t k = 0; k < g_list_off.size(); k++) {
    std::string id, name, st;
    list_at(k, id, name, st);
    cnt[dom_of(id)]++;
  }
  char b[64];
  snprintf(b, sizeof(b), "CTRL %d  DEVICE TYPE", g_pick_slot + 1);
  lv_coord_t y;
  lv_obj_t *pn = shub::picker_frame(b, y);
  auto go = [](lv_event_t *ev) {
    g_pick_dom = (int) (intptr_t) lv_event_get_user_data(ev);
    g_list_idx.clear();
    for (size_t k = 0; k < g_list_off.size(); k++) {
      std::string id, name, st;
      list_at(k, id, name, st);
      if (dom_of(id) == g_pick_dom) g_list_idx.push_back((uint16_t) k);
    }
    list_page(0);
  };
  for (int d = 1; d < D_N; d++) {
    if (!cnt[d]) continue;
    snprintf(b, sizeof(b), "%s   (%d)", DOM_NAME[d], cnt[d]);
    shub::picker_row(pn, y, b, g_pick_slot >= 0 && g_ent[g_pick_slot].dom == d, go, d);
  }
  if (g_pick_slot >= 0 && !g_ent[g_pick_slot].id.empty())
    shub::picker_row(pn, y, LV_SYMBOL_TRASH "  REMOVE", false, [](lv_event_t *ev) {
      int sl = g_pick_slot;
      g_pick_slot = -1;
      shub::close_picker();
      if (sl >= 0 && g_on_pick) g_on_pick(sl, "");
    }, 0);
}

// 簡單的訊息框(錯誤 / 載入中)
inline void msg(const char *title, const char *text, uint32_t col) {
  lv_coord_t y;
  lv_obj_t *pn = shub::picker_frame(title, y);
  lv_obj_t *l = lv_label_create(pn);
  lv_obj_set_style_text_font(l, shub::g_f_name, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
  lv_obj_set_pos(l, 0, y);
  lv_label_set_text(l, text);
}

// 點空格或齒輪:要清單 → LOADING,結果回來由 show_list() 接手
inline void open_slot(int slot) {
  if (millis() - shub::g_gesture_ms < 600) return;   // 剛滑完的那次放開不算點
  if (!g_ha_ok) {
    msg("HOME ASSISTANT", "SET HA URL AND HA TOKEN\nIN THE WEB UI FIRST", 0xFF7800);
    return;
  }
  g_pick_slot = slot;
  char b[48];
  snprintf(b, sizeof(b), "CTRL %d  DEVICE TYPE", slot + 1);
  msg(b, "LOADING ...", 0x9AE6B4);
  if (g_on_list_req) g_on_list_req();
}

// 清單回來(主迴圈呼叫):還在挑才顯示
inline void show_list(const std::string &raw, int status) {
  if (g_pick_slot < 0 || !shub::g_pick) { g_pick_slot = -1; return; }
  g_list_raw = raw;
  g_list_off.clear();
  for (size_t p = 0; p < g_list_raw.size();) {
    size_t e = g_list_raw.find('\n', p);
    if (e == std::string::npos) e = g_list_raw.size();
    if (e > p && std::count(g_list_raw.begin() + p, g_list_raw.begin() + e, '|') >= 2) g_list_off.push_back(p);
    p = e + 1;
  }
  if (status != 200 || g_list_off.empty()) {
    char b[64];
    if (status == 401) snprintf(b, sizeof(b), "HA TOKEN REJECTED (401)");
    else if (status != 200) snprintf(b, sizeof(b), "HA NOT REACHABLE (%d)", status);
    else snprintf(b, sizeof(b), "NO LIGHTS / COVERS / CLIMATE / FANS");
    msg("HOME ASSISTANT", b, 0xFF7800);
    g_pick_slot = -1;
    return;
  }
  dom_menu();
}

// ---------------- 觸控操作 ----------------
inline bool just_swiped() { return millis() - shub::g_gesture_ms < 600; }   // 滑完放開的那一下不算點

// 點卡片:空格開選單;燈 / 風扇開關(窗簾、空調用下方按鈕)
inline void tap(int i) {
  if (just_swiped()) return;
  Ent &e = g_ent[i];
  if (e.id.empty()) { open_slot(i); return; }
  if (!e.valid || e.state == "unavailable") return;
  if (e.dom == D_LIGHT || e.dom == D_FAN) {
    act(i, e.dom == D_LIGHT ? "light/toggle" : "fan/toggle", body_id(e));
    e.state = e.state == "on" ? "off" : "on";
    render(i);
  }
}

// 卡片下方按鈕 k
inline void press(int i, int k) {
  if (just_swiped()) return;
  Ent &e = g_ent[i];
  if (!e.valid) return;
  char x[48];
  const std::string &cmd = g_tile[i].bcmd[k];
  if (e.dom == D_COVER) {
    std::string path = "cover/" + cmd;
    act(i, path.c_str(), body_id(e));
    if (cmd == "open_cover") e.state = "opening";
    else if (cmd == "close_cover") e.state = "closing";
  } else if (e.dom == D_CLIMATE) {
    if (cmd == e.state || cmd.size() > 24) return;
    for (char c : cmd) if (!(isalnum((unsigned char) c) || c == '_')) return;   // 放進 JSON 字串
    snprintf(x, sizeof(x), "\"hvac_mode\":\"%s\"", cmd.c_str());
    act(i, "climate/set_hvac_mode", body_id(e, x));
    e.state = cmd;
  } else {
    return;
  }
  render(i);
}

// 卡片上的滑桿:拖動中只更新數字(不送),放開才送一次
inline void slide_preview(int i, int32_t v) {
  Tile &t = g_tile[i];
  const Ent &e = g_ent[i];
  char b[32];
  if (e.dom == D_CLIMATE) {
    float lo, hi, step;
    clim_range(e, lo, hi, step);
    snprintf(b, sizeof(b), step < 1.0f ? "%.1f" : "%.0f", v / 10.0f);
  } else if (e.dom == D_FAN || e.dom == D_LIGHT) {
    snprintf(b, sizeof(b), "%d", (int) v);
    shub::set_text(t.val, b);
    shub::set_text(t.unit, "%");
    lv_obj_align_to(t.unit, t.val, LV_ALIGN_OUT_RIGHT_TOP, S(3), S(4));
  } else if (e.dom == D_COVER) {
    snprintf(b, sizeof(b), "%d%%", (int) v);
    shub::set_text(t.state, b);
  }
  if (e.dom == D_CLIMATE) {
    char st[24];
    snprintf(st, sizeof(st), "%s  %s%s", mode_label(e.state) ? mode_label(e.state) : "", b,
             shub::imperial_now ? "F" : "C");
    shub::set_text(t.state, st);
  }
}

inline void slide_send(int i, int32_t v) {
  Ent &e = g_ent[i];
  char x[48];
  if (e.dom == D_CLIMATE) {
    float lo, hi, step;
    clim_range(e, lo, hi, step);
    float tv = roundf(v / 10.0f / step) * step;   // 對齊 HA 的步進(例如 0.5)
    tv = tv < lo ? lo : tv > hi ? hi : tv;
    snprintf(x, sizeof(x), "\"temperature\":%.1f", tv);
    act(i, "climate/set_temperature", body_id(e, x));
    e.tgt_t = tv;
  } else if (e.dom == D_FAN) {
    float step = (!std::isnan(e.pstep) && e.pstep > 0) ? e.pstep : 1.0f;
    float pv = roundf(v / step) * step;           // 對齊風扇的段數(例如 3 段 = 33.33%)
    pv = pv < 0 ? 0 : pv > 100 ? 100 : pv;
    snprintf(x, sizeof(x), "\"percentage\":%d", (int) lroundf(pv));
    act(i, "fan/set_percentage", body_id(e, x));   // 0 = 關
    e.pct = pv;
    e.state = pv > 0 ? "on" : "off";
  } else if (e.dom == D_LIGHT) {
    if (v <= 0) {   // 拖到 0 = 關燈
      act(i, "light/turn_off", body_id(e));
      e.state = "off";
    } else {
      snprintf(x, sizeof(x), "\"brightness_pct\":%d", (int) v);
      act(i, "light/turn_on", body_id(e, x));
      e.state = "on";
      e.bri = (float) v;
    }
  } else if (e.dom == D_COVER) {
    snprintf(x, sizeof(x), "\"position\":%d", (int) v);
    act(i, "cover/set_cover_position", body_id(e, x));
    e.state = v > (std::isnan(e.pos) ? 0 : e.pos) ? "opening" : "closing";
    e.pos = (float) v;
  } else {
    return;
  }
  render(i);
}

// 齒輪 → 進階:燈的色溫 / 模式、空調模式、擺頭、換裝置
// JSON 字串跳脫(模式名稱可能有空白、引號、中文)
inline std::string json_str(const std::string &v) {
  std::string o = "\"";
  for (char c : v) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char) c < 0x20) continue;
    else o += c;
  }
  return o + "\"";
}

// 燈的色溫:齒輪 → COLOR TEMP,彈出滑桿,放開才送
inline int g_ct_slot = -1;
inline lv_obj_t *g_ct_lab = nullptr;
inline void ct_panel(int i) {
  Ent &e = g_ent[i];
  g_ct_slot = i;
  char b[96];
  snprintf(b, sizeof(b), "%s  COLOR TEMP", e.name.c_str());
  lv_coord_t y;
  lv_obj_t *pn = shub::picker_frame(b, y);
  int lo = (int) e.ctmin, hi = std::isnan(e.ctmax) ? 6500 : (int) e.ctmax;
  int cur = std::isnan(e.ct) ? (lo + hi) / 2 : (int) e.ct;
  g_ct_lab = lv_label_create(pn);
  lv_obj_set_style_text_font(g_ct_lab, g_f_mid, 0);
  lv_obj_set_style_text_color(g_ct_lab, lv_color_hex(0x00E66E), 0);
  lv_obj_set_pos(g_ct_lab, 0, y);
  snprintf(b, sizeof(b), "%dK", cur);
  lv_label_set_text(g_ct_lab, b);
  lv_obj_t *wl = shub::mk_label(pn, shub::g_f_name, 0x6E8273);   // 兩端標示暖 / 冷
  lv_label_set_text(wl, "WARM");
  lv_obj_set_pos(wl, 0, y + S(84));
  lv_obj_t *cl = shub::mk_label(pn, shub::g_f_name, 0x6E8273);
  lv_label_set_text(cl, "COOL");
  lv_obj_align(cl, LV_ALIGN_TOP_RIGHT, 0, y + S(84));
  lv_obj_t *sl = lv_slider_create(pn);
  lv_obj_set_width(sl, LV_PCT(92));
  lv_obj_set_height(sl, S(14));
  lv_obj_set_pos(sl, S(14), y + S(60));
  lv_slider_set_range(sl, lo, hi);
  lv_slider_set_value(sl, cur, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(sl, lv_color_hex(0x0A2416), LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl, lv_color_hex(0x00C060), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, lv_color_hex(0x00E66E), LV_PART_KNOB);
  lv_obj_set_style_pad_all(sl, S(8), LV_PART_KNOB);
  lv_obj_set_ext_click_area(sl, S(16));
  lv_obj_add_event_cb(sl, [](lv_event_t *ev) {
    char t[12];
    snprintf(t, sizeof(t), "%dK", (int) lv_slider_get_value((lv_obj_t *) lv_event_get_target(ev)));
    if (g_ct_lab) lv_label_set_text(g_ct_lab, t);
  }, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(sl, [](lv_event_t *ev) {
    int i = g_ct_slot;
    if (i < 0) return;
    Ent &e = g_ent[i];
    int v = (int) lv_slider_get_value((lv_obj_t *) lv_event_get_target(ev));
    char x[40];
    snprintf(x, sizeof(x), "\"color_temp_kelvin\":%d", v);
    act(i, "light/turn_on", body_id(e, x));
    e.ct = (float) v;
    e.effect.clear();
    e.state = "on";
    render(i);
  }, LV_EVENT_RELEASED, nullptr);
  y += S(124);
  shub::picker_row(pn, y, "DONE", false, [](lv_event_t *ev) { g_ct_lab = nullptr; shub::close_picker(); }, 0);
}

// 燈的模式(effect_list):齒輪 → MODE,清單點選
inline int g_fx_slot = -1;
inline std::vector<std::string> light_effects(const Ent &e) {
  std::vector<std::string> v;
  size_t p = 0;
  while (p < e.effects.size()) {
    size_t q = e.effects.find(';', p);
    if (q == std::string::npos) q = e.effects.size();
    if (q > p) v.push_back(e.effects.substr(p, q - p));
    p = q + 1;
  }
  return v;
}
inline void effect_menu(int i) {
  Ent &e = g_ent[i];
  g_fx_slot = i;
  char b[96];
  snprintf(b, sizeof(b), "%s  MODE", e.name.c_str());
  lv_coord_t y;
  lv_obj_t *pn = shub::picker_frame(b, y);
  auto fx = light_effects(e);
  for (size_t k = 0; k < fx.size() && k < 40; k++)
    shub::picker_row(pn, y, fx[k].c_str(), fx[k] == e.effect, [](lv_event_t *ev) {
      int k = (int) (intptr_t) lv_event_get_user_data(ev);
      int i = g_fx_slot;
      shub::close_picker();
      if (i < 0) return;
      Ent &e = g_ent[i];
      auto fx = light_effects(e);
      if (k >= (int) fx.size()) return;
      std::string x = "\"effect\":" + json_str(fx[k]);
      act(i, "light/turn_on", body_id(e, x.c_str()));
      e.effect = fx[k];
      e.state = "on";
      render(i);
    }, (intptr_t) k);
}

// 空調全部模式(卡片放不下的在這裡選)
inline int g_mode_slot = -1;
inline void mode_menu(int i) {
  Ent &e = g_ent[i];
  g_mode_slot = i;
  char b[96];
  snprintf(b, sizeof(b), "%s  MODE", e.name.c_str());
  lv_coord_t y;
  lv_obj_t *pn = shub::picker_frame(b, y);
  auto ms = climate_modes(e);
  for (size_t k = 0; k < ms.size() && k < 12; k++) {
    char tmp[24];
    const char *lb = mode_label(ms[k]);
    if (!lb) lb = upper_state(ms[k], tmp, sizeof(tmp));
    shub::picker_row(pn, y, lb, ms[k] == e.state, [](lv_event_t *ev) {
      int k = (int) (intptr_t) lv_event_get_user_data(ev);
      int i = g_mode_slot;
      shub::close_picker();
      if (i < 0) return;
      auto ms = climate_modes(g_ent[i]);
      if (k >= (int) ms.size()) return;
      Tile &t = g_tile[i];
      t.bcmd[5] = ms[k];   // 借最後一顆按鈕的指令欄位走同一條路
      press(i, 5);
    }, (intptr_t) k);
  }
}

inline void gear(int i) {
  if (just_swiped()) return;
  Ent &e = g_ent[i];
  if (e.id.empty() || !e.valid) { open_slot(i); return; }
  lv_coord_t y;
  lv_obj_t *pn = shub::picker_frame(e.name.c_str(), y);
  if (e.dom == D_LIGHT && light_ct(e))
    shub::picker_row(pn, y, "COLOR TEMP ...", false, [](lv_event_t *ev) {
      ct_panel((int) (intptr_t) lv_event_get_user_data(ev));
    }, i);
  if (e.dom == D_LIGHT && !e.effects.empty())
    shub::picker_row(pn, y, "MODE ...", false, [](lv_event_t *ev) {
      effect_menu((int) (intptr_t) lv_event_get_user_data(ev));
    }, i);
  if (e.dom == D_CLIMATE && !climate_modes(e).empty())
    shub::picker_row(pn, y, "MODE ...", false, [](lv_event_t *ev) {
      mode_menu((int) (intptr_t) lv_event_get_user_data(ev));
    }, i);
  if (e.dom == D_FAN && fan_osc(e))
    shub::picker_row(pn, y, e.osc == 1 ? "OSCILLATE: ON" : "OSCILLATE: OFF", e.osc == 1, [](lv_event_t *ev) {
      int i = (int) (intptr_t) lv_event_get_user_data(ev);
      shub::close_picker();
      Ent &e = g_ent[i];
      bool on = e.osc != 1;
      act(i, "fan/oscillate", body_id(e, on ? "\"oscillating\":true" : "\"oscillating\":false"));
      e.osc = on ? 1 : 0;
      render(i);
    }, i);
  shub::picker_row(pn, y, "CHANGE DEVICE ...", false, [](lv_event_t *ev) {
    open_slot((int) (intptr_t) lv_event_get_user_data(ev));
  }, i);
}

// ---------------- 排版 / 建立 ----------------
// n 張卡片放到 r[0..n-1];其餘藏起來。n = 0 → 全藏(切回 SENSOR 時)
inline void place(int n, const lv_area_t *r) {
  g_n = n;
  for (int i = 0; i < shub::SLOT_MAX; i++) {
    Tile &t = g_tile[i];
    if (i >= n) { lv_obj_add_flag(t.box, LV_OBJ_FLAG_HIDDEN); continue; }
    lv_obj_clear_flag(t.box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(t.box, r[i].x1, r[i].y1);
    lv_obj_set_size(t.box, r[i].x2 - r[i].x1 + 1, r[i].y2 - r[i].y1 + 1);
    lv_obj_update_layout(t.box);
    arrange(i);
    render(i);
  }
  if (n > 0 && g_on_poll) g_on_poll();   // 一顯示就抓一次,不等 5 秒
}

// shub::build()、shub::clock_init() 之後呼叫
inline void build(const lv_font_t *f_val, const lv_font_t *f_mid, const lv_font_t *f_tiny,
                  const lv_font_t *f_sym) {
  if (g_built || !shub::g_page) return;
  g_built = true;
  g_f_val = f_val; g_f_mid = f_mid; g_f_tiny = f_tiny;
  for (int i = 0; i < shub::SLOT_MAX; i++) {
    Tile &t = g_tile[i];
    t.box = lv_obj_create(shub::g_page);
    lv_obj_clear_flag(t.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(t.box, lv_color_hex(0x07140D), 0);
    lv_obj_set_style_bg_opa(t.box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(t.box, lv_color_hex(0x1E4A32), 0);
    lv_obj_set_style_border_width(t.box, 1, 0);
    lv_obj_set_style_radius(t.box, S(6), 0);
    lv_obj_set_style_pad_all(t.box, 0, 0);
    // 點卡片:空格開選單,燈 / 風扇開關
    lv_obj_add_event_cb(t.box, [](lv_event_t *e) {
      tap((int) (intptr_t) lv_event_get_user_data(e));
    }, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    t.add = shub::mk_label(t.box, shub::g_f_name, 0x3E5A48);
    t.name = shub::mk_label(t.box, shub::g_f_name, 0x9FB8A6);
    t.tag = shub::mk_label(t.box, f_tiny, 0x6E8273);
    lv_obj_set_style_border_color(t.tag, lv_color_hex(0x2E6A48), 0);
    lv_obj_set_style_border_width(t.tag, 1, 0);
    lv_obj_set_style_radius(t.tag, 3, 0);
    lv_obj_set_style_pad_hor(t.tag, S(5), 0);
    t.gear = shub::mk_label(t.box, f_sym, 0x6E8273);
    lv_label_set_text(t.gear, LV_SYMBOL_SETTINGS);
    lv_obj_add_flag(t.gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(t.gear, S(14));
    lv_obj_add_event_cb(t.gear, [](lv_event_t *e) {
      gear((int) (intptr_t) lv_event_get_user_data(e));
    }, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    t.state = shub::mk_label(t.box, f_mid, 0x6E8273);
    t.val = shub::mk_label(t.box, f_val, 0x6E8273);
    t.unit = shub::mk_label(t.box, shub::g_f_name, 0x6E8273);
    t.sub = shub::mk_label(t.box, shub::g_f_name, 0x6E8273);
    t.sl = lv_slider_create(t.box);
    lv_obj_set_style_bg_color(t.sl, lv_color_hex(0x0A2416), LV_PART_MAIN);
    lv_obj_set_style_bg_color(t.sl, lv_color_hex(0x00A050), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(t.sl, lv_color_hex(0x00E66E), LV_PART_KNOB);
    lv_obj_set_style_pad_all(t.sl, S(9), LV_PART_KNOB);
    lv_obj_set_ext_click_area(t.sl, S(14));
    // 拖滑桿也是左右滑:不能讓手勢冒泡到頁面(否則會切 SENSOR / CTRL)
    lv_obj_clear_flag(t.sl, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(t.sl, [](lv_event_t *e) {
      int i = (int) (intptr_t) lv_event_get_user_data(e);
      lv_event_code_t c = lv_event_get_code(e);
      int32_t v = lv_slider_get_value(g_tile[i].sl);
      if (c == LV_EVENT_PRESSED) {
        g_tile[i].drag = true;
      } else if (c == LV_EVENT_VALUE_CHANGED) {
        slide_preview(i, v);
      } else if ((c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) && g_tile[i].drag) {
        g_tile[i].drag = false;   // 兩個事件可能都來:只送一次
        slide_send(i, v);
      }
    }, LV_EVENT_ALL, (void *) (intptr_t) i);
    lv_obj_add_flag(t.sl, LV_OBJ_FLAG_HIDDEN);
    for (int k = 0; k < 6; k++) {
      lv_obj_t *b = lv_btn_create(t.box);
      lv_obj_set_style_bg_color(b, lv_color_hex(0x0A2416), 0);
      lv_obj_set_style_bg_color(b, lv_color_hex(0x00C060), LV_STATE_PRESSED);
      lv_obj_set_style_bg_color(b, lv_color_hex(0x00C060), LV_STATE_CHECKED);   // 空調目前的模式
      lv_obj_set_style_text_color(b, lv_color_hex(0x9AE6B4), 0);
      lv_obj_set_style_text_color(b, lv_color_hex(0x04180C), LV_STATE_CHECKED);
      lv_obj_set_style_border_color(b, lv_color_hex(0x00A050), 0);
      lv_obj_set_style_border_width(b, 1, 0);
      lv_obj_set_style_radius(b, S(4), 0);
      lv_obj_set_style_shadow_width(b, 0, 0);
      lv_obj_set_style_pad_all(b, 0, 0);
      lv_obj_set_ext_click_area(b, S(4));
      t.btl[k] = lv_label_create(b);
      lv_obj_set_style_text_font(t.btl[k], f_sym, 0);   // Montserrat 20:英文字與符號都有;顏色繼承按鈕
      lv_obj_center(t.btl[k]);
      lv_obj_add_event_cb(b, [](lv_event_t *e) {
        int u = (int) (intptr_t) lv_event_get_user_data(e);
        press(u / 8, u % 8);
      }, LV_EVENT_CLICKED, (void *) (intptr_t) (i * 8 + k));
      lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
      t.btn[k] = b;
    }
    lv_obj_add_flag(t.box, LV_OBJ_FLAG_HIDDEN);
  }
  shub::g_ctrl_place = place;
  recount();
}

}  // namespace ctrl
