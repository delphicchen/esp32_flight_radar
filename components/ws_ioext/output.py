# ws_ioext 的背光 PWM(暫存器 0x05)→ ESPHome float output,
# 可直接接 light: monochromatic,亮度滑桿就是真的調光。
import esphome.codegen as cg
from esphome.components import output
import esphome.config_validation as cv
from esphome.const import CONF_ENABLE_PIN, CONF_ID

from . import CONF_WS_IOEXT, WsIoExtComponent, ws_ioext_ns

DEPENDENCIES = ["ws_ioext"]

WsIoExtPwmOutput = ws_ioext_ns.class_(
    "WsIoExtPwmOutput", output.FloatOutput, cg.Parented.template(WsIoExtComponent)
)

CONFIG_SCHEMA = output.FLOAT_OUTPUT_SCHEMA.extend(
    {
        cv.Required(CONF_ID): cv.declare_id(WsIoExtPwmOutput),
        cv.GenerateID(CONF_WS_IOEXT): cv.use_id(WsIoExtComponent),
        # 背光 enable 所在的 EXIO 腳(選用)。7B 的 DISP(EXIO2)與 EXIO_PWM 接在
        # 同一條 AP3032 EN 線上,關燈時兩者都要拉低才會全滅(#25)。
        cv.Optional(CONF_ENABLE_PIN): cv.int_range(min=0, max=7),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await output.register_output(var, config)
    await cg.register_parented(var, config[CONF_WS_IOEXT])
    if CONF_ENABLE_PIN in config:
        cg.add(var.set_enable_pin(config[CONF_ENABLE_PIN]))
