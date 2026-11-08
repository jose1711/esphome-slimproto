import esphome.codegen as cg
from esphome.components import audio, media_source, psram
import esphome.config_validation as cv
from esphome.const import CONF_BUFFER_SIZE, CONF_ID, CONF_PORT, CONF_TASK_STACK_IN_PSRAM
from esphome.core import CORE
from esphome.types import ConfigType

AUTO_LOAD = ["audio", "ring_buffer"]
DEPENDENCIES = ["network"]

CONF_CLI_PORT = "cli_port"
CONF_MAX_SAMPLE_RATE = "max_sample_rate"
CONF_PLAYER_NAME = "player_name"
CONF_SERVER = "server"

slimproto_ns = cg.esphome_ns.namespace("slimproto")
SlimprotoMediaSource = slimproto_ns.class_(
    "SlimprotoMediaSource", cg.Component, media_source.MediaSource
)


def _request_codecs(config: ConfigType) -> ConfigType:
    audio.request_flac_support()
    audio.request_mp3_support()
    audio.request_wav_support()
    return config


CONFIG_SCHEMA = cv.All(
    media_source.media_source_schema(SlimprotoMediaSource)
    .extend(
        {
            cv.Optional(CONF_SERVER): cv.ipv4address,
            cv.Optional(CONF_PORT, default=3483): cv.port,
            cv.Optional(CONF_CLI_PORT, default=9090): cv.port,
            cv.Optional(CONF_PLAYER_NAME): cv.All(cv.string, cv.Length(min=1, max=32)),
            cv.Optional(CONF_BUFFER_SIZE, default=256000): cv.int_range(
                min=32000, max=4000000
            ),
            cv.Optional(CONF_MAX_SAMPLE_RATE, default=48000): cv.int_range(
                min=8000, max=192000
            ),
            cv.Optional(CONF_TASK_STACK_IN_PSRAM): psram.validate_task_stack_in_psram,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _request_codecs,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_source.register_media_source(var, config)

    if (server := config.get(CONF_SERVER)) is not None:
        cg.add(var.set_server(str(server)))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_cli_port(config[CONF_CLI_PORT]))
    cg.add(var.set_buffer_size(config[CONF_BUFFER_SIZE]))
    cg.add(var.set_max_sample_rate(config[CONF_MAX_SAMPLE_RATE]))
    cg.add(
        var.set_player_name(
            config.get(CONF_PLAYER_NAME, CORE.friendly_name or CORE.name)
        )
    )

    if config.get(CONF_TASK_STACK_IN_PSRAM):
        cg.add(var.set_task_stack_in_psram(True))
        psram.request_external_task_stack()
