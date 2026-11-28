#pragma once
// 掃描線的 CRT 效果(仿 P7 螢光粉:電子束打到時先閃一下,再是長餘暉,先快後慢地暗下去)
//  - 掃描線:亮白綠的細芯 + 寬而淡的綠色光暈
//  - 扇形:掃描線後面一小片漸層(2 片 × 3°;實機上 6° 之後看不出差別)
//  - 海岸線與距離環:被掃到時亮起,在扇形範圍內暗回底色
//  - 飛機:被掃到時閃白,之後 ~ 1/(1+t/τ) 漸回原色,約一圈的 92%(τ = 一圈的 5%)
//
// 以上(飛機除外)全部畫在同一個蓋住雷達畫布的物件裡(radarfx::g_obj),每幀只把
// 「上一幀到這一幀掃過的楔形」沿半徑切成一段段小矩形標成要重畫。原本掃描線 / 光暈 / 扇形各是一個
// 物件、每幀失效整個外接矩形:指向 45° 時外接矩形接近 (R/√2)² —— 1024x600 的 5B 上約 4 萬像素,
// 12 秒一圈每 3 秒碰到一次斜角,肉眼看得到週期性卡頓。切段後斜角時約 1.7 萬像素,而且各角度差不多。
// 兩個分支共用(LVGL 8 / 9 的畫圖 API 不同,用 #if 分開)。
#include <cmath>

// ---------------- lv_line 只失效自己的外接矩形 ----------------
// lv_line 的物件大小是「物件原點到最遠的點」(lv_line.c GET_SELF_SIZE)。放在頁面原點、用頁面座標
// 給點時,每次改點都會失效「頁面左上角到線尾」整塊。速度向量 / 選中航跡每秒更新:5B 上每秒一幀
// 重畫約 29 萬像素、180ms(掃描線以前也是同一個問題)。這裡把物件移到點的外接矩形左上角,
// 點就地改成相對座標(呼叫端的陣列本來就是 static,LVGL 只存指標)。
namespace lineutil {
template <typename P>
inline void set_points(lv_obj_t *ln, P *pts, int n) {
  if (n <= 0) { lv_line_set_points(ln, pts, 0); return; }
  lv_coord_t m = lv_obj_get_style_line_width(ln, LV_PART_MAIN) / 2 + 1;
  lv_coord_t x1 = (lv_coord_t) pts[0].x, y1 = (lv_coord_t) pts[0].y;
  for (int i = 1; i < n; i++) {
    x1 = LV_MIN(x1, (lv_coord_t) pts[i].x);
    y1 = LV_MIN(y1, (lv_coord_t) pts[i].y);
  }
  x1 -= m;
  y1 -= m;
  for (int i = 0; i < n; i++) { pts[i].x -= x1; pts[i].y -= y1; }
  lv_obj_set_pos(ln, x1, y1);
  lv_line_set_points(ln, pts, n);
}
}  // namespace lineutil

// ---------------- 值有變才設定 ----------------
// LVGL 8 的 set_pos / set_style_* / clear_flag(HIDDEN) 不管值有沒有變都會失效整個物件。
// 每秒的航班更新對每架飛機無條件呼叫一串:5B 上每秒多畫 10 萬多像素(每個標記容器 164×92)。
namespace lvq {
inline bool ceq(lv_color_t a, lv_color_t b) {
#if LVGL_VERSION_MAJOR >= 9
  return lv_color_eq(a, b);
#else
  return a.full == b.full;
#endif
}
inline void pos(lv_obj_t *o, lv_coord_t x, lv_coord_t y) {
  if (lv_obj_get_style_x(o, LV_PART_MAIN) != x || lv_obj_get_style_y(o, LV_PART_MAIN) != y)
    lv_obj_set_pos(o, x, y);
}
inline void show(lv_obj_t *o, bool on) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == !on) return;
  if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}
