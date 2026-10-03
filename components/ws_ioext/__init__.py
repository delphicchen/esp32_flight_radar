# =====================================================================
#  ws_ioext — Waveshare ESP32-S3-Touch-LCD-7B 的 IO 擴充晶片
#
#  5/5B 用的是 CH422G(ESPHome 有內建 ch422g 元件),7B 換成微雪自家韌體的
#  MCU,協定完全不同:單一 I2C 位址 0x24,以暫存器操作:
#    0x02 腳位模式(1 = 輸出)  0x03 輸出值  0x04 輸入值  0x05 背光 PWM(0-255)
#  協定取自 waveshareteam/ESP32-S3-Touch-LCD-7B 的
#  examples/Arduino/examples/06_LCD/io_extension.{h,cpp}。
#
#  用法與 ch422g 相同,可直接放進任何 pin: 區塊:
#    pin: { ws_ioext: io_ex, number: 3 }
#  另有 output 平台(output.py)提供真正的背光 PWM。
# =====================================================================
from esphome import pins
import esphome.codegen as cg
from esphome.components import i2c
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_INPUT,
    CONF_INVERTED,
    CONF_MODE,
    CONF_NUMBER,
    CONF_OUTPUT,
)

DEPENDENCIES = ["i2c"]
MULTI_CONF = True
ws_ioext_ns = cg.esphome_ns.namespace("ws_ioext")

WsIoExtComponent = ws_ioext_ns.class_(
    "WsIoExtComponent", cg.Component, i2c.I2CDevice
)
WsIoExtGPIOPin = ws_ioext_ns.class_(
    "WsIoExtGPIOPin", cg.GPIOPin, cg.Parented.template(WsIoExtComponent)
)

CONF_WS_IOEXT = "ws_ioext"
CONF_INITIAL_OUTPUT = "initial_output"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WsIoExtComponent),
            # 開機時一次寫入的輸出值(bit n = IOn)。預設 0xDF:全部拉高,只有
            # IO5 拉低 —— IO5 選擇 GPIO19/20 走 USB(0)還是 CAN(1),
            # 拉高會讓 USB 序列埠/燒錄口斷線。
            cv.Optional(CONF_INITIAL_OUTPUT, default=0xDF): cv.hex_uint8_t,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x24))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)
    cg.add(var.set_initial_output(config[CONF_INITIAL_OUTPUT]))


WS_IOEXT_PIN_SCHEMA = pins.gpio_base_schema(
    WsIoExtGPIOPin,
    cv.int_range(min=0, max=7),
    modes=[CONF_INPUT, CONF_OUTPUT],
).extend(
    {
        cv.Required(CONF_WS_IOEXT): cv.use_id(WsIoExtComponent),
    }
)


@pins.PIN_SCHEMA_REGISTRY.register(CONF_WS_IOEXT, WS_IOEXT_PIN_SCHEMA)
async def ws_ioext_pin_to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    parent = await cg.get_variable(config[CONF_WS_IOEXT])
    cg.add(var.set_parent(parent))
    cg.add(var.set_pin(config[CONF_NUMBER]))
    cg.add(var.set_inverted(config[CONF_INVERTED]))
    cg.add(var.set_flags(pins.gpio_flags_expr(config[CONF_MODE])))
    return var
