"""ESPHome external component for EVN DLMS smart meter (Kaifa MA309M via M-Bus).

Migrated from the legacy `custom_component` lambda layout to a real external
component. Add the following to your YAML to use it:

    external_components:
      - source:
          type: local
          path: external_components
        components: [espdm]

    espdm:
      id: dlms_meter1
      uart_id: mbus
      key: "DEADBEEFCAFEBABEDEADBEEFCAFEBABE"  # 32 hex chars; use !secret in real configs
      voltage: { l1: meter01_voltage_l1, l2: meter01_voltage_l2, l3: meter01_voltage_l3 }
      ...
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, text_sensor, uart
from esphome.const import CONF_ID, CONF_KEY

try:
    from esphome.components import mqtt as _mqtt

    _HAS_MQTT = True
except ImportError:  # pragma: no cover - mqtt always ships with ESPHome
    _HAS_MQTT = False

CODEOWNERS = ["@local"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "text_sensor"]
MULTI_CONF = False

espdm_ns = cg.esphome_ns.namespace("espdm")
DlmsMeter = espdm_ns.class_("DlmsMeter", cg.Component, uart.UARTDevice)

CONF_VOLTAGE = "voltage"
CONF_CURRENT = "current"
CONF_ACTIVE_POWER = "active_power"
CONF_ACTIVE_ENERGY = "active_energy"
CONF_REACTIVE_ENERGY = "reactive_energy"
CONF_TIMESTAMP = "timestamp"
CONF_METERNUMBER = "meternumber"
CONF_MQTT = "mqtt"

CONF_L1 = "l1"
CONF_L2 = "l2"
CONF_L3 = "l3"
CONF_PLUS = "plus"
CONF_MINUS = "minus"
CONF_POWER_FACTOR = "power_factor"

CONF_BROKER_ID = "mqtt_id"
CONF_TOPIC = "topic"


def _validate_key(value):
    value = cv.string_strict(value)
    cleaned = value.replace(":", "").replace(" ", "").replace("-", "")
    if len(cleaned) != 32:
        raise cv.Invalid(
            f"'key' must be a 16-byte (32 hex character) value, got {len(cleaned)} chars"
        )
    try:
        bytes.fromhex(cleaned)
    except ValueError as err:
        raise cv.Invalid(f"'key' must be hexadecimal: {err}") from err
    return cleaned.lower()


_PHASE_SENSORS = cv.Schema(
    {
        cv.Required(CONF_L1): cv.use_id(sensor.Sensor),
        cv.Required(CONF_L2): cv.use_id(sensor.Sensor),
        cv.Required(CONF_L3): cv.use_id(sensor.Sensor),
    }
)

_PLUS_MINUS = cv.Schema(
    {
        cv.Required(CONF_PLUS): cv.use_id(sensor.Sensor),
        cv.Required(CONF_MINUS): cv.use_id(sensor.Sensor),
    }
)

_ACTIVE_POWER = cv.Schema(
    {
        cv.Required(CONF_PLUS): cv.use_id(sensor.Sensor),
        cv.Required(CONF_MINUS): cv.use_id(sensor.Sensor),
        cv.Required(CONF_POWER_FACTOR): cv.use_id(sensor.Sensor),
    }
)

_MQTT_SCHEMA = None
if _HAS_MQTT:
    _MQTT_SCHEMA = cv.Schema(
        {
            cv.Required(CONF_BROKER_ID): cv.use_id(_mqtt.MQTTClientComponent),
            cv.Required(CONF_TOPIC): cv.string_strict,
        }
    )


_base_schema = {
    cv.GenerateID(): cv.declare_id(DlmsMeter),
    cv.Required(CONF_KEY): _validate_key,
    cv.Optional(CONF_VOLTAGE): _PHASE_SENSORS,
    cv.Optional(CONF_CURRENT): _PHASE_SENSORS,
    cv.Optional(CONF_ACTIVE_POWER): _ACTIVE_POWER,
    cv.Optional(CONF_ACTIVE_ENERGY): _PLUS_MINUS,
    cv.Optional(CONF_REACTIVE_ENERGY): _PLUS_MINUS,
    cv.Optional(CONF_TIMESTAMP): cv.use_id(text_sensor.TextSensor),
    cv.Optional(CONF_METERNUMBER): cv.use_id(text_sensor.TextSensor),
}
if _MQTT_SCHEMA is not None:
    _base_schema[cv.Optional(CONF_MQTT)] = _MQTT_SCHEMA

CONFIG_SCHEMA = (
    cv.Schema(_base_schema)
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def _maybe_set_phase(var, config, key, setter):
    if key not in config:
        return
    group = config[key]
    l1 = await cg.get_variable(group[CONF_L1])
    l2 = await cg.get_variable(group[CONF_L2])
    l3 = await cg.get_variable(group[CONF_L3])
    cg.add(getattr(var, setter)(l1, l2, l3))


async def _maybe_set_plus_minus(var, config, key, setter):
    if key not in config:
        return
    group = config[key]
    plus = await cg.get_variable(group[CONF_PLUS])
    minus = await cg.get_variable(group[CONF_MINUS])
    cg.add(getattr(var, setter)(plus, minus))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_key_hex(config[CONF_KEY]))

    await _maybe_set_phase(var, config, CONF_VOLTAGE, "set_voltage_sensors")
    await _maybe_set_phase(var, config, CONF_CURRENT, "set_current_sensors")

    if CONF_ACTIVE_POWER in config:
        ap = config[CONF_ACTIVE_POWER]
        plus = await cg.get_variable(ap[CONF_PLUS])
        minus = await cg.get_variable(ap[CONF_MINUS])
        pf = await cg.get_variable(ap[CONF_POWER_FACTOR])
        cg.add(var.set_active_power_sensors(plus, minus, pf))

    await _maybe_set_plus_minus(var, config, CONF_ACTIVE_ENERGY, "set_active_energy_sensors")
    await _maybe_set_plus_minus(var, config, CONF_REACTIVE_ENERGY, "set_reactive_energy_sensors")

    if CONF_TIMESTAMP in config:
        ts = await cg.get_variable(config[CONF_TIMESTAMP])
        cg.add(var.set_timestamp_sensor(ts))
    if CONF_METERNUMBER in config:
        mn = await cg.get_variable(config[CONF_METERNUMBER])
        cg.add(var.set_meternumber_sensor(mn))

    if CONF_MQTT in config:
        mqtt_conf = config[CONF_MQTT]
        broker = await cg.get_variable(mqtt_conf[CONF_BROKER_ID])
        cg.add(var.enable_mqtt(broker, mqtt_conf[CONF_TOPIC]))
