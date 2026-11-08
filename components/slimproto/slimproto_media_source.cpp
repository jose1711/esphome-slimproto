#include "slimproto_media_source.h"

#ifdef USE_ESP32

#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace esphome::slimproto {

static const char *const TAG = "slimproto";

static constexpr const char *const URI_CURRENT = "slimproto://current";
static constexpr const char *const URI_PREFIX = "slimproto://";
static constexpr const char *const FIRMWARE_VERSION = "1";

static constexpr uint32_t RETRY_INTERVAL_MS = 5000;
static constexpr uint32_t DISCOVERY_INTERVAL_MS = 5000;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 5000;
static constexpr uint32_t CLI_TIMEOUT_MS = 2000;
static constexpr uint32_t SERVER_TIMEOUT_MS = 35000;
static constexpr uint32_t UPDATE_INTERVAL_MS = 100;
static constexpr uint32_t STATUS_INTERVAL_MS = 1000;
static constexpr uint32_t HTTP_CONNECT_TIMEOUT_MS = 5000;
static constexpr uint32_t HTTP_HEADER_TIMEOUT_MS = 15000;
static constexpr uint32_t DRAIN_GRACE_MS = 2000;
static constexpr uint32_t DEFAULT_SAMPLE_RATE = 44100;
static constexpr uint32_t VOLUME_HOLD_MS = 1500;
static constexpr float VOLUME_TOLERANCE = 0.015f;

static constexpr size_t CONTROL_FRAME_SIZE = 2048;
static constexpr size_t RECEIVE_BUFFER_SIZE = 2048;
static constexpr uint8_t MAX_RECEIVE_PASSES = 16;
static constexpr size_t PCM_BUFFER_SIZE = 4096;
static constexpr size_t HTTP_HEADERS_SIZE = 1536;
static constexpr size_t METADATA_SIZE = 512;
static constexpr size_t MAX_REQUEST_LENGTH = 2000;
static constexpr size_t DECODER_INPUT_SIZE = 4096;
static constexpr size_t DECODER_OUTPUT_SIZE = 8192;

static constexpr uint32_t TASK_STACK_SIZE = 8192;
static constexpr UBaseType_t TASK_PRIORITY = 2;
static constexpr UBaseType_t QUEUE_LENGTH = 8;
static constexpr uint32_t PCM_WRITE_TIMEOUT_MS = 20;

enum StreamEvent : uint32_t {
  STREAM_EVENT_FLUSHED = 1 << 0,
  STREAM_EVENT_DECODE_DONE = 1 << 1,
  STREAM_EVENT_DECODE_ERROR = 1 << 2,
  STREAM_EVENT_DISCONNECTED = 1 << 3,
  STREAM_EVENT_HEADERS = 1 << 4,
  STREAM_EVENT_LOADED = 1 << 5,
};

static bool set_non_blocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

/// @return A non-blocking socket with a connection attempt in progress, or -1 on failure.
static int open_connection(uint32_t ip, uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    return -1;
  }
  const int enable = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
  if (!set_non_blocking(fd)) {
    ::close(fd);
    return -1;
  }
  struct sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(ip);
  if (::connect(fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) < 0 && errno != EINPROGRESS) {
    ::close(fd);
    return -1;
  }
  return fd;
}

/// @return 1 if the connection is established, 0 if it is still in progress, -1 if it failed.
static int check_connection(int fd) {
  fd_set write_set;
  FD_ZERO(&write_set);
  FD_SET(fd, &write_set);
  struct timeval timeout {};
  const int ready = ::select(fd + 1, nullptr, &write_set, nullptr, &timeout);
  if (ready == 0) {
    return 0;
  }
  if (ready < 0) {
    return -1;
  }
  int error = 0;
  socklen_t length = sizeof(error);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error != 0) {
    return -1;
  }
  return 1;
}

static bool would_block() { return errno == EWOULDBLOCK || errno == EAGAIN; }

static bool time_reached(uint32_t now, uint32_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }

void SlimprotoMediaSource::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Slimproto Media Source:\n"
                "  Server: %s\n"
                "  Port: %u\n"
                "  CLI Port: %u\n"
                "  Player Name: %s\n"
                "  Buffer Size: %zu bytes\n"
                "  Max Sample Rate: %" PRIu32 "\n"
                "  Capabilities: %s",
                this->configured_server_.empty() ? "autodiscovery" : this->configured_server_.c_str(), this->port_,
                this->cli_port_, this->player_name_.c_str(), this->buffer_size_, this->max_sample_rate_,
                this->capabilities_);
}

void SlimprotoMediaSource::setup() {
  get_mac_address_raw(this->mac_);

  snprintf(this->capabilities_, sizeof(this->capabilities_),
           "Model=squeezelite,ModelName=ESPHome,Firmware=%s,MaxSampleRate=%" PRIu32 ",flc,mp3,pcm", FIRMWARE_VERSION,
           this->max_sample_rate_);

  auto ring_buffer = ring_buffer::RingBuffer::create(this->buffer_size_);
  if (ring_buffer == nullptr) {
    this->mark_failed(LOG_STR("Failed to allocate the stream buffer"));
    return;
  }
  this->ring_buffer_ = std::move(ring_buffer);

  RAMAllocator<uint8_t> allocator;
  this->memory_ = allocator.allocate(CONTROL_FRAME_SIZE + RECEIVE_BUFFER_SIZE + PCM_BUFFER_SIZE + HTTP_HEADERS_SIZE +
                                     METADATA_SIZE);
  if (this->memory_ == nullptr) {
    this->mark_failed(LOG_STR("Failed to allocate memory"));
    return;
  }
  uint8_t *next = this->memory_;
  this->control_frame_ = next;
  next += CONTROL_FRAME_SIZE;
  this->receive_buffer_ = next;
  next += RECEIVE_BUFFER_SIZE;
  this->pcm_buffer_ = next;
  next += PCM_BUFFER_SIZE;
  this->http_headers_ = reinterpret_cast<char *>(next);
  next += HTTP_HEADERS_SIZE;
  this->metadata_ = reinterpret_cast<char *>(next);
  this->frames_.set_buffer(this->control_frame_, CONTROL_FRAME_SIZE);

  this->stream_commands_ = xQueueCreate(QUEUE_LENGTH, sizeof(StreamCommand));
  if (this->stream_commands_ == nullptr) {
    this->mark_failed(LOG_STR("Failed to create the command queue"));
    return;
  }

  if (this->task_stack_in_psram_) {
    RAMAllocator<StackType_t> stack_allocator(RAMAllocator<StackType_t>::ALLOC_EXTERNAL);
    RAMAllocator<StaticTask_t> task_allocator(RAMAllocator<StaticTask_t>::ALLOC_INTERNAL);
    StackType_t *stack = stack_allocator.allocate(TASK_STACK_SIZE);
    StaticTask_t *task = task_allocator.allocate(1);
    if (stack != nullptr && task != nullptr) {
      this->task_handle_ = xTaskCreateStatic(SlimprotoMediaSource::stream_task_, "slimproto", TASK_STACK_SIZE, this,
                                             TASK_PRIORITY, stack, task);
    }
  } else {
    xTaskCreate(SlimprotoMediaSource::stream_task_, "slimproto", TASK_STACK_SIZE, this, TASK_PRIORITY,
                &this->task_handle_);
  }
  if (this->task_handle_ == nullptr) {
    this->mark_failed(LOG_STR("Failed to create the stream task"));
    return;
  }
  this->state_entered_ = millis() - RETRY_INTERVAL_MS;
}

