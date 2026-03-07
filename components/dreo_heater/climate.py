import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, uart, switch
from esphome.const import CONF_ID, CONF_UART_ID

DEPENDENCIES = ['uart']

CONF_DEBUG = "debug"
CONF_TEMP_UNIT_SWITCH = "temp_unit_switch"

dreo_heater_ns = cg.esphome_ns.namespace('dreo_heater')
DreoHeater = dreo_heater_ns.class_('DreoHeater', climate.Climate, uart.UARTDevice, cg.Component)

CONFIG_SCHEMA = climate._CLIMATE_SCHEMA.extend({
    cv.GenerateID(): cv.declare_id(DreoHeater),
    cv.Optional(CONF_DEBUG, default=False): cv.boolean,
    cv.Optional(CONF_TEMP_UNIT_SWITCH): cv.use_id(switch.Switch),
    cv.Optional("unit_switch"): cv.use_id(switch.Switch),
}).extend(uart.UART_DEVICE_SCHEMA).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    uart_component = await cg.get_variable(config[CONF_UART_ID])
    var = cg.new_Pvariable(config[CONF_ID], uart_component)
    await cg.register_component(var, config)
    await climate.register_climate(var, config)

    if config.get(CONF_TEMP_UNIT_SWITCH) is not None:
        temp_unit_switch = await cg.get_variable(config[CONF_TEMP_UNIT_SWITCH])
        cg.add(var.set_temp_unit_switch(temp_unit_switch))

    if config.get("unit_switch") is not None:
        unit_switch_var = await cg.get_variable(config["unit_switch"])
        cg.add(var.set_unit_switch(unit_switch_var))

    if config[CONF_DEBUG]:
        cg.add(var.set_debug(True))
    cg.add_global(cg.RawStatement('#include "esphome/components/dreo_heater/dreo_heater.h"'))
