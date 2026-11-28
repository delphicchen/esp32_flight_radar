#pragma once
// 時鐘頁 CTRL 模式:Home Assistant 裝置控制卡片(燈光 / 窗簾 / 空調 / 風扇),規劃見 PLAN_CTRL.md。
// 格子位置由 shub::layout_ctrl() 算好後交給 ctrl::place();卡片全部在 C++ 建立,
// 字型、縮放 S()、選單外框沿用 sensors_hub.h。兩個分支共用(LVGL 8 / 9)。
//
// 第 i 格固定對應網頁的 *Control 1..6*(text,存 entity_id)。網路一律走 radar_bg 背景 task:
// 清單 = Job 11,狀態 = Job 12(頁面顯示中每 5 秒);結果由主迴圈交給 show_list() / apply()。
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
};
inline Ent g_ent[shub::SLOT_MAX];

struct Tile {
  lv_obj_t *box, *add, *name, *tag, *gear, *state, *val, *unit, *sub;
  uint32_t c_state = 0xFFFFFFFF, c_val = 0xFFFFFFFF, c_box = 0xFFFFFFFF;   // 顏色沒變就不 set(省重繪)
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
  lv_obj_align(t.val, LV_ALIGN_BOTTOM_LEFT, S(10), -S(6));
  lv_obj_align(t.sub, LV_ALIGN_BOTTOM_RIGHT, -S(8), -S(8));
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
  if (empty) {
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
  char st[24];
  upper_state(e.valid ? e.state : std::string("..."), st, sizeof(st));
  switch (e.dom) {
    case D_LIGHT:
      on = e.state == "on";
      if (on && !std::isnan(e.bri)) { snprintf(b, sizeof(b), "%.0f", e.bri); strcpy(u, "%"); }
      break;
    case D_COVER:
      on = e.state == "open" || e.state == "opening";
      if (!std::isnan(e.pos)) { snprintf(b, sizeof(b), "%.0f", e.pos); strcpy(u, "%"); }
      break;
    case D_CLIMATE: {
      on = !e.state.empty() && e.state != "off";
      bool half = !std::isnan(e.tstep) && e.tstep < 1.0f;
      // 字型沒有 ° 字形:單位照感測卡片寫 C / F(跟著本機的英制設定,HA 端單位通常一致)
      if (!std::isnan(e.tgt_t)) { snprintf(b, sizeof(b), half ? "%.1f" : "%.0f", e.tgt_t); strcpy(u, shub::imperial_now ? "F" : "C"); }
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
  shub::set_text(t.state, st);
  shub::set_text(t.val, b[0] ? b : (na ? "--" : ""));
  shub::set_text(t.unit, u);
  shub::set_text(t.sub, sub);
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
//       supported_color_modes
inline void apply(const std::string &raw) {
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
    if (f.size() < 16) continue;
    for (auto &s : f) while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
    for (int i = 0; i < shub::SLOT_MAX; i++) {
      Ent &x = g_ent[i];
      if (x.id != f[0]) continue;
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
      x.feat = (uint32_t) strtoul(f[14].c_str(), nullptr, 10);
      x.cmodes = f[15];
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
    // 空格:點整張卡片開選單;有設定的卡片第 3 階段才會接上操作,現在只有齒輪
    lv_obj_add_event_cb(t.box, [](lv_event_t *e) {
      int i = (int) (intptr_t) lv_event_get_user_data(e);
      if (g_ent[i].id.empty()) open_slot(i);
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
      open_slot((int) (intptr_t) lv_event_get_user_data(e));
    }, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    t.state = shub::mk_label(t.box, f_mid, 0x6E8273);
    t.val = shub::mk_label(t.box, f_val, 0x6E8273);
    t.unit = shub::mk_label(t.box, shub::g_f_name, 0x6E8273);
    t.sub = shub::mk_label(t.box, f_tiny, 0x6E8273);
    lv_obj_add_flag(t.box, LV_OBJ_FLAG_HIDDEN);
  }
  shub::g_ctrl_place = place;
  recount();
}

}  // namespace ctrl
