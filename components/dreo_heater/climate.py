import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, uart
from esphome.const import CONF_ID, CONF_UART_ID

DEPENDENCIES = ['uart']

dreo_heater_ns = cg.esphome_ns.namespace('dreo_heater')
DreoHeater = dreo_heater_ns.class_('DreoHeater', climate.Climate, uart.UARTDevice, cg.Component)

# Define the configuration key constant
CONF_DEBUG = "debug"

CONFIG_SCHEMA = climate._CLIMATE_SCHEMA.extend({
    cv.GenerateID(): cv.declare_id(DreoHeater),
    # Add the debug option (defaults to False if not specified)
    cv.Optional(CONF_DEBUG, default=False): cv.boolean,
}).extend(uart.UART_DEVICE_SCHEMA).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    uart_component = await cg.get_variable(config[CONF_UART_ID])
    var = cg.new_Pvariable(config[CONF_ID], uart_component)
    await cg.register_component(var, config)
    await climate.register_climate(var, config)
    
    # Pass the debug configuration to C++
    if config[CONF_DEBUG]:
        cg.add(var.set_debug(True))
    # FIX: Use the full path so the compiler finds the file inside the component folder
    cg.add_global(cg.RawStatement('#include "esphome/components/dreo_heater/dreo_heater.h"'))