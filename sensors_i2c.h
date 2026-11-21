#pragma once
// #24 第三階段:I2C 本地感測器自動偵測
// 所有驅動都編進固件,開機時探測固定位址,只啟用有回應的;沒接感測器的板子什麼都不做。
// 讀取在主迴圈做(ESPHome 的 I2C bus 只在主迴圈用,GT911 也一樣),每步只做一次
// 短傳輸,等待量測完成靠 due 時間,不 delay(探測階段例外,只在開機跑一次)。
//   0x38 AHT20      溫濕度(Waveshare 板上和 CH422G 撞位址,由 -DSHUB_I2C_PROBE_38 控制)
//   0x44/0x45 SHT3x/SHT4x 溫濕度
//   0x76/0x77 BME280 溫濕度+氣壓 / BMP280 溫度+氣壓
//   0x62 SCD4x      CO2(它的溫濕度受自身發熱影響,只在沒有別的溫濕度晶片時才用)
#include <string>
#include <vector>
#include "esphome/components/i2c/i2c_bus.h"
#include "esphome/core/log.h"
#include "sensors_hub.h"

#ifndef SHUB_I2C_PROBE_38
#define SHUB_I2C_PROBE_38 0
#endif

namespace isens {
using esphome::i2c::I2CBus;
using esphome::i2c::ERROR_OK;
static const char *const TAG = "isens";

enum Chip : uint8_t { AHT20, SHT3X, SHT4X, BME280, BMP280, SCD4X };
static const char *const CHIP_NAME[] = {"AHT20", "SHT3x", "SHT4x", "BME280", "BMP280", "SCD4x"};
// 溫濕度的優先序:專用溫濕度晶片 > BME/BMP > SCD4x
static const uint8_t TH_PRIO[] = {3, 3, 3, 2, 2, 1};

struct Dev {
  Chip chip;
  uint8_t addr;
  uint8_t phase;     // 0 = 該送量測指令,1 = 等結果;SCD4x 另有 2/10/11
  uint32_t due;      // 下一步的 millis
  uint8_t fails;     // 連續失敗次數
  // BME/BMP280 校正參數
  uint16_t T1, P1;
  int16_t T2, T3, P2, P3, P4, P5, P6, P7, P8, P9, H2, H4, H5;
  uint8_t H1, H3;
  int8_t H6;
};

inline I2CBus *g_bus = nullptr;
inline std::vector<Dev> g_dev;
inline std::string g_found = "SCANNING";
inline int g_t_own = -1, g_h_own = -1, g_p_own = -1;   // 哪個裝置負責 IN_T / IN_H / PRES
inline bool g_probed = false;

inline uint8_t crc8(const uint8_t *d, int n) {
  uint8_t c = 0xFF;
  for (int i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t) ((c << 1) ^ 0x31) : (uint8_t) (c << 1);
  }
  return c;
}
// Sensirion 格式:每 2 bytes 跟 1 byte CRC
inline bool words_ok(const uint8_t *d, int words) {
  for (int i = 0; i < words; i++)
    if (crc8(d + 3 * i, 2) != d[3 * i + 2]) return false;
  return true;
}

inline bool present(uint8_t a) { return g_bus->write_readv(a, nullptr, 0, nullptr, 0) == ERROR_OK; }
inline bool wr(uint8_t a, const uint8_t *b, size_t n) { return g_bus->write_readv(a, b, n, nullptr, 0) == ERROR_OK; }
inline bool rd(uint8_t a, uint8_t *b, size_t n) { return g_bus->write_readv(a, nullptr, 0, b, n) == ERROR_OK; }
inline bool reg_rd(uint8_t a, uint8_t reg, uint8_t *b, size_t n) {
  return g_bus->write_readv(a, &reg, 1, b, n) == ERROR_OK;
}
inline bool reg_wr(uint8_t a, uint8_t reg, uint8_t v) {
  uint8_t b[2] = {reg, v};
  return wr(a, b, 2);
}
inline bool cmd16(uint8_t a, uint16_t c) {
  uint8_t b[2] = {(uint8_t) (c >> 8), (uint8_t) (c & 0xFF)};
  return wr(a, b, 2);
}

