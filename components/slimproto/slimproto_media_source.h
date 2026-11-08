#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_decoder.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/media_source/media_source.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/core/component.h"

#include "slimproto_protocol.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>
#include <string>

namespace esphome::slimproto {

// THREADING MODEL:
// - Main loop: control connection to Lyrion, the command line connection, and all state changes.
// - Stream task: HTTP download, decoding and writing audio to the listener.
// - Speaker callback task: notify_audio_played() only updates an atomic counter.
//
// The main loop sends work to the stream task with a FreeRTOS queue. The stream task reports back by setting bits in
// events_, which loop() reads and clears.

/// @brief A Squeezebox player that receives its commands and audio from a Lyrion Music Server.
class SlimprotoMediaSource final : public Component, public media_source::MediaSource, public audio::AudioSinkCallback {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_server(const std::string &server) { this->configured_server_ = server; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_cli_port(uint16_t port) { this->cli_port_ = port; }
  void set_buffer_size(size_t buffer_size) { this->buffer_size_ = buffer_size; }
  void set_max_sample_rate(uint32_t max_sample_rate) { this->max_sample_rate_ = max_sample_rate; }
  void set_player_name(const std::string &name) { this->player_name_ = name; }
  void set_task_stack_in_psram(bool in_psram) { this->task_stack_in_psram_ = in_psram; }

  // MediaSource interface
  bool play_uri(const std::string &uri) override;
  void handle_command(media_source::MediaSourceCommand command) override;
  bool can_handle(const std::string &uri) const override;
  bool has_internal_playlist() const override { return true; }
  void notify_volume_changed(float volume) override;
  void notify_mute_changed(bool is_muted) override;
  void notify_audio_played(uint32_t frames, int64_t timestamp) override;

  // AudioSinkCallback interface, called from the stream task
  size_t audio_sink_write(uint8_t *data, size_t length, TickType_t ticks_to_wait) override;

 protected:
  struct StreamCommand {
    enum Type : uint8_t { STREAM_COMMAND_START, STREAM_COMMAND_FLUSH, STREAM_COMMAND_CONT, STREAM_COMMAND_CODEC };
    Type type;
    uint8_t autostart;
    uint8_t threshold_kb;
    char format;
    char pcm_sample_size;
    char pcm_sample_rate;
    char pcm_channels;
    char pcm_endianness;
    bool hard_flush;  // True for the q command, which also ends what is in the output
    uint16_t port;
    uint16_t request_length;
    uint32_t ip;
    uint32_t meta_interval;
    char *request;  // Owned by the receiver of this command
  };

  enum class ControlState : uint8_t {
    CONTROL_STATE_WAITING,
    CONTROL_STATE_DISCOVERING,
    CONTROL_STATE_CONNECTING,
    CONTROL_STATE_CONNECTED,
  };

  enum class HttpPhase : uint8_t {
    HTTP_PHASE_NONE,
    HTTP_PHASE_CONNECTING,
    HTTP_PHASE_SENDING,
    HTTP_PHASE_HEADERS,
    HTTP_PHASE_WAIT_CONT,
    HTTP_PHASE_BODY,
  };

  enum class CliState : uint8_t {
    CLI_STATE_IDLE,
    CLI_STATE_CONNECTING,
  };

  // Main loop: control connection
  void service_control_();
  void start_discovery_();
  void service_discovery_();
  void start_connect_(uint32_t ip);
  void close_control_(bool retry);
  void receive_control_();
  void handle_frame_(const uint8_t *frame, size_t length);
  void handle_strm_(const StrmPacket &packet);
  void handle_start_(const StrmPacket &packet);
  void handle_codc_(const uint8_t *frame, size_t length);
  void handle_setd_(const uint8_t *frame, size_t length);
  void handle_audg_(uint32_t gain);
  void send_helo_(bool reconnect);
  void send_stat_(const char *event, uint32_t server_timestamp = 0);
  void send_name_();
  bool send_control_(const uint8_t *data, size_t length);
  void send_packet_(const char *opcode, const void *body, size_t body_length);
  void stop_locally_();
  void process_events_();
  void update_playback_();

  // Main loop: command line connection, used to pass player controls to the server
  void queue_cli_command_(const char *command);
  void service_cli_();
  void close_cli_();

