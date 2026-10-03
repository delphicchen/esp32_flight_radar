#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/i2c/i2c.h"
#ifdef USE_OUTPUT
#include "esphome/components/output/float_output.h"
#endif

namespace esphome {
namespace ws_ioext {

// 暫存器位址(見 __init__.py 的協定說明)
static const uint8_t REG_MODE = 0x02;
static const uint8_t REG_OUTPUT = 0x03;
static const uint8_t REG_INPUT = 0x04;
static const uint8_t REG_PWM = 0x05;

class WsIoExtComponent : public Component, public i2c::I2CDevice {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::IO; }

  void set_initial_output(uint8_t v) { this->output_bits_ = v; }
  bool digital_read(uint8_t pin);
  void digital_write(uint8_t pin, bool value);
  void write_pwm(uint8_t value);

 protected:
  bool write_reg_(uint8_t reg, uint8_t value);
  // 晶片只能整個位元組寫入,所以保留一份輸出影子暫存器
  uint8_t output_bits_{0xDF};
};

class WsIoExtGPIOPin : public GPIOPin, public Parented<WsIoExtComponent> {
 public:
  void setup() override {}
  // 所有腳在 setup() 已設成輸出;輸入讀暫存器 0x04 不受模式影響
  void pin_mode(gpio::Flags flags) override {}
  bool digital_read() override { return this->parent_->digital_read(this->pin_) ^ this->inverted_; }
  void digital_write(bool value) override { this->parent_->digital_write(this->pin_, value ^ this->inverted_); }
  size_t dump_summary(char *buffer, size_t len) const override;

  void set_pin(uint8_t pin) { this->pin_ = pin; }
  void set_inverted(bool inverted) { this->inverted_ = inverted; }
  void set_flags(gpio::Flags flags) { this->flags_ = flags; }
  gpio::Flags get_flags() const override { return this->flags_; }

 protected:
  uint8_t pin_{};
  bool inverted_{};
  gpio::Flags flags_{};
};

#ifdef USE_OUTPUT
class WsIoExtPwmOutput : public output::FloatOutput, public Parented<WsIoExtComponent> {
 protected:
  void write_state(float state) override;
};
#endif

}  // namespace ws_ioext
}  // namespace esphome
