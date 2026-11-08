# slimproto for ESPHome

An external component that turns an ESP32 into a Squeezebox player for Lyrion Music Server (LMS).
It plugs into the `speaker_source` media player as a `media_source`, so I2S, the mixer and the
resampler stay standard ESPHome components.

Status: **experimental.** Successfully tested on a Home Assistant Voice PE (ESP32-S3) with a real LMS. Other boards have not been tested.

## Installation

1. Copy the `components/slimproto` folder into your ESPHome configuration, next to your YAML file
   (in Home Assistant that is `/config/esphome/components/slimproto`).
2. Add the parts from `example.yaml` to your YAML and adjust the pins, PSRAM and chip type for your board.
3. Compile and flash. The player shows up in LMS under the device name.

Instead of copying, the component can be loaded straight from GitHub if the repository is public:

```yaml
external_components:
  - source: github://jose1711/esphome-slimproto
    components: [slimproto]
```

Requires an ESP32 with PSRAM (ESP32-S3 recommended) and ESPHome 2026.10 or newer.

## Options of `media_source: - platform: slimproto`

| Option | Default | Description |
|---|---|---|
| `server` | automatic | IP address of the LMS; without it the server is found with a UDP broadcast |
| `port` | 3483 | SlimProto port |
| `cli_port` | 9090 | CLI port of the LMS, used for commands coming from Home Assistant |
| `player_name` | device name | Name of the player in LMS |
| `buffer_size` | 256000 | Size of the stream buffer in bytes |
| `max_sample_rate` | 48000 | Highest sample rate reported to the server |
| `task_stack_in_psram` | false | Put the task stack in PSRAM |

## What is implemented

- FLAC, MP3 and PCM/WAV; other formats are transcoded by LMS if it is set up to do so
- start, stop, pause, seek, next track, gapless transitions between tracks
- volume and mute in both directions, internet radio with metadata
- control from Home Assistant through the CLI port of the LMS

## Limitations

- When jumping to another track or pausing, the content of the output buffer is still played out (`buffer_duration` of the speaker).
- Synchronization of multiple players is only basic and its accuracy still needs to be verified.
- No HTTPS, crossfade or ReplayGain.
- The CLI port must not be protected by a password.
- MP3 tracks may have a small gap at the boundary (no LAME gapless trimming).
- The player name comes from the YAML; renaming it in LMS does not survive a reboot.
