#pragma once
// #22 第二階段:各航班「進入畫面以來」的航跡歷史(給選中航班畫完整航跡用)
// - 以 ICAO24 hex 為 key,不綁顯示槽:飛機暫時被擠出名單再回來也接得上
// - 固定大小池:SLOTS 條 × PTS 點,經緯度存 PSRAM(約 40KB),執行中不再配置
// - 點滿就每隔一點丟一點(最舊與最新都保留),早期路線變稀疏但不會斷
// - 只在主迴圈存取(資料進來時 add、每秒繪製時 find),不需要 mutex
#include <cstdint>
#include <cstdlib>
#include "esp_heap_caps.h"

namespace trk {

constexpr int SLOTS = 40;                   // 與 AC_SLOTS 同量級
constexpr int PTS = 128;                    // 每條上限;30s 抓一次 ≈ 1 小時不必抽稀
constexpr uint32_t STALE_MS = 10 * 60000;   // 超過 10 分鐘沒更新 = 已離開,重來

struct Track {
  uint32_t hex = 0;        // 0 = 空槽
  uint32_t last_ms = 0;    // 最後一次 add 的 millis
  uint16_t n = 0;          // 目前點數,索引 0=最舊
  float *lat = nullptr;
  float *lon = nullptr;
};

inline Track g_tracks[SLOTS];

inline bool init() {
  static bool done = false, ok = false;
  if (done) return ok;
  done = true;
  size_t bytes = sizeof(float) * SLOTS * PTS;
  float *a = (float *) heap_caps_malloc(bytes * 2, MALLOC_CAP_SPIRAM);
  if (!a) a = (float *) malloc(bytes * 2);
  if (!a) return false;
  for (int s = 0; s < SLOTS; s++) {
    g_tracks[s].lat = a + s * PTS;
    g_tracks[s].lon = a + SLOTS * PTS + s * PTS;
  }
  ok = true;
  return true;
}

inline const Track *find(uint32_t hex) {
  if (hex == 0) return nullptr;
  for (auto &t : g_tracks)
    if (t.hex == hex) return &t;
  return nullptr;
}

// 新資料進來時,每架呼叫一次
inline void add(uint32_t hex, float lat, float lon, uint32_t now) {
  if (hex == 0 || !init()) return;
  Track *t = const_cast<Track *>(find(hex));
  if (t && now - t->last_ms > STALE_MS) t->n = 0;   // 離開太久:舊航跡作廢
  if (!t) {   // 新面孔:佔空槽,沒有空槽就回收最久沒更新的
    t = &g_tracks[0];
    for (auto &c : g_tracks) {
      if (c.hex == 0) { t = &c; break; }
      if (c.last_ms < t->last_ms) t = &c;
    }
    t->hex = hex;
    t->n = 0;
  }
  t->last_ms = now;
  if (t->n > 0 && t->lat[t->n - 1] == lat && t->lon[t->n - 1] == lon) return;   // 位置沒更新
  if (t->n >= PTS) {   // 抽稀:保留偶數索引(含最舊),新點接在後面
    int m = 0;
    for (int k = 0; k < PTS; k += 2, m++) { t->lat[m] = t->lat[k]; t->lon[m] = t->lon[k]; }
    t->n = m;
  }
  t->lat[t->n] = lat;
  t->lon[t->n] = lon;
  t->n++;
}

}  // namespace trk