// ---- BME280 / BMP280 校正與補償(Bosch datasheet 的整數公式)----
inline bool bme_calib(Dev &d) {
  uint8_t c[24];
  if (!reg_rd(d.addr, 0x88, c, 24)) return false;
  auto u16 = [&](int i) { return (uint16_t) (c[i] | (c[i + 1] << 8)); };
  d.T1 = u16(0); d.T2 = (int16_t) u16(2); d.T3 = (int16_t) u16(4);
  d.P1 = u16(6); d.P2 = (int16_t) u16(8); d.P3 = (int16_t) u16(10); d.P4 = (int16_t) u16(12);
  d.P5 = (int16_t) u16(14); d.P6 = (int16_t) u16(16); d.P7 = (int16_t) u16(18);
  d.P8 = (int16_t) u16(20); d.P9 = (int16_t) u16(22);
  if (d.chip != BME280) return true;
  uint8_t e[7];
  if (!reg_rd(d.addr, 0xA1, &d.H1, 1) || !reg_rd(d.addr, 0xE1, e, 7)) return false;
  d.H2 = (int16_t) (e[0] | (e[1] << 8));
  d.H3 = e[2];
  d.H4 = (int16_t) (((int8_t) e[3]) * 16 | (e[4] & 0x0F));
  d.H5 = (int16_t) (((int8_t) e[5]) * 16 | (e[4] >> 4));
  d.H6 = (int8_t) e[6];
  return true;
}

inline void bme_comp(const Dev &d, int32_t aT, int32_t aP, int32_t aH, float &t, float &p, float &h) {
  int32_t v1 = ((((aT >> 3) - ((int32_t) d.T1 << 1))) * ((int32_t) d.T2)) >> 11;
  int32_t v2 = (((((aT >> 4) - ((int32_t) d.T1)) * ((aT >> 4) - ((int32_t) d.T1))) >> 12) * ((int32_t) d.T3)) >> 14;
  int32_t tf = v1 + v2;
  t = ((tf * 5 + 128) >> 8) / 100.0f;

  p = NAN;
  if (aP != 0x80000) {
    int64_t q1 = (int64_t) tf - 128000;
    int64_t q2 = q1 * q1 * (int64_t) d.P6;
    q2 = q2 + ((q1 * (int64_t) d.P5) << 17);
    q2 = q2 + (((int64_t) d.P4) << 35);
    q1 = ((q1 * q1 * (int64_t) d.P3) >> 8) + ((q1 * (int64_t) d.P2) << 12);
    q1 = ((((int64_t) 1) << 47) + q1) * ((int64_t) d.P1) >> 33;
    if (q1 != 0) {
      int64_t pp = 1048576 - aP;
      pp = (((pp << 31) - q2) * 3125) / q1;
      q1 = (((int64_t) d.P9) * (pp >> 13) * (pp >> 13)) >> 25;
      q2 = (((int64_t) d.P8) * pp) >> 19;
      pp = ((pp + q1 + q2) >> 8) + (((int64_t) d.P7) << 4);
      p = (float) pp / 256.0f / 100.0f;   // Pa → hPa
    }
  }

  h = NAN;
  if (d.chip == BME280 && aH != 0x8000) {
    int32_t v = tf - 76800;
    v = (((((aH << 14) - (((int32_t) d.H4) << 20) - (((int32_t) d.H5) * v)) + 16384) >> 15) *
         (((((((v * ((int32_t) d.H6)) >> 10) * (((v * ((int32_t) d.H3)) >> 11) + 32768)) >> 10) + 2097152) *
               ((int32_t) d.H2) + 8192) >> 14));
    v = v - (((((v >> 15) * (v >> 15)) >> 7) * ((int32_t) d.H1)) >> 4);
    v = v < 0 ? 0 : (v > 419430400 ? 419430400 : v);
    h = (v >> 12) / 1024.0f;
  }
}

// ---- 開機探測(只跑一次,可以短暫 delay)----
inline void add(Chip c, uint8_t a) {
  Dev d{};
  d.chip = c;
  d.addr = a;
  d.due = millis() + 200;
  if (c == SCD4X) d.phase = 10;   // 先停掉可能還在跑的週期量測(ESP 重開但感測器沒斷電)
  if ((c == BME280 || c == BMP280) && !bme_calib(d)) {
    ESP_LOGW(TAG, "%s@%02X calibration read failed", CHIP_NAME[c], a);
    return;
  }
  g_dev.push_back(d);
}