void SlimprotoMediaSource::loop() {
  const uint32_t now = App.get_loop_component_start_time();
  this->service_control_();
  this->process_events_();
  this->service_cli_();
  if (now - this->last_update_ >= UPDATE_INTERVAL_MS) {
    this->last_update_ = now;
    this->update_playback_();
  }
}

// --- Control connection ---

void SlimprotoMediaSource::service_control_() {
  const uint32_t now = App.get_loop_component_start_time();
  switch (this->control_state_) {
    case ControlState::CONTROL_STATE_WAITING: {
      if (!network::is_connected() || now - this->state_entered_ < RETRY_INTERVAL_MS) {
        return;
      }
      if (this->server_ip_ != 0) {
        this->start_connect_(this->server_ip_);
      } else if (!this->configured_server_.empty()) {
        this->start_connect_(ntohl(inet_addr(this->configured_server_.c_str())));
      } else {
        this->start_discovery_();
      }
      break;
    }
    case ControlState::CONTROL_STATE_DISCOVERING:
      this->service_discovery_();
      break;
    case ControlState::CONTROL_STATE_CONNECTING: {
      const int result = check_connection(this->control_socket_);
      if (result > 0) {
        ESP_LOGI(TAG, "Connected to the server");
        this->control_state_ = ControlState::CONTROL_STATE_CONNECTED;
        this->last_received_ = now;
        this->frames_.reset();
        this->status_clear_warning();
        this->send_helo_(this->connected_before_);
        this->send_name_();
        this->connected_before_ = true;
      } else if (result < 0 || now - this->state_entered_ >= CONNECT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Could not connect to the server");
        this->close_control_(true);
      }
      break;
    }
    case ControlState::CONTROL_STATE_CONNECTED:
      this->receive_control_();
      if (this->control_state_ == ControlState::CONTROL_STATE_CONNECTED &&
          now - this->last_received_ >= SERVER_TIMEOUT_MS) {
        ESP_LOGW(TAG, "No messages from the server, reconnecting");
        this->close_control_(true);
      }
      break;
  }
}

void SlimprotoMediaSource::start_discovery_() {
  if (this->discovery_socket_ < 0) {
    this->discovery_socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (this->discovery_socket_ < 0) {
      this->state_entered_ = App.get_loop_component_start_time();
      return;
    }
    const int enable = 1;
    ::setsockopt(this->discovery_socket_, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));
    set_non_blocking(this->discovery_socket_);
  }
  this->control_state_ = ControlState::CONTROL_STATE_DISCOVERING;
  this->last_discovery_ = App.get_loop_component_start_time() - DISCOVERY_INTERVAL_MS;
}