  // Stream task
  static void stream_task_(void *params);
  void run_stream_task_();
  void handle_stream_command_(const StreamCommand &command);
  void begin_stream_(const StreamCommand &command);
  void close_stream_socket_();
  void reset_stream_();
  void finish_http_(DisconnectReason reason);
  bool service_http_();
  bool receive_headers_();
  bool receive_body_();
  bool receive_metadata_();
  void check_buffered_();
  bool start_decoding_();
  bool service_decoder_();
  bool service_pcm_();
  void set_event_(uint32_t event) { this->events_.fetch_or(event, std::memory_order_acq_rel); }

  // Configuration
  std::string configured_server_;
  std::string player_name_;
  size_t buffer_size_{256000};
  uint32_t max_sample_rate_{48000};
  uint16_t port_{SLIMPROTO_PORT};
  uint16_t cli_port_{9090};
  bool task_stack_in_psram_{false};

  // Shared buffers, allocated once in setup()
  std::shared_ptr<ring_buffer::RingBuffer> ring_buffer_;
  uint8_t *memory_{nullptr};
  uint8_t *control_frame_{nullptr};
  uint8_t *receive_buffer_{nullptr};
  uint8_t *pcm_buffer_{nullptr};
  char *http_headers_{nullptr};
  char *metadata_{nullptr};
  char capabilities_[128]{};

  TaskHandle_t task_handle_{nullptr};
  QueueHandle_t stream_commands_{nullptr};

  // Stream task to main loop
  std::atomic<uint32_t> events_{0};
  std::atomic<uint32_t> bytes_received_{0};
  std::atomic<uint32_t> next_track_rate_{0};
  std::atomic<uint32_t> next_track_start_{0};
  std::atomic<bool> next_track_pending_{false};
  std::atomic<uint8_t> disconnect_reason_{0};
  std::atomic<uint16_t> http_headers_length_{0};
  std::atomic<uint16_t> metadata_length_{0};
  std::atomic<bool> metadata_ready_{false};

  // Output control, written by both tasks and read by the stream task
  std::atomic<bool> output_open_{false};
  std::atomic<uint32_t> start_at_{0};
  std::atomic<uint32_t> pause_until_{0};
  std::atomic<uint32_t> skip_ms_{0};

  // Frame counters; only their differences are used, so wrapping around is harmless
  std::atomic<uint32_t> frames_written_{0};
  std::atomic<uint32_t> frames_played_{0};

  // Main loop state
  FrameAssembler frames_;
  ControlState control_state_{ControlState::CONTROL_STATE_WAITING};
  int control_socket_{-1};
  int discovery_socket_{-1};
  uint32_t server_ip_{0};
  uint32_t state_entered_{0};
  uint32_t last_received_{0};
  uint32_t last_update_{0};
  uint32_t last_status_{0};
  uint32_t last_discovery_{0};
  uint32_t current_track_start_{0};
  uint32_t current_track_rate_{0};
  uint32_t drain_deadline_{0};
  float lms_volume_{-1.0f};
  bool lms_muted_{false};
  bool connected_before_{false};
  bool stream_requested_{false};
  bool pending_start_{false};
  bool awaiting_drain_{false};
  bool track_playing_{false};
  uint8_t mac_[6]{};

  // Command line connection
  CliState cli_state_{CliState::CLI_STATE_IDLE};
  int cli_socket_{-1};
  uint32_t cli_started_{0};
  char cli_command_[96]{};
  bool cli_pending_{false};

  // Stream task state
  std::unique_ptr<audio::AudioDecoder> decoder_;
  audio::AudioStreamInfo stream_info_;
  HttpPhase http_phase_{HttpPhase::HTTP_PHASE_NONE};
  int stream_socket_{-1};
  uint32_t http_started_{0};
  char *request_{nullptr};
  size_t request_length_{0};
  size_t request_sent_{0};
  size_t threshold_bytes_{0};
  size_t pcm_length_{0};
  size_t headers_received_{0};
  uint32_t skip_frames_{0};
  uint32_t meta_interval_{0};
  uint32_t meta_next_{0};
  size_t meta_left_{0};
  size_t meta_received_{0};
  uint8_t autostart_{0};
  char format_{'?'};
  char pcm_sample_size_{'?'};
  char pcm_sample_rate_{'?'};
  char pcm_channels_{'?'};
  char pcm_endianness_{'?'};
  PcmFormat pcm_format_{};
  bool passthrough_pcm_{false};
  bool decoding_{false};
  bool buffered_{false};
  bool stream_ended_{false};
  bool first_write_{false};
  bool meta_in_block_{false};
  uint8_t header_match_{0};
};

}  // namespace esphome::slimproto

#endif  // USE_ESP32