inline void probe(I2CBus *bus) {
  g_probed = true;
  g_bus = bus;
  if (!g_bus) { g_found = "NO BUS"; return; }
#if SHUB_I2C_PROBE_38
  if (present(0x38)) {
    uint8_t st = 0;
    if (rd(0x38, &st, 1)) {
      if (!(st & 0x08)) {   // 未校正 → 送初始化
        static const uint8_t init[3] = {0xBE, 0x08, 0x00};
        wr(0x38, init, 3);
        delay(10);
      }
      add(AHT20, 0x38);
    }
  }
#endif
  for (uint8_t a : {(uint8_t) 0x44, (uint8_t) 0x45}) {
    if (!present(a)) continue;
    // SHT4x 認得單 byte 的讀序號指令 0x89;SHT3x 不認得,讀回來會失敗或 CRC 錯
    uint8_t c = 0x89, b[6];
    bool v4 = wr(a, &c, 1);
    delay(2);
    v4 = v4 && rd(a, b, 6) && words_ok(b, 2);
    add(v4 ? SHT4X : SHT3X, a);
  }
  for (uint8_t a : {(uint8_t) 0x76, (uint8_t) 0x77}) {
    if (!present(a)) continue;
    uint8_t id = 0;
    if (!reg_rd(a, 0xD0, &id, 1)) continue;
    if (id == 0x60) add(BME280, a);
    else if (id == 0x58 || id == 0x56 || id == 0x57) add(BMP280, a);
    else ESP_LOGI(TAG, "unknown chip id 0x%02X at 0x%02X", id, a);
  }
  if (present(0x62)) add(SCD4X, 0x62);

  // 指定 IN_T / IN_H / PRES / CO2 由哪個裝置負責
  int bt = 0, bh = 0;
  for (int i = 0; i < (int) g_dev.size(); i++) {
    Chip c = g_dev[i].chip;
    if (TH_PRIO[c] > bt) { bt = TH_PRIO[c]; g_t_own = i; }
    if (c != BMP280 && TH_PRIO[c] > bh) { bh = TH_PRIO[c]; g_h_own = i; }
    if ((c == BME280 || c == BMP280) && g_p_own < 0) g_p_own = i;
  }
  if (g_t_own >= 0) shub::describe(shub::IN_T, nullptr, CHIP_NAME[g_dev[g_t_own].chip], nullptr, -1);
  if (g_h_own >= 0) shub::describe(shub::IN_H, nullptr, CHIP_NAME[g_dev[g_h_own].chip], nullptr, -1);
  if (g_p_own >= 0) shub::describe(shub::PRES, nullptr, CHIP_NAME[g_dev[g_p_own].chip], nullptr, -1);
  for (auto &d : g_dev)
    if (d.chip == SCD4X) shub::describe(shub::CO2, nullptr, "SCD4x", nullptr, -1);

  g_found.clear();
  char b[16];
  for (auto &d : g_dev) {
    snprintf(b, sizeof(b), "%s%s@%02X", g_found.empty() ? "" : " ", CHIP_NAME[d.chip], d.addr);
    g_found += b;
  }
  if (g_found.empty()) g_found = "NONE";
  ESP_LOGI(TAG, "I2C sensors: %s", g_found.c_str());
}

// 一筆量測結果交給 hub(只登記這個裝置負責的量)
inline void publish(int i, float t, float h, float p) {
  if (i == g_t_own && !std::isnan(t) && t > -40 && t < 85) shub::set(shub::IN_T, t);
  if (i == g_h_own && !std::isnan(h) && h >= 0 && h <= 100) shub::set(shub::IN_H, h);
  if (i == g_p_own && !std::isnan(p) && p > 300 && p < 1100) shub::set(shub::PRES, p);
}

inline void fail(Dev &d, uint32_t now) {
  if (++d.fails == 5) ESP_LOGW(TAG, "%s@%02X not responding", CHIP_NAME[d.chip], d.addr);
  d.phase = d.chip == SCD4X && d.fails >= 5 ? 10 : 0;   // SCD4x 連續失敗就從停止/啟動重來
  d.due = now + (d.fails >= 5 ? 60000 : 5000);
}
inline void ok(Dev &d) {
  if (d.fails >= 5) ESP_LOGI(TAG, "%s@%02X back", CHIP_NAME[d.chip], d.addr);
  d.fails = 0;
}

static const uint32_t TH_PERIOD = 10000;   // 溫濕度/氣壓每 10 秒一筆(forced mode,自身發熱可忽略)