inline void text_color(lv_obj_t *o, lv_color_t c) {
  if (!ceq(lv_obj_get_style_text_color(o, LV_PART_MAIN), c)) lv_obj_set_style_text_color(o, c, 0);
}
inline void bg_color(lv_obj_t *o, lv_color_t c) {
  if (!ceq(lv_obj_get_style_bg_color(o, LV_PART_MAIN), c)) lv_obj_set_style_bg_color(o, c, 0);
}
inline void recolor(lv_obj_t *o, lv_color_t c) {
#if LVGL_VERSION_MAJOR >= 9
  if (!ceq(lv_obj_get_style_image_recolor(o, LV_PART_MAIN), c)) lv_obj_set_style_image_recolor(o, c, 0);
#else
  if (!ceq(lv_obj_get_style_img_recolor(o, LV_PART_MAIN), c)) lv_obj_set_style_img_recolor(o, c, 0);
#endif
}
inline void recolor_opa(lv_obj_t *o, lv_opa_t v) {
#if LVGL_VERSION_MAJOR >= 9
  if (lv_obj_get_style_image_recolor_opa(o, LV_PART_MAIN) != v) lv_obj_set_style_image_recolor_opa(o, v, 0);
#else
  if (lv_obj_get_style_img_recolor_opa(o, LV_PART_MAIN) != v) lv_obj_set_style_img_recolor_opa(o, v, 0);
#endif
}
inline void text(lv_obj_t *l, const char *t) {
  const char *o = lv_label_get_text(l);
  if (!o || strcmp(o, t) != 0) lv_label_set_text(l, t);
}
}  // namespace lvq

