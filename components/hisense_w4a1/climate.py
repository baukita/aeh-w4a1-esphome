import esphome.codegen as cg
from esphome import pins
from esphome.components import binary_sensor, climate, switch, uart
import esphome.config_validation as cv
from esphome.const import (
    CONF_FLOW_CONTROL_PIN,
    DEVICE_CLASS_CONNECTIVITY,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from . import hisense_w4a1_ns

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["binary_sensor", "switch"]

CONF_DISPLAY = "display"
CONF_LINK = "link"

HisenseClimate = hisense_w4a1_ns.class_(
    "HisenseClimate", climate.Climate, cg.PollingComponent, uart.UARTDevice
)
HisenseDisplaySwitch = hisense_w4a1_ns.class_(
    "HisenseDisplaySwitch", switch.Switch, cg.Parented.template(HisenseClimate)
)

CONFIG_SCHEMA = (
    climate.climate_schema(HisenseClimate)
    .extend(
        {
            # Driven high while sending (RS-485 driver on), low while listening.
            cv.Optional(CONF_FLOW_CONTROL_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_DISPLAY): switch.switch_schema(
                HisenseDisplaySwitch,
                icon="mdi:led-on",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
            # On while the AC answers status requests.
            cv.Optional(CONF_LINK): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.polling_component_schema("5s"))
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "hisense_w4a1", baud_rate=9600, require_rx=True, require_tx=True
)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    if CONF_FLOW_CONTROL_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_FLOW_CONTROL_PIN])
        cg.add(var.set_flow_control_pin(pin))
    if CONF_DISPLAY in config:
        sw = await switch.new_switch(config[CONF_DISPLAY])
        await cg.register_parented(sw, var)
        cg.add(var.set_display_switch(sw))
    if CONF_LINK in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_LINK])
        cg.add(var.set_link_sensor(sens))