// 主迴圈每 100ms 呼叫:每個裝置到時間才做一步
inline void tick() {
  if (!g_probed) return;
  uint32_t now = millis();
  for (int i = 0; i < (int) g_dev.size(); i++) {
    Dev &d = g_dev[i];
    if ((int32_t) (now - d.due) < 0) continue;
    switch (d.chip) {
      case AHT20: {
        if (d.phase == 0) {
          static const uint8_t m[3] = {0xAC, 0x33, 0x00};
          if (!wr(d.addr, m, 3)) { fail(d, now); break; }
          d.phase = 1; d.due = now + 90;
        } else {
          uint8_t b[7];
          if (!rd(d.addr, b, 7)) { fail(d, now); break; }
          if (b[0] & 0x80) { d.due = now + 30; break; }   // 還在量
          if (crc8(b, 6) != b[6]) { fail(d, now); break; }
          uint32_t rh = ((uint32_t) b[1] << 12) | ((uint32_t) b[2] << 4) | (b[3] >> 4);
          uint32_t rt = ((uint32_t) (b[3] & 0x0F) << 16) | ((uint32_t) b[4] << 8) | b[5];
          ok(d);
          publish(i, rt / 1048576.0f * 200.0f - 50.0f, rh / 1048576.0f * 100.0f, NAN);
          d.phase = 0; d.due = now + TH_PERIOD;
        }
        break;
      }
      case SHT3X:
      case SHT4X: {
        if (d.phase == 0) {
          bool s;
          if (d.chip == SHT4X) { uint8_t c = 0xFD; s = wr(d.addr, &c, 1); }
          else s = cmd16(d.addr, 0x2400);   // 單次、高重複性、不 clock stretch
          if (!s) { fail(d, now); break; }
          d.phase = 1; d.due = now + 25;
        } else {
          uint8_t b[6];
          if (!rd(d.addr, b, 6) || !words_ok(b, 2)) { fail(d, now); break; }
          float t = -45.0f + 175.0f * ((b[0] << 8) | b[1]) / 65535.0f;
          float raw = ((b[3] << 8) | b[4]) / 65535.0f;
          float h = d.chip == SHT4X ? -6.0f + 125.0f * raw : 100.0f * raw;
          h = h < 0 ? 0 : (h > 100 ? 100 : h);
          ok(d);
          publish(i, t, h, NAN);
          d.phase = 0; d.due = now + TH_PERIOD;
        }
        break;
      }
      case BME280:
      case BMP280: {
        if (d.phase == 0) {
          bool s = d.chip != BME280 || reg_wr(d.addr, 0xF2, 0x01);   // 濕度 x1(要先寫才生效)
          s = s && reg_wr(d.addr, 0xF4, 0x25);                         // 溫度 x1、氣壓 x1、forced
          if (!s) { fail(d, now); break; }
          d.phase = 1; d.due = now + 50;
        } else {
          uint8_t b[8];
          int n = d.chip == BME280 ? 8 : 6;
          if (!reg_rd(d.addr, 0xF7, b, n)) { fail(d, now); break; }
          int32_t aP = ((int32_t) b[0] << 12) | ((int32_t) b[1] << 4) | (b[2] >> 4);
          int32_t aT = ((int32_t) b[3] << 12) | ((int32_t) b[4] << 4) | (b[5] >> 4);
          int32_t aH = n == 8 ? (((int32_t) b[6] << 8) | b[7]) : 0x8000;
          if (aT == 0x80000) { fail(d, now); break; }
          float t, p, h;
          bme_comp(d, aT, aP, aH, t, p, h);
          ok(d);
          publish(i, t, h, p);
          d.phase = 0; d.due = now + TH_PERIOD;
        }
        break;
      }
      case SCD4X: {
        if (d.phase == 10) {          // stop_periodic_measurement,之後要等 500ms
          cmd16(d.addr, 0x3F86);
          d.phase = 11; d.due = now + 600;
        } else if (d.phase == 11) {   // start_periodic_measurement(每 5 秒一筆)
          if (!cmd16(d.addr, 0x21B1)) { fail(d, now); break; }
          d.phase = 0; d.due = now + 5500;
        } else if (d.phase == 0) {    // get_data_ready_status
          if (!cmd16(d.addr, 0xE4B8)) { fail(d, now); break; }
          d.phase = 1; d.due = now + 5;
        } else if (d.phase == 1) {
          uint8_t b[3];
          if (!rd(d.addr, b, 3) || !words_ok(b, 1)) { fail(d, now); break; }
          if ((((b[0] << 8) | b[1]) & 0x07FF) == 0) { d.phase = 0; d.due = now + 1000; break; }
          if (!cmd16(d.addr, 0xEC05)) { fail(d, now); break; }   // read_measurement
          d.phase = 2; d.due = now + 5;
        } else {
          uint8_t b[9];
          if (!rd(d.addr, b, 9) || !words_ok(b, 3)) { fail(d, now); break; }
          uint16_t co2 = (b[0] << 8) | b[1];
          float t = -45.0f + 175.0f * ((b[3] << 8) | b[4]) / 65535.0f;
          float h = 100.0f * ((b[6] << 8) | b[7]) / 65535.0f;
          ok(d);
          if (co2 > 0) shub::set(shub::CO2, (float) co2);   // 剛啟動的第一筆可能是 0
          publish(i, t, h, NAN);
          d.phase = 0; d.due = now + 5000;
        }
        break;
      }
    }
  }
}

// 網頁/HA 用:這個量目前的值(沒資料回 NAN)
inline float value(shub::Key k) {
  shub::init();
  const shub::Reading &r = shub::g_rd[k];
  return r.registered && shub::fresh(r) ? r.value : NAN;
}

}  // namespace isens
