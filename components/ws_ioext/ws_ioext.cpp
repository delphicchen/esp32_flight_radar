#include "ws_ioext.h"
#include "esphome/core/log.h"

namespace esphome {
namespace ws_ioext {

static const char *const TAG = "ws_ioext";

void WsIoExtComponent::setup() {
  // 全部設成輸出,再一次寫入初始輸出值(官方範例同樣是 0xFF 全輸出)
  if (!this->write_reg_(REG_MODE, 0xFF) || !this->write_reg_(REG_OUTPUT, this->output_bits_)) {
    this->mark_failed();
    return;
  }
  if (this->touch_int_ != nullptr)
    this->reset_touch_();
}

// 照官方 08_TOUCH 的 touch_gt911_init():RST 低 → INT 低 → RST 高。重置放開時
// INT 為低 → GT911 用位址 0x5D。共阻塞約 400ms,只在開機做一次。
void WsIoExtComponent::reset_touch_() {
  this->touch_int_->setup();
  this->touch_int_->pin_mode(gpio::FLAG_OUTPUT);
  this->touch_int_->digital_write(false);
  this->digital_write(this->touch_rst_, false);
  delay(100);
  this->touch_int_->digital_write(false);
  delay(100);
  this->digital_write(this->touch_rst_, true);
  delay(200);
  this->touch_int_->pin_mode(gpio::FLAG_INPUT);  // 交還給 gt911 當中斷輸入
  ESP_LOGD(TAG, "GT911 reset via EXIO%u done", this->touch_rst_);
}

void WsIoExtComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Waveshare IO extension:");
  LOG_I2C_DEVICE(this);
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Communication failed");
  }
}

bool WsIoExtComponent::digital_read(uint8_t pin) {
  uint8_t v = 0;
  if (this->read_register(REG_INPUT, &v, 1) != i2c::ERROR_OK)
    return false;
  return v & (1 << pin);
}

void WsIoExtComponent::digital_write(uint8_t pin, bool value) {
  if (value) {
    this->output_bits_ |= (1 << pin);
  } else {
    this->output_bits_ &= ~(1 << pin);
  }
  this->write_reg_(REG_OUTPUT, this->output_bits_);
}

void WsIoExtComponent::write_pwm(uint8_t value) { this->write_reg_(REG_PWM, value); }

bool WsIoExtComponent::write_reg_(uint8_t reg, uint8_t value) {
  if (this->write_register(reg, &value, 1) != i2c::ERROR_OK) {
    ESP_LOGW(TAG, "write reg 0x%02X failed", reg);
    return false;
  }
  return true;
}

size_t WsIoExtGPIOPin::dump_summary(char *buffer, size_t len) const {
  return snprintf(buffer, len, "EXIO%u via ws_ioext", this->pin_);
}

#ifdef USE_OUTPUT
void WsIoExtPwmOutput::write_state(float state) {
  // 極性是反的:PWM 值越大越暗(實機回報,#17)。官方 IO_EXTENSION_Pwm_Output()
  // 把上限夾在 97%(≈247)免得全黑,滑桿照做。PWR 鍵關燈(state 0):有 enable 腳時
  // 也只寫 247 —— 與 ESPHome 官方 waveshare_io_ch32v003 的「7B 安全上限」相同。
  // v1.3.17 寫 255 又同時拉低 enable,CH32V003 之後就叫不回背光(#25)。
  // 沒有 enable 腳時沿用 255,只能靠 PWM 盡量暗。
  const uint8_t off_v = this->enable_pin_ >= 0 ? 247 : 255;
  uint8_t v = state <= 0.0f ? off_v : (uint8_t) ((1.0f - state) * 247.0f + 0.5f);
  // 7B 原理圖:DISP(EXIO2)與 EXIO_PWM 同接 AP3032 的 EN。DISP 一直拉高時,PWM
  // 寫 255 也拉不到底,背光只會變暗不會滅(#25)→ 關燈時一併拉低 enable 腳。
  // 開燈要先拉回 enable 再寫 PWM;只在狀態改變時寫,拖亮度滑桿不多打 I2C。
  const bool on = state > 0.0f;
  if (this->enable_pin_ >= 0 && on && !this->enabled_) {
    this->parent_->digital_write(this->enable_pin_, true);
    this->enabled_ = true;
  }
  this->parent_->write_pwm(v);
  if (this->enable_pin_ >= 0 && !on && this->enabled_) {
    this->parent_->digital_write(this->enable_pin_, false);
    this->enabled_ = false;
  }
}
#endif

}  // namespace ws_ioext
}  // namespace esphome
