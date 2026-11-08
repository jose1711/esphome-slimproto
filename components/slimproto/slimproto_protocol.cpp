#include "slimproto_protocol.h"

#include <cmath>

namespace esphome::slimproto {

static constexpr uint32_t PCM_SAMPLE_RATES[] = {11025, 22050, 32000, 44100, 48000,  8000,  12000,
                                                16000, 24000, 96000, 88200, 176400, 192000};
static constexpr size_t PCM_SAMPLE_RATE_COUNT = sizeof(PCM_SAMPLE_RATES) / sizeof(PCM_SAMPLE_RATES[0]);

// Lyrion maps the volume of device id 12 to dB with two straight lines that meet at volume 25 and -37 dB
// (Slim::Player::SqueezePlay::getVolumeParameters).
static constexpr float VOLUME_RANGE_DB = -74.0f;
static constexpr float VOLUME_STEP_DB = -37.0f;
static constexpr float VOLUME_STEP_POINT = 25.0f;

static uint16_t read_be16(const uint8_t *data) { return (static_cast<uint16_t>(data[0]) << 8) | data[1]; }

static uint32_t read_be32(const uint8_t *data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

static uint8_t *write_be16(uint8_t *data, uint16_t value) {
  *data++ = value >> 8;
  *data++ = value & 0xFF;
  return data;
}

static uint8_t *write_be32(uint8_t *data, uint32_t value) {
  *data++ = value >> 24;
  *data++ = (value >> 16) & 0xFF;
  *data++ = (value >> 8) & 0xFF;
  *data++ = value & 0xFF;
  return data;
}

static uint8_t *write_opcode(uint8_t *data, const char *opcode, uint32_t body_length) {
  std::memcpy(data, opcode, 4);
  return write_be32(data + 4, body_length);
}

bool parse_strm(const uint8_t *frame, size_t length, StrmPacket &packet) {
  if (length < STRM_PACKET_SIZE || !frame_is(frame, length, "strm")) {
    return false;
  }
  packet.command = static_cast<char>(frame[4]);
  packet.autostart = frame[5] >= '0' ? frame[5] - '0' : 0;
  packet.format = static_cast<char>(frame[6]);
  packet.pcm_sample_size = static_cast<char>(frame[7]);
  packet.pcm_sample_rate = static_cast<char>(frame[8]);
  packet.pcm_channels = static_cast<char>(frame[9]);
  packet.pcm_endianness = static_cast<char>(frame[10]);
  packet.threshold_kb = frame[11];
  packet.flags = frame[15];
  packet.replay_gain = read_be32(frame + 18);
  packet.server_port = read_be16(frame + 22);
  packet.server_ip = read_be32(frame + 24);
  packet.request = reinterpret_cast<const char *>(frame + STRM_PACKET_SIZE);
  packet.request_length = length - STRM_PACKET_SIZE;
  return true;
}

bool parse_audg(const uint8_t *frame, size_t length, uint32_t &gain) {
  if (length < 22 || !frame_is(frame, length, "audg")) {
    return false;
  }
  if (frame[12] == 0) {
    gain = GAIN_UNITY;
    return true;
  }
  const uint32_t left = read_be32(frame + 14);
  const uint32_t right = read_be32(frame + 18);
  gain = left > right ? left : right;
  return true;
}

bool parse_cont(const uint8_t *frame, size_t length, uint32_t &meta_interval) {
  if (length < 8 || !frame_is(frame, length, "cont")) {
    return false;
  }
  meta_interval = read_be32(frame + 4);
  return true;
}

bool parse_serv(const uint8_t *frame, size_t length, uint32_t &server_ip) {
  if (length < 8 || !frame_is(frame, length, "serv")) {
    return false;
  }
  server_ip = read_be32(frame + 4);
  return true;
}

PcmFormat decode_pcm_format(char sample_size, char sample_rate, char channels, char endianness) {
  PcmFormat format{};
  format.big_endian = endianness == '0';
  format.known = sample_size >= '0' && sample_size <= '3' && channels >= '1' && channels <= '2' && endianness != '?';
  if (format.known) {
    format.bits_per_sample = (sample_size - '0' + 1) * 8;
    format.channels = channels - '0';
    // The server continues the sequence with the characters after '9' for the high sample rates
    const int rate_index = sample_rate - '0';
    if (rate_index >= 0 && static_cast<size_t>(rate_index) < PCM_SAMPLE_RATE_COUNT) {
      format.sample_rate = PCM_SAMPLE_RATES[rate_index];
    } else {
      format.known = false;
    }
  }
  return format;
}

float gain_to_volume(uint32_t gain) {
  if (gain == 0) {
    return 0.0f;
  }
  if (gain >= GAIN_UNITY) {
    return 1.0f;
  }
  const float gain_db = 20.0f * std::log10(static_cast<float>(gain) / static_cast<float>(GAIN_UNITY));
  float position;
  if (gain_db >= VOLUME_STEP_DB) {
    position = 100.0f + gain_db * (100.0f - VOLUME_STEP_POINT) / -VOLUME_STEP_DB;
  } else {
    position = (gain_db - VOLUME_RANGE_DB) * VOLUME_STEP_POINT / (VOLUME_STEP_DB - VOLUME_RANGE_DB);
  }
  if (position < 0.0f) {
    position = 0.0f;
  }
  return std::round(position) / 100.0f;
}

size_t build_helo(uint8_t *buffer, size_t capacity, const uint8_t *mac, bool reconnect, uint64_t bytes_received,
                  const char *capabilities) {
  const size_t capabilities_length = std::strlen(capabilities);
  const size_t total = HELO_HEADER_SIZE + capabilities_length;
  if (total > capacity) {
    return 0;
  }
  std::memset(buffer, 0, HELO_HEADER_SIZE);
  uint8_t *out = write_opcode(buffer, "HELO", total - 8);
  *out++ = DEVICE_ID_SQUEEZEPLAY;
  *out++ = 0;  // Revision
  std::memcpy(out, mac, 6);
  out += 6 + 16;  // The UUID stays zero
  out = write_be16(out, reconnect ? 0x4000 : 0x0000);
  out = write_be32(out, static_cast<uint32_t>(bytes_received >> 32));
  out = write_be32(out, static_cast<uint32_t>(bytes_received));
  out += 2;  // The language stays zero
  std::memcpy(out, capabilities, capabilities_length);
  return total;
}

size_t build_stat(uint8_t *buffer, const StatFields &fields) {
  std::memset(buffer, 0, STAT_PACKET_SIZE);
  uint8_t *out = write_opcode(buffer, "STAT", STAT_PACKET_SIZE - 8);
  std::memcpy(out, fields.event, 4);
  out += 4 + 3;  // num_crlf, mas_initialized and mas_mode stay zero
  out = write_be32(out, fields.stream_buffer_size);
  out = write_be32(out, fields.stream_buffer_fullness);
  out = write_be32(out, static_cast<uint32_t>(fields.bytes_received >> 32));
  out = write_be32(out, static_cast<uint32_t>(fields.bytes_received));
  out = write_be16(out, 0xFFFF);  // Signal strength: wired
  out = write_be32(out, fields.jiffies);
  out = write_be32(out, fields.output_buffer_size);
  out = write_be32(out, fields.output_buffer_fullness);
  out = write_be32(out, fields.elapsed_seconds);
  out = write_be16(out, 0);  // Voltage
  out = write_be32(out, fields.elapsed_milliseconds);
  write_be32(out, fields.server_timestamp);
  return STAT_PACKET_SIZE;
}

size_t build_header(uint8_t *buffer, const char *opcode, uint32_t body_length) {
  write_opcode(buffer, opcode, body_length);
  return 8;
}

size_t build_dsco(uint8_t *buffer, DisconnectReason reason) {
  uint8_t *out = write_opcode(buffer, "DSCO", 1);
  *out = static_cast<uint8_t>(reason);
  return 9;
}

}  // namespace esphome::slimproto