// ---------------- 雷達頁大時鐘拆成「時:分」+「:秒」----------------
// 整串 HH:MM:SS 一個標籤時,每秒換秒數就重畫整個標籤(5B 上 340×142 = 4.8 萬像素)。
// 拆開後每秒只重畫「:秒」那一小塊。數字是等寬字,開機量一次位置就固定。
namespace clocksplit {
inline lv_obj_t *g_hm = nullptr, *g_sec = nullptr;
inline void create(lv_obj_t *hm) {
  if (g_sec) return;
  g_hm = hm;
  // 先用完整寬度的字串排一次,記下置中後的位置,再改成固定座標
  lv_label_set_text(hm, "00:00:00");
  lv_obj_update_layout(hm);
  lv_coord_t x = lv_obj_get_x(hm), y = lv_obj_get_y(hm);
  const lv_font_t *f = lv_obj_get_style_text_font(hm, LV_PART_MAIN);
  lv_point_t sz;
  lv_txt_get_size(&sz, "00:00", f, lv_obj_get_style_text_letter_space(hm, LV_PART_MAIN), 0,
                  LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  lv_obj_set_align(hm, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(hm, x, y);
  lv_label_set_text(hm, "--:--");
  g_sec = lv_label_create(lv_obj_get_parent(hm));
  lv_obj_set_style_text_font(g_sec, f, 0);
  lv_obj_set_style_text_color(g_sec, lv_obj_get_style_text_color(hm, LV_PART_MAIN), 0);
  lv_obj_set_pos(g_sec, x + sz.x, y);
  lv_label_set_text(g_sec, ":--");
  // 點秒數也跟點時鐘一樣(進時鐘頁):把點擊轉給原本的標籤
  lv_obj_add_flag(g_sec, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_sec, [](lv_event_t *e) {
#if LVGL_VERSION_MAJOR >= 9
    lv_obj_send_event(g_hm, LV_EVENT_CLICKED, nullptr);
#else
    lv_event_send(g_hm, LV_EVENT_CLICKED, nullptr);
#endif
  }, LV_EVENT_CLICKED, nullptr);
}
inline void set(const char *hm, const char *sec) {
  if (!g_sec) return;
  lvq::text(g_hm, hm);
  lvq::text(g_sec, sec);
}
inline void show(bool on) {
  if (g_hm) lvq::show(g_hm, on);
  if (g_sec) lvq::show(g_sec, on);
}
}  // namespace clocksplit

// ---------------- 飛機餘暉與轉速 ----------------
namespace sweepfx {

static const int FAN_N = 2;             // 扇片數
static const float FAN_STEP = 3.0f;     // 每片角度(度);總長 6°
inline float g_ang = 0;                 // 掃描線角度(度,北 = 0,順時針)

inline uint32_t g_hit[AC_SLOTS] = {0};       // 最近一次被掃到的時間(0 = 沒被掃過)
inline uint8_t g_lvl[AC_SLOTS] = {0};        // 目前套用的亮度階(0~20),階有變才 recolor
inline uint32_t g_period = 12000;            // 掃描一圈的毫秒數(設定頁 / 網頁 Sweep Period)

// 0~20:1/(1+t/τ),τ = 一圈的 5%,一圈的 92% 後歸零(掃描線回來前剛好暗完)
inline uint8_t level(int i, uint32_t now) {
  if (!g_hit[i]) return 0;
  uint32_t t = now - g_hit[i];
  if (t >= g_period * 92 / 100) return 0;
  float g = 1.0f / (1.0f + t / (g_period * 0.05f));
  return (uint8_t) lroundf(g * 20.0f);
}

// 亮度階 → 顏色:原色(綠 / 選中的琥珀)往冷白(P7 的閃光)混
inline lv_color_t color(bool sel, uint8_t lvl) {
  float g = lvl / 20.0f;
  uint8_t br = sel ? 0xFF : 0x40, bg = sel ? 0xD2 : 0xFF, bb = sel ? 0x4A : 0x9A;
  return lv_color_make((uint8_t) (br + (0xE8 - br) * g), (uint8_t) (bg + (0xFF - bg) * g),
                       (uint8_t) (bb + (0xFF - bb) * g));
}

// 相容舊呼叫:掃描線與扇形現在都由 radarfx 畫
inline void create(lv_obj_t *, lv_coord_t, lv_coord_t, lv_coord_t) {}
inline void update(float ang) { g_ang = ang; }
inline void show(bool) {}

}  // namespace sweepfx

// ---------------- 雷達畫布上的掃描效果(單一物件)----------------
// 國界 / 省界 / 縣市界不亮(台灣圖磚的縣市界在圖資版次 1 改標成種類 3,見 map_tiles.h 的 MAPS_REV)。
namespace coastfx {

static const int NB = 4;
static const int16_t BOUND[NB] = {0, 20, 40, 60};   // 0.1° 單位:扇形 6° 內分 3 階,之後是底色
static const float BRIGHT[NB - 1] = {1.0f, 0.60f, 0.30f};
inline lv_obj_t *g_obj = nullptr;
inline bool g_static = false;      // 掃描線關掉:不畫掃描線 / 扇形,海岸線全部畫原色
inline int16_t g_sweep = 0;        // 目前掃描角(0.1°)

// 掃過後 d(0.1°)→ 階(0 最亮;NB-1 = 底色,不畫)
inline int level_of(int d) {
  for (int k = NB - 1; k >= 1; k--) if (d >= BOUND[k]) return k;
  return 0;
}
inline lv_color_t color_of(int lv) {
  // 底色 0x7A7248 → 亮的白黃 0xFFF8C8;掃描線關掉時用地圖原本的海岸線色
  if (g_static) return lv_color_hex(0xD8C878);
  float b = BRIGHT[lv];
  return lv_color_make((uint8_t) (0x7A + (0xFF - 0x7A) * b), (uint8_t) (0x72 + (0xF8 - 0x72) * b),
                       (uint8_t) (0x48 + (0xC8 - 0x48) * b));
}

// 扇形第 k 片(0 = 緊貼掃描線)的顏色與透明度
inline void fan_style(int k, lv_color_t &c, lv_opa_t &o) {
  float d = (k + 0.5f) * sweepfx::FAN_STEP;
  float a = 0.32f / (1.0f + d / 5.0f);            // 很淡:CRT 空白處只有雜訊微光
  o = (lv_opa_t) lroundf(a * 255.0f);
  float t = sweepfx::FAN_N > 1 ? (float) k / (sweepfx::FAN_N - 1) : 0.0f;   // 亮綠 → 深綠
  c = lv_color_make((uint8_t) (0x40 * (1 - t)), (uint8_t) (0xFF - 0x7F * t), (uint8_t) (0xA0 - 0x60 * t));
}

// 畫布座標的方位角 deg、半徑 r 處的點
inline void polar(float deg, float r, int &x, int &y) {
  float a = deg * 3.14159265f / 180.0f;
  x = RADAR_CX + (int) lroundf(r * sinf(a));
  y = RADAR_CX - (int) lroundf(r * cosf(a));
}

#if LVGL_VERSION_MAJOR >= 9
#define FX_CTX lv_layer_t *ctx
#else
#define FX_CTX lv_draw_ctx_t *ctx
#endif

inline void fx_line(FX_CTX, int x1, int y1, int x2, int y2, lv_color_t c, int w, lv_opa_t opa) {
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.color = c;
  d.width = w;
  d.opa = opa;
#if LVGL_VERSION_MAJOR >= 9
  d.p1.x = x1; d.p1.y = y1;
  d.p2.x = x2; d.p2.y = y2;
  lv_draw_line(ctx, &d);
#else
  lv_point_t p1{(lv_coord_t) x1, (lv_coord_t) y1}, p2{(lv_coord_t) x2, (lv_coord_t) y2};
  lv_draw_line(ctx, &d, &p1, &p2);
#endif
}

#ifdef PERF_SLOW_LOG
uint32_t g_diag_fx_us = 0;   // DIAG:這一幀 draw_cb 累計微秒
uint32_t g_diag_part[4] = {0, 0, 0, 0};   // DIAG:扇形 / 海岸線 / 距離環 / 掃描線
#define FX_PART_BEGIN int64_t _pt = esp_timer_get_time();
#define FX_PART_END(k) g_diag_part[k] += (uint32_t) (esp_timer_get_time() - _pt);
struct FxTimer {
  int64_t t0 = esp_timer_get_time();
  ~FxTimer() { g_diag_fx_us += (uint32_t) (esp_timer_get_time() - t0); }
};
#else
#define FX_PART_BEGIN
#define FX_PART_END(k)
#endif
inline void draw_cb(lv_event_t *e) {
#ifdef PERF_SLOW_LOG
  FxTimer _fx_t;
#endif
  lv_obj_t *o = (lv_obj_t *) lv_event_get_target(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
#if LVGL_VERSION_MAJOR >= 9
  lv_layer_t *ctx = lv_event_get_layer(e);
  const lv_area_t clip = ctx->_clip_area;
#else
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  const lv_area_t clip = *ctx->clip_area;
#endif
  const int ox = a.x1, oy = a.y1;                 // 畫布座標 → 螢幕座標
  const int cx = ox + RADAR_CX, cy = oy + RADAR_CX;
  const float ang = sweepfx::g_ang;

  // 1) 扇形(最底下)
  if (!g_static) {
    FX_PART_BEGIN
    for (int k = 0; k < sweepfx::FAN_N; k++) {
      int x0, y0, x1, y1;
      polar(ang - k * sweepfx::FAN_STEP, RADAR_R, x0, y0);
      polar(ang - (k + 1) * sweepfx::FAN_STEP, RADAR_R, x1, y1);
      lv_color_t c;
      lv_opa_t op;
      fan_style(k, c, op);
#if LVGL_VERSION_MAJOR >= 9
      lv_draw_triangle_dsc_t d;
      lv_draw_triangle_dsc_init(&d);
      d.color = c;
      d.opa = op;
      d.p[0].x = cx; d.p[0].y = cy;
      d.p[1].x = ox + x0; d.p[1].y = oy + y0;
      d.p[2].x = ox + x1; d.p[2].y = oy + y1;
      lv_draw_triangle(ctx, &d);
#else
      lv_draw_rect_dsc_t d;
      lv_draw_rect_dsc_init(&d);
      d.bg_color = c;
      d.bg_opa = op;
      lv_point_t p[3] = {{(lv_coord_t) cx, (lv_coord_t) cy},
                         {(lv_coord_t) (ox + x0), (lv_coord_t) (oy + y0)},
                         {(lv_coord_t) (ox + x1), (lv_coord_t) (oy + y1)}};
      lv_draw_polygon(ctx, &d, p, 3);
#endif
    }
    FX_PART_END(0)
  }

  // 2) 海岸線:掃過後扇形範圍內亮(掃描線關掉時整條畫原色)
  int cx1 = clip.x1 - ox, cy1 = clip.y1 - oy, cx2 = clip.x2 - ox, cy2 = clip.y2 - oy;
  {
  FX_PART_BEGIN
  for (const CoastSeg &c : g_coast) {
    if (c.bx2 < cx1 || c.bx1 > cx2 || c.by2 < cy1 || c.by1 > cy2) continue;
    int lv = 0;
    if (!g_static) {
      int d = (g_sweep - c.ang + 3600) % 3600;
      lv = level_of(d);
      if (lv >= NB - 1) continue;   // 已經暗回底色:底圖裡就有
    }
    fx_line(ctx, ox + c.x1, oy + c.y1, ox + c.x2, oy + c.y2, color_of(lv), 2, LV_OPA_COVER);
  }
  FX_PART_END(1)
  }
  if (g_static) return;

  // 3) 距離環:真實 PPI 的距離環是視訊裡的電子脈衝,跟回波一樣被電子束畫亮、再暗回去。
  //    扇形範圍內那一小段弧畫亮一點(底圖裡是 0x006030)。十字線是刻度板,不亮。
  //    用每 1° 一段的短直線(弦)畫,不用 lv_draw_arc:LVGL 8 每次畫弧都要建圓形遮罩,
  //    遮罩快取只有 4 格、我們的半徑種類比這多,5B 上光這一項每幀 14–22ms。2° 的弧在最外圈
  //    也才十來個像素,弦跟弧差不到 0.1 像素。
  const int RR[4] = {RADAR_CX, RADAR_CX * 3 / 4, RADAR_CX / 2, RADAR_CX / 4};
  {
  FX_PART_BEGIN
  for (int k = 0; k < NB - 1; k++) {
    float b = BRIGHT[k];
    lv_color_t col = lv_color_make((uint8_t) (0x60 * b), (uint8_t) (0x60 + (0xFF - 0x60) * b),
                                   (uint8_t) (0x30 + (0xA0 - 0x30) * b));
    float d1 = (g_sweep - BOUND[k + 1]) / 10.0f, d2 = (g_sweep - BOUND[k]) / 10.0f;
    for (int r = 0; r < 4; r++) {
      // 這一圈這一段的外接矩形不在 clip 裡就跳過(大部分的段都不在)
      int ax, ay, bx, by;
      polar(d1, RR[r], ax, ay);
      polar(d2, RR[r], bx, by);
      if (LV_MAX(ax, bx) + 1 < cx1 || LV_MIN(ax, bx) - 1 > cx2 || LV_MAX(ay, by) + 1 < cy1 ||
          LV_MIN(ay, by) - 1 > cy2)
        continue;
      int n = (int) ceilf(d2 - d1);
      if (n < 1) n = 1;
      int px = ax, py = ay;
      for (int s = 1; s <= n; s++) {
        int qx, qy;
        polar(d1 + (d2 - d1) * s / n, RR[r], qx, qy);
        fx_line(ctx, ox + px, oy + py, ox + qx, oy + qy, col, 1, LV_OPA_COVER);
        px = qx; py = qy;
      }
    }
  }
  FX_PART_END(2)
  }

  // 4) 掃描線:光暈在下、亮芯在上(亮白綠;純白在實機上太白)
  int tx, ty;
  polar(ang, RADAR_R, tx, ty);
  {
  FX_PART_BEGIN
  fx_line(ctx, cx, cy, ox + tx, oy + ty, lv_color_hex(0x00D070), RS(8), (lv_opa_t) 56);
  fx_line(ctx, cx, cy, ox + tx, oy + ty, lv_color_hex(0x7CFFAC), 2, (lv_opa_t) 230);
  FX_PART_END(3)
  }
}

// 蓋在底圖上、同位置同大小;z 順序緊貼底圖之上(航班、標籤在它上面)
inline void create(lv_obj_t *canvas) {
  if (g_obj) return;
  lv_obj_t *page = lv_obj_get_parent(canvas);
  g_obj = lv_obj_create(page);
  lv_obj_remove_style_all(g_obj);
  lv_obj_clear_flag(g_obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(g_obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(g_obj, lv_obj_get_x(canvas), lv_obj_get_y(canvas));
  lv_obj_set_size(g_obj, RADAR_CANVAS, RADAR_CANVAS);
  lv_obj_add_event_cb(g_obj, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_move_to_index(g_obj, 1);   // 0 = 底圖
}

// 掃描線開關:關掉時不畫掃描線 / 扇形,海岸線全部畫原色
inline void set_static(bool st) {
  if (g_static == st) return;
  g_static = st;
  if (g_obj) lv_obj_invalidate(g_obj);
}

// 每幀:把「上一幀到這一幀掃過的楔形」(含扇形長度與線寬邊距)沿半徑每 48px 切一段,
// 每段只失效它的外接矩形。掃描線 / 扇形 / 海岸線 / 距離環的新舊位置都落在這些段裡。
inline void step(float prev, float cur) {
  g_sweep = (int16_t) ((int) (cur * 10.0f) % 3600);
  sweepfx::g_ang = cur;
  if (!g_obj || g_static) return;
  float a0 = prev - sweepfx::FAN_N * sweepfx::FAN_STEP - 1.0f, a1 = cur + 1.0f;
  while (a1 < a0) a1 += 360.0f;
  lv_area_t oa;
  lv_obj_get_coords(g_obj, &oa);
  if (a1 - a0 > 45.0f) {   // 跳太多(剛從暗螢幕回來、剛改轉速):整片重畫一次
    lv_obj_invalidate(g_obj);
    return;
  }
  const int S = RS(48);                         // 每段的半徑長度(48:段數少一點,省 LVGL 的待重畫格)
  const int M = RS(8) / 2 + 4;                  // 邊距:光暈半寬 + 海岸線 2px + 抗鋸齒
  const int R = RADAR_CX + 1;                   // 最外圈距離環
  for (int r0 = 0; r0 < R; r0 += S) {
    int r1 = LV_MIN(r0 + S, R);
    int x1 = 32767, y1 = 32767, x2 = -32768, y2 = -32768;
    auto add = [&](float deg, float r) {
      int x, y;
      polar(deg, r, x, y);
      x1 = LV_MIN(x1, x); y1 = LV_MIN(y1, y); x2 = LV_MAX(x2, x); y2 = LV_MAX(y2, y);
    };
    add(a0, r0); add(a1, r0); add(a0, r1); add(a1, r1);
    // 楔形跨過正北 / 東 / 南 / 西時,外弧的最外點在軸上
    for (int q = 0; q < 8; q++) {
      float axis = q * 90.0f;
      if (axis > a0 && axis < a1) add(axis, r1);
    }
    lv_area_t t;
    t.x1 = oa.x1 + LV_MAX(0, x1 - M);
    t.y1 = oa.y1 + LV_MAX(0, y1 - M);
    t.x2 = oa.x1 + LV_MIN(RADAR_CANVAS - 1, x2 + M);
    t.y2 = oa.y1 + LV_MIN(RADAR_CANVAS - 1, y2 + M);
    lv_obj_invalidate_area(g_obj, &t);
  }
}

}  // namespace coastfx
