#include "ws_ioext.h"
#include "esphome/core/log.h"

namespace esphome {
namespace ws_ioext {

static const char *const TAG = "ws_ioext";

void WsIoExtComponent::setup() {
  // 全部設成輸出,再一次寫入初始輸出值(官方範例同樣是 0xFF 全輸出)
  if (!this->write_reg_(REG_MODE, 0xFF) || !this->write_reg_(REG_OUTPUT, this->output_bits_)) {
    this->mark_failed();
  }
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
  // 官方 IO_EXTENSION_Pwm_Output() 把上限夾在 97%(≈247),照做
  this->parent_->write_pwm((uint8_t) (state * 247.0f + 0.5f));
}
#endif

}  // namespace ws_ioext
}  // namespace esphome
