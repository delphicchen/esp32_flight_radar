// 鬧鐘:ESP32 只做排程與 UI,實際發聲由 Home Assistant 的喇叭
// (media_player.play_media)播放。每組鬧鐘打包進一個 uint32_t:
//   bit0      : enabled
//   bit1..7   : 星期遮罩(bit1=Sun … bit7=Sat,週日起算)
//   bit8..12  : 時 (0-23)
//   bit13..18 : 分 (0-59)
#pragma once
#include <cstdint>

namespace alarmpk {

inline bool     en(uint32_t a)  { return a & 1u; }
inline uint8_t  dow(uint32_t a) { return (a >> 1) & 0x7Fu; }
inline int      hh(uint32_t a)  { return (a >> 8) & 0x1Fu; }
inline int      mm(uint32_t a)  { return (a >> 13) & 0x3Fu; }

inline uint32_t pack(bool e, uint8_t d, int h, int m) {
  return (e ? 1u : 0u) | ((uint32_t)(d & 0x7F) << 1) |
         ((uint32_t)(h & 0x1F) << 8) | ((uint32_t)(m & 0x3F) << 13);
}

inline uint32_t set_en(uint32_t a, bool e) { return pack(e, dow(a), hh(a), mm(a)); }
inline uint32_t toggle_en(uint32_t a)      { return set_en(a, !en(a)); }
inline uint32_t add_h(uint32_t a, int d)   { return pack(en(a), dow(a), (hh(a) + d + 24) % 24, mm(a)); }
inline uint32_t add_m(uint32_t a, int d)   { return pack(en(a), dow(a), hh(a), (mm(a) + d + 60) % 60); }
inline uint32_t toggle_day(uint32_t a, int k) {   // k=0..6 (一..日)
  return pack(en(a), dow(a) ^ (uint8_t)(1u << k), hh(a), mm(a));
}
inline bool day_on(uint32_t a, int k) { return (dow(a) >> k) & 1u; }

// ESPHome day_of_week:1=週日 … 7=週六 → 週日起算 index:週日=0 … 週六=6
inline int iso_today(int day_of_week) { return day_of_week - 1; }

// 是否此刻該響
inline bool due(uint32_t a, int hour, int minute, int iso) {
  return en(a) && hh(a) == hour && mm(a) == minute && ((dow(a) >> iso) & 1u);
}

}  // namespace alarmpk
