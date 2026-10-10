"""Read TWC vitals on a worker task so HTTP cannot block Modbus replies."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32
from esphome.const import CONF_ID, CONF_TIMEOUT, CONF_URL

DEPENDENCIES = ["esp32", "network"]
AUTO_LOAD = ["json"]
twc_ns = cg.esphome_ns.namespace("twc_vitals")
TWCVitals = twc_ns.class_("TWCVitals", cg.PollingComponent)

def local_http_url(value):
    value = cv.url(value)
    if not value.startswith("http://"):
        raise cv.Invalid("Use the charger's local http:// vitals endpoint")
    return value

CONFIG_SCHEMA = cv.All(
    cv.Schema({
        cv.GenerateID(): cv.declare_id(TWCVitals),
        cv.Required(CONF_URL): local_http_url,
        cv.Optional(CONF_TIMEOUT, default="2s"): cv.positive_time_period_milliseconds,
    }).extend(cv.polling_component_schema("2s")),
    cv.only_with_framework("esp-idf"),
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_url(config[CONF_URL]))
    cg.add(var.set_timeout(config[CONF_TIMEOUT].total_milliseconds))
    esp32.include_builtin_idf_component("esp_http_client")
