// 鬧鐘:ESP32 做排程與 UI,發聲有兩條路——Home Assistant 的喇叭
// (media_player.play_media),或板載 I2S 喇叭(見下方 RADAR_LOCAL_SPK)。
// 每組鬧鐘打包進一個 uint32_t:
//   bit0      : enabled
//   bit1..7   : 星期遮罩(bit1=Sun … bit7=Sat,週日起算)
//   bit8..12  : 時 (0-23)
//   bit13..18 : 分 (0-59)
#pragma once
#include <cstdint>
#include <string>

// 板載喇叭:有音訊硬體的板子在 boards/*.yaml 加 -DRADAR_LOCAL_SPK=1 打開,
// 鬧鐘頁的喇叭下拉才會多出 LOCAL SPEAKER 這一項。沒有的板子維持 0。
// 兩種板子都要提供 local_ring_start / local_ring_stop 兩個 script(沒喇叭的
// 是空實作),common/core.yaml 才能無條件呼叫。
#ifndef RADAR_LOCAL_SPK
#define RADAR_LOCAL_SPK 0
#endif

namespace alarmpk {

// 選到板載喇叭時寫進 Alarm Speaker 文字實體的哨兵值。
// HA 的 entity_id 一定是 "media_player.xxx" 這種含點的字串,不會與它相撞。
inline constexpr const char *LOCAL_SPK_ID   = "local";
inline constexpr const char *LOCAL_SPK_NAME = "LOCAL SPEAKER";

inline bool is_local_spk(const std::string &s) { return s == LOCAL_SPK_ID; }

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
inline uint32_t set_h(uint32_t a, int h)   { return pack(en(a), dow(a), h, mm(a)); }
inline uint32_t set_m(uint32_t a, int m)   { return pack(en(a), dow(a), hh(a), m); }
// k=0..6,週日起算(0=Sun … 6=Sat)—— 與 day_on()、iso_today() 同一套索引,
// 也與鬧鐘頁上 Su…Sa 的按鈕順序一致。改成週一起算會讓鬧鐘整整早響一天。
inline uint32_t toggle_day(uint32_t a, int k) {
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
