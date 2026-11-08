#pragma once

// Encoding and decoding of SlimProto packets. This file has no ESPHome or ESP-IDF dependency so that it can be
// tested on a desktop computer.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome::slimproto {

static constexpr uint16_t SLIMPROTO_PORT = 3483;
static constexpr uint8_t DEVICE_ID_SQUEEZEPLAY = 12;
static constexpr uint32_t GAIN_UNITY = 65536;

static constexpr size_t HELO_HEADER_SIZE = 44;
static constexpr size_t STAT_PACKET_SIZE = 61;
static constexpr size_t STRM_PACKET_SIZE = 28;

/// @brief Reason codes for the DSCO packet.
enum class DisconnectReason : uint8_t {
  DISCONNECT_REASON_OK = 0,
  DISCONNECT_REASON_LOCAL = 1,
  DISCONNECT_REASON_REMOTE = 2,
  DISCONNECT_REASON_UNREACHABLE = 3,
  DISCONNECT_REASON_TIMEOUT = 4,
};

/// @brief A decoded strm packet. request points into the frame passed to parse_strm().
struct StrmPacket {
  char command;
  uint8_t autostart;
  char format;
  char pcm_sample_size;
  char pcm_sample_rate;
  char pcm_channels;
  char pcm_endianness;
  uint8_t threshold_kb;
  uint8_t flags;
  uint32_t replay_gain;  // Holds the interval or timestamp for the p, a, u and t commands
  uint16_t server_port;
  uint32_t server_ip;  // Host byte order, 0 means the server of the control connection
  const char *request;
  size_t request_length;
};

/// @brief Values for the fields of a STAT packet.
struct StatFields {
  const char *event;  // Exactly four characters, for example "STMt"
  uint32_t stream_buffer_size;
  uint32_t stream_buffer_fullness;
  uint64_t bytes_received;
  uint32_t jiffies;
  uint32_t output_buffer_size;
  uint32_t output_buffer_fullness;
  uint32_t elapsed_seconds;
  uint32_t elapsed_milliseconds;
  uint32_t server_timestamp;
};

/// @brief The PCM format announced in a strm or codc packet.
struct PcmFormat {
  uint8_t bits_per_sample;
  uint8_t channels;
  uint32_t sample_rate;
  bool big_endian;
  bool known;  // False when any field was sent as '?', which means it must come from a file header
};

/// @brief Reassembles the length-prefixed frames sent by the server from a TCP byte stream.
/// Frames larger than the buffer are dropped without losing track of the following frames.
class FrameAssembler {
 public:
  void set_buffer(uint8_t *buffer, size_t capacity) {
    this->buffer_ = buffer;
    this->capacity_ = capacity;
  }

  void reset() { this->header_received_ = 0; }

  /// @param on_frame Called with (const uint8_t *frame, size_t length). The frame starts with the 4 byte opcode.
  template<typename F> void feed(const uint8_t *data, size_t length, F &&on_frame) {
    while (length > 0) {
      if (this->header_received_ < 2) {
        this->header_[this->header_received_++] = *data++;
        length--;
        if (this->header_received_ == 2) {
          this->frame_length_ = (static_cast<size_t>(this->header_[0]) << 8) | this->header_[1];
          this->frame_received_ = 0;
          this->oversized_ = this->frame_length_ > this->capacity_;
          if (this->frame_length_ == 0) {
            this->header_received_ = 0;
          }
        }
        continue;
      }
      size_t chunk = this->frame_length_ - this->frame_received_;
      if (chunk > length) {
        chunk = length;
      }
      if (!this->oversized_) {
        std::memcpy(this->buffer_ + this->frame_received_, data, chunk);
      }
      this->frame_received_ += chunk;
      data += chunk;
      length -= chunk;
      if (this->frame_received_ == this->frame_length_) {
        if (!this->oversized_) {
          on_frame(static_cast<const uint8_t *>(this->buffer_), this->frame_length_);
        }
        this->header_received_ = 0;
      }
    }
  }

 protected:
  uint8_t *buffer_{nullptr};
  size_t capacity_{0};
  size_t frame_length_{0};
  size_t frame_received_{0};
  uint8_t header_[2]{};
  uint8_t header_received_{0};
  bool oversized_{false};
};

/// @brief Tests if a frame has the given four character opcode.
inline bool frame_is(const uint8_t *frame, size_t length, const char *opcode) {
  return length >= 4 && std::memcmp(frame, opcode, 4) == 0;
}

bool parse_strm(const uint8_t *frame, size_t length, StrmPacket &packet);

/// @brief Extracts the volume from an audg frame.
/// @return True if the frame is valid. gain is GAIN_UNITY when the server asks for a fixed output level.
bool parse_audg(const uint8_t *frame, size_t length, uint32_t &gain);

/// @brief Extracts the metadata interval from a cont frame.
bool parse_cont(const uint8_t *frame, size_t length, uint32_t &meta_interval);

/// @brief Extracts the new server address from a serv frame. The address 0.0.0.1 means mysqueezebox.com.
bool parse_serv(const uint8_t *frame, size_t length, uint32_t &server_ip);

/// @return The PCM format for the ASCII codes used in strm and codc packets.
PcmFormat decode_pcm_format(char sample_size, char sample_rate, char channels, char endianness);

/// @brief Converts a 16.16 gain to the 0.0 to 1.0 volume that Lyrion shows, for a player with device id 12.
float gain_to_volume(uint32_t gain);

/// @brief Builds a HELO packet.
/// @return The packet length, or 0 if it does not fit in the buffer.
size_t build_helo(uint8_t *buffer, size_t capacity, const uint8_t *mac, bool reconnect, uint64_t bytes_received,
                  const char *capabilities);

/// @brief Builds a STAT packet. The buffer must hold STAT_PACKET_SIZE bytes.
size_t build_stat(uint8_t *buffer, const StatFields &fields);

/// @brief Builds the header of a packet that has only an opcode and a body: RESP, META or SETD.
/// The buffer must hold 8 bytes. The caller sends body_length bytes of body after the header.
size_t build_header(uint8_t *buffer, const char *opcode, uint32_t body_length);

/// @brief Builds a DSCO packet. The buffer must hold 9 bytes.
size_t build_dsco(uint8_t *buffer, DisconnectReason reason);

}  // namespace esphome::slimproto