void SlimprotoMediaSource::service_discovery_() {
  const uint32_t now = App.get_loop_component_start_time();
  if (now - this->last_discovery_ >= DISCOVERY_INTERVAL_MS) {
    this->last_discovery_ = now;
    struct sockaddr_in destination {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(this->port_);
    destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    ::sendto(this->discovery_socket_, "e", 1, 0, reinterpret_cast<struct sockaddr *>(&destination),
             sizeof(destination));
  }

  char reply[32];
  struct sockaddr_in source {};
  socklen_t source_length = sizeof(source);
  const ssize_t received = ::recvfrom(this->discovery_socket_, reply, sizeof(reply), 0,
                                      reinterpret_cast<struct sockaddr *>(&source), &source_length);
  if (received > 0 && reply[0] == 'E') {
    ::close(this->discovery_socket_);
    this->discovery_socket_ = -1;
    this->server_ip_ = ntohl(source.sin_addr.s_addr);
    ESP_LOGI(TAG, "Found a server at %s", inet_ntoa(source.sin_addr));
    this->start_connect_(this->server_ip_);
  }
}

void SlimprotoMediaSource::start_connect_(uint32_t ip) {
  this->server_ip_ = ip;
  this->state_entered_ = App.get_loop_component_start_time();
  this->control_socket_ = open_connection(ip, this->port_);
  if (this->control_socket_ < 0) {
    this->status_set_warning();
    this->control_state_ = ControlState::CONTROL_STATE_WAITING;
    return;
  }
  this->control_state_ = ControlState::CONTROL_STATE_CONNECTING;
}

void SlimprotoMediaSource::close_control_(bool retry) {
  if (this->control_socket_ >= 0) {
    ::close(this->control_socket_);
    this->control_socket_ = -1;
  }
  this->control_state_ = ControlState::CONTROL_STATE_WAITING;
  this->state_entered_ = App.get_loop_component_start_time();
  if (!retry) {
    // Connect to the new server straight away
    this->state_entered_ -= RETRY_INTERVAL_MS;
  } else {
    this->status_set_warning();
    if (this->configured_server_.empty()) {
      // The server may have moved, so look for it again
      this->server_ip_ = 0;
    }
  }
}

void SlimprotoMediaSource::receive_control_() {
  uint8_t buffer[512];
  while (true) {
    const ssize_t received = ::recv(this->control_socket_, buffer, sizeof(buffer), 0);
    if (received > 0) {
      this->last_received_ = App.get_loop_component_start_time();
      this->frames_.feed(buffer, received,
                         [this](const uint8_t *frame, size_t length) { this->handle_frame_(frame, length); });
      if (this->control_state_ != ControlState::CONTROL_STATE_CONNECTED) {
        return;
      }
      continue;
    }
    if (received == 0 || !would_block()) {
      ESP_LOGW(TAG, "Connection to the server was lost");
      this->close_control_(true);
    }
    return;
  }
}

void SlimprotoMediaSource::handle_frame_(const uint8_t *frame, size_t length) {
  if (frame_is(frame, length, "strm")) {
    StrmPacket packet;
    if (parse_strm(frame, length, packet)) {
      this->handle_strm_(packet);
    }
  } else if (frame_is(frame, length, "audg")) {
    uint32_t gain;
    if (parse_audg(frame, length, gain)) {
      this->handle_audg_(gain);
    }
  } else if (frame_is(frame, length, "cont")) {
    StreamCommand command{};
    command.type = StreamCommand::STREAM_COMMAND_CONT;
    if (parse_cont(frame, length, command.meta_interval)) {
      xQueueSend(this->stream_commands_, &command, 0);
    }
  } else if (frame_is(frame, length, "codc")) {
    this->handle_codc_(frame, length);
  } else if (frame_is(frame, length, "setd")) {
    this->handle_setd_(frame, length);
  } else if (frame_is(frame, length, "serv")) {
    uint32_t ip;
    // 0.0.0.1 asks the player to move to mysqueezebox.com, which no longer exists
    if (parse_serv(frame, length, ip) && ip != 0 && ip != 1) {
      ESP_LOGI(TAG, "The server asked to switch servers");
      this->close_control_(false);
      this->server_ip_ = ip;
    }
  }
}

void SlimprotoMediaSource::stop_locally_() {
  this->stream_requested_ = false;
  this->awaiting_drain_ = false;
  this->track_playing_ = false;
  this->output_open_.store(false);
  this->start_at_.store(0);
  this->pause_until_.store(0);
  this->skip_ms_.store(0);
  if (this->get_state() != media_source::MediaSourceState::IDLE) {
    this->set_state_(media_source::MediaSourceState::IDLE);
  }

  StreamCommand command{};
  command.type = StreamCommand::STREAM_COMMAND_FLUSH;
  command.hard_flush = true;
  xQueueSend(this->stream_commands_, &command, pdMS_TO_TICKS(10));
}

void SlimprotoMediaSource::handle_strm_(const StrmPacket &packet) {
  switch (packet.command) {
    case 't':
      this->send_stat_("STMt", packet.replay_gain);
      break;
    case 'q':
      this->stop_locally_();
      break;
    case 'f': {
      this->stream_requested_ = false;
      StreamCommand command{};
      command.type = StreamCommand::STREAM_COMMAND_FLUSH;
      xQueueSend(this->stream_commands_, &command, pdMS_TO_TICKS(10));
      break;
    }
    case 'p':
      if (packet.replay_gain != 0) {
        this->pause_until_.store(millis() + packet.replay_gain);
      } else {
        this->output_open_.store(false);
        if (this->get_state() == media_source::MediaSourceState::PLAYING) {
          this->set_state_(media_source::MediaSourceState::PAUSED);
        }
        this->send_stat_("STMp");
      }
      break;
    case 'a':
      this->skip_ms_.fetch_add(packet.replay_gain);
      break;
    case 'u':
      this->start_at_.store(packet.replay_gain);
      this->output_open_.store(true);
      if (this->get_state() == media_source::MediaSourceState::PAUSED) {
        this->set_state_(media_source::MediaSourceState::PLAYING);
      }
      this->send_stat_("STMr");
      break;
    case 's':
      this->handle_start_(packet);
      break;
    default:
      ESP_LOGD(TAG, "Ignoring strm command '%c'", packet.command);
      break;
  }
}

void SlimprotoMediaSource::handle_start_(const StrmPacket &packet) {
  this->send_stat_("STMf");

  const bool supported = packet.format == 'f' || packet.format == 'm' || packet.format == 'p' ||
                         (packet.format == '?' && packet.autostart >= 2);
  if (!supported || packet.request_length == 0 || packet.request_length > MAX_REQUEST_LENGTH) {
    ESP_LOGW(TAG, "Cannot play format '%c' with a request of %zu bytes", packet.format, packet.request_length);
    this->send_stat_("STMn");
    return;
  }

  RAMAllocator<char> allocator;
  char *request = allocator.allocate(packet.request_length);
  if (request == nullptr) {
    this->send_stat_("STMn");
    return;
  }
  std::memcpy(request, packet.request, packet.request_length);

  StreamCommand command{};
  command.type = StreamCommand::STREAM_COMMAND_START;
  command.autostart = packet.autostart;
  command.threshold_kb = packet.threshold_kb;
  command.format = packet.format;
  command.pcm_sample_size = packet.pcm_sample_size;
  command.pcm_sample_rate = packet.pcm_sample_rate;
  command.pcm_channels = packet.pcm_channels;
  command.pcm_endianness = packet.pcm_endianness;
  command.port = packet.server_port;
  command.ip = packet.server_ip != 0 ? packet.server_ip : this->server_ip_;
  command.request = request;
  command.request_length = packet.request_length;
  if (xQueueSend(this->stream_commands_, &command, pdMS_TO_TICKS(10)) != pdTRUE) {
    allocator.deallocate(request, packet.request_length);
    this->send_stat_("STMn");
    return;
  }

  this->stream_requested_ = true;
  this->awaiting_drain_ = false;
  this->send_stat_("STMc");

  if (this->get_state() == media_source::MediaSourceState::IDLE && !this->pending_start_) {
    this->pending_start_ = true;
    this->request_play_uri_(URI_CURRENT);
  }
}

void SlimprotoMediaSource::handle_codc_(const uint8_t *frame, size_t length) {
  if (length < 9) {
    return;
  }
  StreamCommand command{};
  command.type = StreamCommand::STREAM_COMMAND_CODEC;
  command.format = static_cast<char>(frame[4]);
  command.pcm_sample_size = static_cast<char>(frame[5]);
  command.pcm_sample_rate = static_cast<char>(frame[6]);
  command.pcm_channels = static_cast<char>(frame[7]);
  command.pcm_endianness = static_cast<char>(frame[8]);
  xQueueSend(this->stream_commands_, &command, 0);
}

void SlimprotoMediaSource::handle_setd_(const uint8_t *frame, size_t length) {
  // Only setting id 0, the player name, is used. The name always comes from the configuration.
  if (length >= 5 && frame[4] == 0) {
    this->send_name_();
  }
}

void SlimprotoMediaSource::handle_audg_(uint32_t gain) {
  bool confirms_local_change = false;
  if (this->volume_hold_active_) {
    // After a local change the server may still answer earlier changes. Applying those would move the volume back.
    confirms_local_change = std::fabs(gain_to_volume(gain) - this->volume_hold_target_) < VOLUME_TOLERANCE;
    if (!confirms_local_change && !time_reached(App.get_loop_component_start_time(), this->volume_hold_until_)) {
      return;
    }
    this->volume_hold_active_ = false;
  }

  if (gain == 0) {
    if (!this->lms_muted_) {
      this->lms_muted_ = true;
      this->request_mute_(true);
    }
    return;
  }
  if (this->lms_muted_) {
    this->lms_muted_ = false;
    this->request_mute_(false);
  }
  if (confirms_local_change) {
    // The server rounds the volume, so keep the exact local value
    return;
  }
  const float volume = gain_to_volume(gain);
  this->lms_volume_ = volume;
  this->request_volume_(volume);
}

void SlimprotoMediaSource::send_helo_(bool reconnect) {
  uint8_t packet[HELO_HEADER_SIZE + sizeof(this->capabilities_)];
  const size_t length = build_helo(packet, sizeof(packet), this->mac_, reconnect,
                                   this->bytes_received_.load(std::memory_order_relaxed), this->capabilities_);
  if (length > 0) {
    this->send_control_(packet, length);
  }
}

void SlimprotoMediaSource::send_stat_(const char *event, uint32_t server_timestamp) {
  if (this->control_state_ != ControlState::CONTROL_STATE_CONNECTED) {
    return;
  }
  const uint32_t played = this->frames_played_.load(std::memory_order_relaxed);
  const uint32_t written = this->frames_written_.load(std::memory_order_relaxed);
  const int32_t queued = static_cast<int32_t>(written - played);

  uint32_t elapsed_ms = 0;
  if (this->track_playing_ && this->current_track_rate_ != 0) {
    elapsed_ms = static_cast<uint64_t>(played - this->current_track_start_) * 1000 / this->current_track_rate_;
  }

  StatFields fields{};
  fields.event = event;
  fields.stream_buffer_size = this->buffer_size_;
  fields.stream_buffer_fullness = this->ring_buffer_->available();
  fields.bytes_received = this->bytes_received_.load(std::memory_order_relaxed);
  fields.jiffies = millis();
  fields.output_buffer_size = this->buffer_size_;
  fields.output_buffer_fullness = queued > 0 ? static_cast<uint32_t>(queued) * 4 : 0;
  fields.elapsed_seconds = elapsed_ms / 1000;
  fields.elapsed_milliseconds = elapsed_ms;
  fields.server_timestamp = server_timestamp;

  uint8_t packet[STAT_PACKET_SIZE];
  this->send_control_(packet, build_stat(packet, fields));
}

void SlimprotoMediaSource::send_name_() {
  uint8_t body[34];
  const size_t name_length = std::min<size_t>(this->player_name_.size(), sizeof(body) - 2);
  body[0] = 0;
  std::memcpy(body + 1, this->player_name_.data(), name_length);
  body[name_length + 1] = 0;
  this->send_packet_("SETD", body, name_length + 2);
}

void SlimprotoMediaSource::send_packet_(const char *opcode, const void *body, size_t body_length) {
  if (this->control_state_ != ControlState::CONTROL_STATE_CONNECTED) {
    return;
  }
  uint8_t header[8];
  if (this->send_control_(header, build_header(header, opcode, body_length)) && body_length > 0) {
    this->send_control_(static_cast<const uint8_t *>(body), body_length);
  }
}

bool SlimprotoMediaSource::send_control_(const uint8_t *data, size_t length) {
  if (this->control_socket_ < 0) {
    return false;
  }
  const ssize_t sent = ::send(this->control_socket_, data, length, 0);
  if (sent != static_cast<ssize_t>(length)) {
    // A short write would corrupt the packet stream, so start over with a new connection
    ESP_LOGW(TAG, "Could not send to the server, reconnecting");
    this->close_control_(true);
    return false;
  }
  return true;
}

void SlimprotoMediaSource::process_events_() {
  const uint32_t events = this->events_.exchange(0, std::memory_order_acq_rel);

  if (events & STREAM_EVENT_DISCONNECTED) {
    uint8_t packet[9];
    const auto reason = static_cast<DisconnectReason>(this->disconnect_reason_.load());
    this->send_control_(packet, build_dsco(packet, reason));
  }
  if (events & STREAM_EVENT_HEADERS) {
    this->send_packet_("RESP", this->http_headers_, this->http_headers_length_.load());
  }
  if (events & STREAM_EVENT_LOADED) {
    this->send_stat_("STMl");
  }
  if (events & STREAM_EVENT_DECODE_DONE) {
    this->send_stat_("STMd");
  }
  if (events & STREAM_EVENT_DECODE_ERROR) {
    this->send_stat_("STMn");
  }
  if (events & STREAM_EVENT_FLUSHED) {
    this->send_stat_("STMf");
  }
  const bool decode_ended = (events & (STREAM_EVENT_DECODE_DONE | STREAM_EVENT_DECODE_ERROR)) != 0;
  const bool flushed_without_new_stream = (events & STREAM_EVENT_FLUSHED) != 0 && !this->stream_requested_;
  if ((decode_ended || flushed_without_new_stream) && this->get_state() != media_source::MediaSourceState::IDLE) {
    // Nothing more is coming for now: end playback when the output has played what it holds. A new strm command
    // cancels this, which is how the next track continues without a gap.
    uint32_t rate = this->current_track_rate_ != 0 ? this->current_track_rate_ : this->next_track_rate_.load();
    if (rate == 0) {
      rate = DEFAULT_SAMPLE_RATE;
    }
    const int32_t queued = static_cast<int32_t>(this->frames_written_.load() - this->frames_played_.load());
    const uint32_t queued_ms = queued > 0 ? static_cast<uint64_t>(queued) * 1000 / rate : 0;
    this->drain_deadline_ = millis() + queued_ms + DRAIN_GRACE_MS;
    this->awaiting_drain_ = true;
  }

  if (this->metadata_ready_.load(std::memory_order_acquire)) {
    this->send_packet_("META", this->metadata_, this->metadata_length_.load());
    this->metadata_ready_.store(false, std::memory_order_release);
  }
}

void SlimprotoMediaSource::update_playback_() {
  const uint32_t now = App.get_loop_component_start_time();
  const uint32_t played = this->frames_played_.load(std::memory_order_relaxed);

  if (this->next_track_pending_.load(std::memory_order_acquire)) {
    const uint32_t start = this->next_track_start_.load(std::memory_order_relaxed);
    if (static_cast<int32_t>(played - start) > 0) {
      this->current_track_start_ = start;
      this->current_track_rate_ = this->next_track_rate_.load(std::memory_order_relaxed);
      this->next_track_pending_.store(false, std::memory_order_release);
      this->track_playing_ = true;
      this->send_stat_("STMs");
    }
  }

  if (this->awaiting_drain_) {
    const uint32_t written = this->frames_written_.load(std::memory_order_relaxed);
    if (static_cast<int32_t>(written - played) <= 0 || time_reached(now, this->drain_deadline_)) {
      this->awaiting_drain_ = false;
      this->stream_requested_ = false;
      this->send_stat_("STMu");
      this->track_playing_ = false;
      if (this->get_state() != media_source::MediaSourceState::IDLE) {
        this->set_state_(media_source::MediaSourceState::IDLE);
      }
    }
  }

  if (this->track_playing_ && this->get_state() == media_source::MediaSourceState::PLAYING &&
      now - this->last_status_ >= STATUS_INTERVAL_MS) {
    this->last_status_ = now;
    ESP_LOGD(TAG, "stream buffer %zu/%zu B, output queued %d frames, received %u B",
             this->ring_buffer_->available(), this->buffer_size_,
             static_cast<int>(this->frames_written_.load(std::memory_order_relaxed) -
                              this->frames_played_.load(std::memory_order_relaxed)),
             static_cast<unsigned>(this->bytes_received_.load(std::memory_order_relaxed)));
    this->send_stat_("STMt");
  }
}

// --- Command line connection ---

void SlimprotoMediaSource::queue_cli_command_(const char *command) {
  if (this->control_state_ != ControlState::CONTROL_STATE_CONNECTED) {
    return;
  }
  snprintf(this->cli_command_, sizeof(this->cli_command_), "%02x:%02x:%02x:%02x:%02x:%02x %s\n", this->mac_[0],
           this->mac_[1], this->mac_[2], this->mac_[3], this->mac_[4], this->mac_[5], command);
  this->cli_pending_ = true;
}

void SlimprotoMediaSource::service_cli_() {
  const uint32_t now = App.get_loop_component_start_time();
  if (this->cli_state_ == CliState::CLI_STATE_IDLE) {
    if (!this->cli_pending_ || this->server_ip_ == 0) {
      return;
    }
    this->cli_socket_ = open_connection(this->server_ip_, this->cli_port_);
    if (this->cli_socket_ < 0) {
      this->cli_pending_ = false;
      return;
    }
    this->cli_state_ = CliState::CLI_STATE_CONNECTING;
    this->cli_started_ = now;
    return;
  }

  const int result = check_connection(this->cli_socket_);
  if (result > 0) {
    ::send(this->cli_socket_, this->cli_command_, std::strlen(this->cli_command_), 0);
    this->cli_pending_ = false;
    this->close_cli_();
  } else if (result < 0 || now - this->cli_started_ >= CLI_TIMEOUT_MS) {
    ESP_LOGW(TAG, "Could not reach the server's command line port %u", this->cli_port_);
    this->cli_pending_ = false;
    this->close_cli_();
  }
}

void SlimprotoMediaSource::close_cli_() {
  if (this->cli_socket_ >= 0) {
    ::close(this->cli_socket_);
    this->cli_socket_ = -1;
  }
  this->cli_state_ = CliState::CLI_STATE_IDLE;
}

// --- MediaSource interface ---

bool SlimprotoMediaSource::can_handle(const std::string &uri) const { return uri.starts_with(URI_PREFIX); }

bool SlimprotoMediaSource::play_uri(const std::string &uri) {
  this->pending_start_ = false;
  if (!this->is_ready() || this->is_failed() || !this->has_listener() || uri != URI_CURRENT) {
    return false;
  }
  if (!this->stream_requested_ || this->get_state() != media_source::MediaSourceState::IDLE) {
    return false;
  }
  this->set_state_(media_source::MediaSourceState::PLAYING);
  return true;
}

void SlimprotoMediaSource::handle_command(media_source::MediaSourceCommand command) {
  switch (command) {
    case media_source::MediaSourceCommand::STOP:
      // The orchestrator may send a stop before it delivers the play request that this source asked for
      if (!this->pending_start_ && this->get_state() != media_source::MediaSourceState::IDLE) {
        this->stop_locally_();
        this->queue_cli_command_("stop");
      }
      break;
    case media_source::MediaSourceCommand::PLAY:
      this->queue_cli_command_(this->get_state() == media_source::MediaSourceState::PAUSED ? "pause 0" : "play");
      break;
    case media_source::MediaSourceCommand::PAUSE:
      this->queue_cli_command_("pause 1");
      break;
    case media_source::MediaSourceCommand::NEXT:
      this->queue_cli_command_("playlist index +1");
      break;
    case media_source::MediaSourceCommand::PREVIOUS:
      this->queue_cli_command_("playlist index -1");
      break;
    case media_source::MediaSourceCommand::CLEAR_PLAYLIST:
      this->queue_cli_command_("playlist clear");
      break;
    case media_source::MediaSourceCommand::REPEAT_ALL:
      this->queue_cli_command_("playlist repeat 2");
      break;
    case media_source::MediaSourceCommand::REPEAT_ONE:
      this->queue_cli_command_("playlist repeat 1");
      break;
    case media_source::MediaSourceCommand::REPEAT_OFF:
      this->queue_cli_command_("playlist repeat 0");
      break;
    case media_source::MediaSourceCommand::SHUFFLE:
      this->queue_cli_command_("playlist shuffle 1");
      break;
    case media_source::MediaSourceCommand::UNSHUFFLE:
      this->queue_cli_command_("playlist shuffle 0");
      break;
  }
}

void SlimprotoMediaSource::notify_volume_changed(float volume) {
  // Skip changes that came from the server, which would otherwise be echoed back to it
  if (this->lms_volume_ >= 0.0f && std::fabs(volume - this->lms_volume_) < VOLUME_TOLERANCE) {
    return;
  }
  this->lms_volume_ = volume;
  if (this->control_state_ == ControlState::CONTROL_STATE_CONNECTED) {
    this->volume_hold_active_ = true;
    this->volume_hold_target_ = volume;
    this->volume_hold_until_ = App.get_loop_component_start_time() + VOLUME_HOLD_MS;
  }
  char command[24];
  snprintf(command, sizeof(command), "mixer volume %d", static_cast<int>(std::lround(volume * 100.0f)));
  this->queue_cli_command_(command);
}

void SlimprotoMediaSource::notify_mute_changed(bool is_muted) {
  if (is_muted == this->lms_muted_) {
    return;
  }
  this->lms_muted_ = is_muted;
  this->queue_cli_command_(is_muted ? "mixer muting 1" : "mixer muting 0");
}

void SlimprotoMediaSource::notify_audio_played(uint32_t frames, int64_t timestamp) {
  this->frames_played_.fetch_add(frames, std::memory_order_relaxed);
}

// --- Stream task ---

void SlimprotoMediaSource::stream_task_(void *params) {
  static_cast<SlimprotoMediaSource *>(params)->run_stream_task_();
}

void SlimprotoMediaSource::run_stream_task_() {
  StreamCommand command;
  while (true) {
    TickType_t wait = (this->http_phase_ != HttpPhase::HTTP_PHASE_NONE || this->decoding_) ? 0 : portMAX_DELAY;
    while (xQueueReceive(this->stream_commands_, &command, wait) == pdTRUE) {
      this->handle_stream_command_(command);
      wait = 0;
    }

    bool progress = this->service_http_();
    this->check_buffered_();
    if (this->decoding_) {
      progress |= this->passthrough_pcm_ ? this->service_pcm_() : this->service_decoder_();
    }
    if (!progress) {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}

void SlimprotoMediaSource::handle_stream_command_(const StreamCommand &command) {
  switch (command.type) {
    case StreamCommand::STREAM_COMMAND_START:
      this->begin_stream_(command);
      break;
    case StreamCommand::STREAM_COMMAND_FLUSH: {
      const bool active = this->http_phase_ != HttpPhase::HTTP_PHASE_NONE || this->decoding_;
      this->reset_stream_();
      if (command.hard_flush) {
        this->frames_written_.store(0);
        this->frames_played_.store(0);
        this->next_track_pending_.store(false);
        this->skip_frames_ = 0;
      }
      if (active || command.hard_flush) {
        this->set_event_(STREAM_EVENT_FLUSHED);
      }
      break;
    }
    case StreamCommand::STREAM_COMMAND_CONT:
      if (this->http_phase_ == HttpPhase::HTTP_PHASE_WAIT_CONT) {
        this->autostart_ -= 2;
        this->meta_interval_ = command.meta_interval;
        this->meta_next_ = command.meta_interval;
        this->http_phase_ = HttpPhase::HTTP_PHASE_BODY;
      }
      break;
    case StreamCommand::STREAM_COMMAND_CODEC:
      this->format_ = command.format;
      this->pcm_sample_size_ = command.pcm_sample_size;
      this->pcm_sample_rate_ = command.pcm_sample_rate;
      this->pcm_channels_ = command.pcm_channels;
      this->pcm_endianness_ = command.pcm_endianness;
      break;
  }
}

void SlimprotoMediaSource::begin_stream_(const StreamCommand &command) {
  this->reset_stream_();

  this->request_ = command.request;
  this->request_length_ = command.request_length;
  this->request_sent_ = 0;
  this->autostart_ = command.autostart;
  this->format_ = command.format;
  this->pcm_sample_size_ = command.pcm_sample_size;
  this->pcm_sample_rate_ = command.pcm_sample_rate;
  this->pcm_channels_ = command.pcm_channels;
  this->pcm_endianness_ = command.pcm_endianness;
  this->threshold_bytes_ =
      std::max<size_t>(1, std::min<size_t>(command.threshold_kb * 1024, this->buffer_size_ * 3 / 4));
  this->first_write_ = true;
  this->bytes_received_.store(0);

  this->stream_socket_ = open_connection(command.ip, command.port);
  if (this->stream_socket_ < 0) {
    this->finish_http_(DisconnectReason::DISCONNECT_REASON_UNREACHABLE);
    return;
  }
  this->http_started_ = millis();
  this->http_phase_ = HttpPhase::HTTP_PHASE_CONNECTING;
}

void SlimprotoMediaSource::close_stream_socket_() {
  if (this->stream_socket_ >= 0) {
    ::close(this->stream_socket_);
    this->stream_socket_ = -1;
  }
}

void SlimprotoMediaSource::reset_stream_() {
  this->close_stream_socket_();
  this->http_phase_ = HttpPhase::HTTP_PHASE_NONE;
  // The decoder holds a pointer into the ring buffer, so it must go first
  this->decoder_.reset();
  this->ring_buffer_->reset();
  if (this->request_ != nullptr) {
    RAMAllocator<char> allocator;
    allocator.deallocate(this->request_, this->request_length_);
    this->request_ = nullptr;
  }
  this->decoding_ = false;
  this->buffered_ = false;
  this->stream_ended_ = false;
  this->passthrough_pcm_ = false;
  this->pcm_length_ = 0;
  this->headers_received_ = 0;
  this->header_match_ = 0;
  this->meta_interval_ = 0;
  this->meta_next_ = 0;
  this->meta_left_ = 0;
  this->meta_received_ = 0;
  this->meta_in_block_ = false;
}

void SlimprotoMediaSource::finish_http_(DisconnectReason reason) {
  this->close_stream_socket_();
  this->http_phase_ = HttpPhase::HTTP_PHASE_NONE;
  this->stream_ended_ = true;
  this->disconnect_reason_.store(static_cast<uint8_t>(reason));
  this->set_event_(STREAM_EVENT_DISCONNECTED);
}

bool SlimprotoMediaSource::service_http_() {
  switch (this->http_phase_) {
    case HttpPhase::HTTP_PHASE_NONE:
    case HttpPhase::HTTP_PHASE_WAIT_CONT:
      return false;
    case HttpPhase::HTTP_PHASE_CONNECTING: {
      const int result = check_connection(this->stream_socket_);
      if (result > 0) {
        this->http_phase_ = HttpPhase::HTTP_PHASE_SENDING;
        return true;
      }
      if (result < 0) {
        this->finish_http_(DisconnectReason::DISCONNECT_REASON_UNREACHABLE);
      } else if (millis() - this->http_started_ >= HTTP_CONNECT_TIMEOUT_MS) {
        this->finish_http_(DisconnectReason::DISCONNECT_REASON_TIMEOUT);
      }
      return false;
    }
    case HttpPhase::HTTP_PHASE_SENDING: {
      const ssize_t sent = ::send(this->stream_socket_, this->request_ + this->request_sent_,
                                  this->request_length_ - this->request_sent_, 0);
      if (sent < 0) {
        if (!would_block()) {
          this->finish_http_(DisconnectReason::DISCONNECT_REASON_REMOTE);
        }
        return false;
      }
      this->request_sent_ += sent;
      if (this->request_sent_ == this->request_length_) {
        this->http_started_ = millis();
        this->http_phase_ = HttpPhase::HTTP_PHASE_HEADERS;
      }
      return true;
    }
    case HttpPhase::HTTP_PHASE_HEADERS:
      return this->receive_headers_();
    case HttpPhase::HTTP_PHASE_BODY:
      return this->receive_body_();
  }
  return false;
}

bool SlimprotoMediaSource::receive_headers_() {
  static constexpr char END_OF_HEADERS[] = "\r\n\r\n";
  bool progress = false;
  // One byte at a time, so that no audio data is read along with the headers
  for (uint8_t i = 0; i < 64; i++) {
    char byte;
    const ssize_t received = ::recv(this->stream_socket_, &byte, 1, 0);
    if (received == 0) {
      this->finish_http_(DisconnectReason::DISCONNECT_REASON_REMOTE);
      return true;
    }
    if (received < 0) {
      if (!would_block()) {
        this->finish_http_(DisconnectReason::DISCONNECT_REASON_REMOTE);
      } else if (millis() - this->http_started_ >= HTTP_HEADER_TIMEOUT_MS) {
        this->finish_http_(DisconnectReason::DISCONNECT_REASON_TIMEOUT);
      }
      return progress;
    }
    progress = true;
    if (this->headers_received_ < HTTP_HEADERS_SIZE) {
      this->http_headers_[this->headers_received_++] = byte;
    }
    if (byte == END_OF_HEADERS[this->header_match_]) {
      this->header_match_++;
    } else {
      this->header_match_ = byte == '\r' ? 1 : 0;
    }
    if (this->header_match_ == 4) {
      this->http_headers_length_.store(this->headers_received_);
      this->set_event_(STREAM_EVENT_HEADERS);
      this->http_phase_ = this->autostart_ >= 2 ? HttpPhase::HTTP_PHASE_WAIT_CONT : HttpPhase::HTTP_PHASE_BODY;
      return true;
    }
  }
  return progress;
}

bool SlimprotoMediaSource::receive_body_() {
  // Drain the socket in one pass. The decoder shares this loop and can block while it waits for the output, so reading
  // a single small chunk per pass would cap the download speed below the bitrate of high quality streams.
  bool progress = false;
  for (uint8_t pass = 0; pass < MAX_RECEIVE_PASSES; pass++) {
    if (this->meta_interval_ != 0 && this->meta_next_ == 0) {
      if (!this->receive_metadata_()) {
        break;
      }
      progress = true;
      if (this->http_phase_ != HttpPhase::HTTP_PHASE_BODY) {
        // The stream ended while reading the metadata, so the socket is closed
        return true;
      }
      continue;
    }

    size_t wanted = std::min(this->ring_buffer_->free(), RECEIVE_BUFFER_SIZE);
    if (this->meta_interval_ != 0) {
      wanted = std::min<size_t>(wanted, this->meta_next_);
    }
    if (wanted == 0) {
      break;
    }

    const ssize_t received = ::recv(this->stream_socket_, this->receive_buffer_, wanted, 0);
    if (received > 0) {
      this->ring_buffer_->write_without_replacement(this->receive_buffer_, received, 0);
      this->bytes_received_.fetch_add(received, std::memory_order_relaxed);
      if (this->meta_interval_ != 0) {
        this->meta_next_ -= received;
      }
      progress = true;
      if (static_cast<size_t>(received) < wanted) {
        break;
      }
      continue;
    }
    if (received == 0) {
      this->finish_http_(DisconnectReason::DISCONNECT_REASON_OK);
      return true;
    }
    if (!would_block()) {
      this->finish_http_(DisconnectReason::DISCONNECT_REASON_REMOTE);
      return true;
    }
    break;
  }
  return progress;
}

bool SlimprotoMediaSource::receive_metadata_() {
  uint8_t *buffer = this->receive_buffer_;
  const size_t wanted = this->meta_in_block_ ? std::min(this->meta_left_, RECEIVE_BUFFER_SIZE) : 1;
  const ssize_t received = ::recv(this->stream_socket_, buffer, wanted, 0);
  if (received <= 0) {
    if (received == 0) {
      this->finish_http_(DisconnectReason::DISCONNECT_REASON_OK);
      return true;
    }
    if (!would_block()) {
      this->finish_http_(DisconnectReason::DISCONNECT_REASON_REMOTE);
    }
    return false;
  }

  if (!this->meta_in_block_) {
    this->meta_left_ = static_cast<size_t>(buffer[0]) * 16;
    this->meta_received_ = 0;
    this->meta_in_block_ = true;
  } else {
    if (!this->metadata_ready_.load(std::memory_order_acquire) && this->meta_received_ < METADATA_SIZE) {
      const size_t chunk = std::min<size_t>(received, METADATA_SIZE - this->meta_received_);
      std::memcpy(this->metadata_ + this->meta_received_, buffer, chunk);
      this->meta_received_ += chunk;
    }
    this->meta_left_ -= received;
  }

  if (this->meta_left_ == 0) {
    this->meta_in_block_ = false;
    this->meta_next_ = this->meta_interval_;
    if (this->meta_received_ > 0) {
      this->metadata_length_.store(this->meta_received_);
      this->metadata_ready_.store(true, std::memory_order_release);
    }
  }
  return true;
}

void SlimprotoMediaSource::check_buffered_() {
  if (this->buffered_ || this->format_ == '?') {
    return;
  }
  if (!this->stream_ended_ && this->http_phase_ != HttpPhase::HTTP_PHASE_BODY) {
    return;
  }
  const size_t available = this->ring_buffer_->available();
  if (!this->stream_ended_ && available < this->threshold_bytes_) {
    return;
  }

  this->buffered_ = true;
  if (available == 0) {
    // The stream failed before any data arrived. The server already knows from the DSCO packet.
    return;
  }
  if (!this->start_decoding_()) {
    this->set_event_(STREAM_EVENT_DECODE_ERROR);
    return;
  }
  this->decoding_ = true;
  if (this->autostart_ == 0) {
    this->set_event_(STREAM_EVENT_LOADED);
  } else {
    this->output_open_.store(true);
  }
}

bool SlimprotoMediaSource::start_decoding_() {
  audio::AudioFileType file_type;
  switch (this->format_) {
    case 'f':
      file_type = audio::AudioFileType::FLAC;
      break;
    case 'm':
      file_type = audio::AudioFileType::MP3;
      break;
    case 'p':
      this->pcm_format_ =
          decode_pcm_format(this->pcm_sample_size_, this->pcm_sample_rate_, this->pcm_channels_, this->pcm_endianness_);
      if (this->pcm_format_.known) {
        this->passthrough_pcm_ = true;
        this->stream_info_ = audio::AudioStreamInfo(this->pcm_format_.bits_per_sample, this->pcm_format_.channels,
                                                    this->pcm_format_.sample_rate);
        return true;
      }
      file_type = audio::AudioFileType::WAV;
      break;
    default:
      ESP_LOGW(TAG, "Unsupported format '%c'", this->format_);
      return false;
  }

  this->decoder_ = std::make_unique<audio::AudioDecoder>(DECODER_INPUT_SIZE, DECODER_OUTPUT_SIZE);
  std::weak_ptr<ring_buffer::RingBuffer> source = this->ring_buffer_;
  if (this->decoder_->add_source(source) != ESP_OK ||
      this->decoder_->add_sink(static_cast<audio::AudioSinkCallback *>(this)) != ESP_OK ||
      this->decoder_->start(file_type) != ESP_OK) {
    ESP_LOGW(TAG, "Could not start the decoder");
    this->decoder_.reset();
    return false;
  }
  return true;
}

bool SlimprotoMediaSource::service_decoder_() {
  const audio::AudioDecoderState state = this->decoder_->decode(this->stream_ended_);
  if (state == audio::AudioDecoderState::DECODING) {
    return true;
  }
  this->decoder_.reset();
  this->decoding_ = false;
  this->close_stream_socket_();
  this->http_phase_ = HttpPhase::HTTP_PHASE_NONE;
  this->stream_ended_ = true;
  this->set_event_(state == audio::AudioDecoderState::FINISHED ? STREAM_EVENT_DECODE_DONE : STREAM_EVENT_DECODE_ERROR);
  return true;
}

bool SlimprotoMediaSource::service_pcm_() {
  const size_t sample_bytes = this->pcm_format_.bits_per_sample / 8;
  const size_t frame_bytes = sample_bytes * this->pcm_format_.channels;

  if (this->pcm_length_ == 0) {
    const size_t available = this->ring_buffer_->available();
    size_t chunk = std::min(available, PCM_BUFFER_SIZE);
    chunk -= chunk % frame_bytes;
    if (chunk == 0) {
      if (this->stream_ended_) {
        this->decoding_ = false;
        this->set_event_(STREAM_EVENT_DECODE_DONE);
        return true;
      }
      return false;
    }
    size_t read = this->ring_buffer_->read(this->pcm_buffer_, chunk, 0);
    read -= read % frame_bytes;
    if (this->pcm_format_.big_endian && sample_bytes > 1) {
      for (size_t offset = 0; offset < read; offset += sample_bytes) {
        std::reverse(this->pcm_buffer_ + offset, this->pcm_buffer_ + offset + sample_bytes);
      }
    }
    this->pcm_length_ = read;
  }

  const size_t written =
      this->audio_sink_write(this->pcm_buffer_, this->pcm_length_, pdMS_TO_TICKS(PCM_WRITE_TIMEOUT_MS));
  if (written > 0 && written < this->pcm_length_) {
    std::memmove(this->pcm_buffer_, this->pcm_buffer_ + written, this->pcm_length_ - written);
  }
  this->pcm_length_ -= written;
  return written > 0;
}

size_t SlimprotoMediaSource::audio_sink_write(uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  const TickType_t wait = std::max<TickType_t>(ticks_to_wait, 1);
  if (!this->output_open_.load(std::memory_order_relaxed)) {
    vTaskDelay(wait);
    return 0;
  }

  const uint32_t now = millis();
  const uint32_t start_at = this->start_at_.load(std::memory_order_relaxed);
  if (start_at != 0) {
    if (!time_reached(now, start_at)) {
      vTaskDelay(wait);
      return 0;
    }
    this->start_at_.store(0, std::memory_order_relaxed);
  }
  const uint32_t pause_until = this->pause_until_.load(std::memory_order_relaxed);
  if (pause_until != 0) {
    if (!time_reached(now, pause_until)) {
      vTaskDelay(wait);
      return 0;
    }
    this->pause_until_.store(0, std::memory_order_relaxed);
  }

  if (this->decoder_ != nullptr) {
    const auto &info = this->decoder_->get_audio_stream_info();
    if (!info.has_value()) {
      vTaskDelay(wait);
      return 0;
    }
    this->stream_info_ = info.value();
  }

  const uint32_t skip_ms = this->skip_ms_.exchange(0, std::memory_order_relaxed);
  if (skip_ms != 0) {
    this->skip_frames_ += static_cast<uint64_t>(skip_ms) * this->stream_info_.get_sample_rate() / 1000;
  }
  if (this->skip_frames_ != 0) {
    const uint32_t frames = std::min(this->skip_frames_, this->stream_info_.bytes_to_frames(length));
    if (frames != 0) {
      this->skip_frames_ -= frames;
      return this->stream_info_.frames_to_bytes(frames);
    }
  }

  const size_t written = this->write_output(data, length, pdTICKS_TO_MS(wait), this->stream_info_);
  if (written > 0) {
    if (this->first_write_) {
      this->first_write_ = false;
      this->next_track_start_.store(this->frames_written_.load(std::memory_order_relaxed), std::memory_order_relaxed);
      this->next_track_rate_.store(this->stream_info_.get_sample_rate(), std::memory_order_relaxed);
      this->next_track_pending_.store(true, std::memory_order_release);
    }
    this->frames_written_.fetch_add(this->stream_info_.bytes_to_frames(written), std::memory_order_relaxed);
  }
  return written;
}

}  // namespace esphome::slimproto

#endif  // USE_ESP32
