#include "services/audio_service.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#define MINIAUDIO_IMPLEMENTATION
#ifdef APP_MINIAUDIO_HEADER
#include APP_MINIAUDIO_HEADER
#else
#include <miniaudio.h>
#endif

#include "utils/asset_manager.h"
#include "utils/logger.h"

namespace service {
namespace {

constexpr const char* kShutterAsset = "audio/shutter.wav";
constexpr const char* kClickAsset   = "audio/click.wav";
constexpr ma_uint32 kPlaybackChannels = 2;
constexpr ma_uint32 kCaptureRate      = 16000;
constexpr ma_uint32 kCaptureChannels  = 1;

struct AudioData {
  ma_uint32 sample_rate{0};
  ma_uint32 channels{0};
  std::vector<int16_t> samples;
};

std::shared_ptr<AudioData> load_audio_asset(const char* asset_name) {
  std::string path;
  if (!asset_name || !asset::AssetManager::resolve_path(asset_name, path)) {
    LOG_WARN("Audio asset not found: {}", asset_name ? asset_name : "");
    return nullptr;
  }

  ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 0, 0);
  ma_uint64 frame_count    = 0;
  void* pcm_frames         = nullptr;
  const ma_result result = ma_decode_file(path.c_str(), &config, &frame_count, &pcm_frames);
  if (result != MA_SUCCESS || !pcm_frames || frame_count == 0 || config.channels == 0 ||
      config.sampleRate == 0) {
    LOG_WARN("Failed to decode audio asset {}: {}", path, ma_result_description(result));
    if (pcm_frames) {
      ma_free(pcm_frames, nullptr);
    }
    return nullptr;
  }

  auto audio                     = std::make_shared<AudioData>();
  audio->sample_rate             = config.sampleRate;
  audio->channels                = config.channels;
  const size_t sample_count      = static_cast<size_t>(frame_count) * config.channels;
  const auto* decoded_samples    = static_cast<const int16_t*>(pcm_frames);
  audio->samples.assign(decoded_samples, decoded_samples + sample_count);
  ma_free(pcm_frames, nullptr);
  LOG_DEBUG("Loaded audio asset: {} rate={} channels={}",
            path,
            audio->sample_rate,
            audio->channels);
  return audio;
}

std::string device_id(ma_backend backend, const ma_device_id& id) {
  if (backend == ma_backend_pulseaudio) {
    return id.pulse;
  }

  constexpr char kHex[] = "0123456789abcdef";
  constexpr size_t kMaxBytes = 16;
  const auto* bytes = reinterpret_cast<const unsigned char*>(&id);
  std::string value;
  value.reserve(kMaxBytes * 2);
  for (size_t i = 0; i < kMaxBytes && i < sizeof(ma_device_id); ++i) {
    value.push_back(kHex[bytes[i] >> 4U]);
    value.push_back(kHex[bytes[i] & 0x0FU]);
  }
  return value;
}

const ma_device_info* select_default_device(ma_device_info* devices,
                                            ma_uint32 count,
                                            const char* direction,
                                            ma_backend backend) {
  if (!devices || count == 0) {
    LOG_ERROR("No miniaudio {} device found", direction);
    return nullptr;
  }

  const ma_device_info* selected = &devices[0];
  for (ma_uint32 i = 0; i < count; ++i) {
    if (devices[i].isDefault) {
      selected = &devices[i];
      break;
    }
  }
  LOG_INFO("Selected PulseAudio {}: name='{}' id='{}'",
           direction,
           selected->name,
           device_id(backend, selected->id));
  return selected;
}

bool write_wav_file(const std::string& path, const std::vector<int16_t>& samples) {
  if (path.empty() || samples.empty()) {
    return false;
  }

  (void)std::remove(path.c_str());
  ma_encoder_config config =
      ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, kCaptureChannels, kCaptureRate);
  ma_encoder encoder{};
  ma_result result = ma_encoder_init_file(path.c_str(), &config, &encoder);
  if (result != MA_SUCCESS) {
    LOG_ERROR("Failed to create WAV file {}: {}", path, ma_result_description(result));
    return false;
  }

  ma_uint64 frames_written = 0;
  const ma_uint64 frames   = samples.size() / kCaptureChannels;
  result = ma_encoder_write_pcm_frames(&encoder, samples.data(), frames, &frames_written);
  ma_encoder_uninit(&encoder);
  if (result != MA_SUCCESS || frames_written != frames) {
    LOG_ERROR("Failed to write WAV file {}: {}", path, ma_result_description(result));
    return false;
  }
  return true;
}

}  // namespace

struct AudioService::Impl {
  ma_context context{};
  bool context_initialized{false};
  std::string playback_name;
  std::string capture_name;

  ma_device playback_device{};
  bool playback_initialized{false};
  bool playback_started{false};
  std::shared_ptr<AudioData> click_audio;
  std::shared_ptr<AudioData> shutter_audio;
  std::shared_ptr<AudioData> active_audio;
  std::atomic<size_t> playback_frame_offset{0};
  std::atomic<bool> playback_active{false};
  std::mutex playback_mutex;

  ma_device capture_device{};
  bool capture_initialized{false};
  bool capture_started{false};
  std::mutex capture_mutex;
  std::vector<int16_t> capture_samples;
  std::string capture_path;

  ~Impl() { shutdown(); }

  static void playback_callback(ma_device* device,
                                void* output,
                                const void* input,
                                ma_uint32 frame_count) {
    (void)input;
    auto* impl = static_cast<Impl*>(device->pUserData);
    auto* out  = static_cast<int16_t*>(output);
    const ma_uint32 output_channels = std::max<ma_uint32>(1, device->playback.channels);
    if (!out || frame_count == 0) {
      return;
    }
    std::memset(out, 0, static_cast<size_t>(frame_count) * output_channels * sizeof(int16_t));
    if (!impl || !impl->playback_active.load()) {
      return;
    }

    std::shared_ptr<AudioData> audio = std::atomic_load(&impl->active_audio);
    if (!audio || audio->samples.empty() || audio->channels == 0) {
      impl->playback_active.store(false);
      return;
    }

    const ma_uint32 input_channels = audio->channels;
    const size_t total_frames      = audio->samples.size() / input_channels;
    const size_t offset            = impl->playback_frame_offset.load();
    if (offset >= total_frames) {
      impl->playback_active.store(false);
      return;
    }

    const size_t copy_frames = std::min<size_t>(frame_count, total_frames - offset);
    for (size_t frame = 0; frame < copy_frames; ++frame) {
      const size_t input_base  = (offset + frame) * input_channels;
      const size_t output_base = frame * output_channels;
      for (ma_uint32 channel = 0; channel < output_channels; ++channel) {
        const ma_uint32 source_channel =
            input_channels == 1 ? 0 : std::min(channel, input_channels - 1);
        out[output_base + channel] = audio->samples[input_base + source_channel];
      }
    }

    const size_t next_offset = offset + copy_frames;
    impl->playback_frame_offset.store(next_offset);
    if (next_offset >= total_frames) {
      impl->playback_active.store(false);
    }
  }

  static void capture_callback(ma_device* device,
                               void* output,
                               const void* input,
                               ma_uint32 frame_count) {
    (void)output;
    auto* impl = static_cast<Impl*>(device->pUserData);
    if (!impl || !input || frame_count == 0) {
      return;
    }

    const auto* samples = static_cast<const int16_t*>(input);
    const size_t count  = static_cast<size_t>(frame_count) * kCaptureChannels;
    std::lock_guard<std::mutex> lock(impl->capture_mutex);
    impl->capture_samples.insert(impl->capture_samples.end(), samples, samples + count);
  }

  bool initialize() {
    if (context_initialized) {
      return true;
    }

#if defined(__linux__)
    const ma_backend backends[] = {ma_backend_pulseaudio};
    const ma_result result      = ma_context_init(backends, 1, nullptr, &context);
#else
    const ma_result result = ma_context_init(nullptr, 0, nullptr, &context);
#endif
    if (result != MA_SUCCESS) {
      LOG_ERROR("Failed to initialize miniaudio context: {}", ma_result_description(result));
      return false;
    }
    context_initialized = true;

    ma_device_info* playback_devices = nullptr;
    ma_device_info* capture_devices  = nullptr;
    ma_uint32 playback_count         = 0;
    ma_uint32 capture_count          = 0;
    const ma_result enumerate_result = ma_context_get_devices(&context,
                                                               &playback_devices,
                                                               &playback_count,
                                                               &capture_devices,
                                                               &capture_count);
    if (enumerate_result != MA_SUCCESS) {
      LOG_ERROR("Failed to enumerate miniaudio devices: {}",
                ma_result_description(enumerate_result));
      shutdown();
      return false;
    }

    const auto* playback =
        select_default_device(playback_devices, playback_count, "sink", context.backend);
    const auto* capture =
        select_default_device(capture_devices, capture_count, "source", context.backend);
    if (!playback || !capture) {
      shutdown();
      return false;
    }
    playback_name = playback->name;
    capture_name  = capture->name;

    click_audio   = load_audio_asset(kClickAsset);
    shutter_audio = load_audio_asset(kShutterAsset);
    if (!click_audio || !shutter_audio) {
      shutdown();
      return false;
    }
    if (click_audio->sample_rate != shutter_audio->sample_rate) {
      LOG_ERROR("Audio assets must use the same sample rate");
      shutdown();
      return false;
    }

    ma_device_config config   = ma_device_config_init(ma_device_type_playback);
    config.playback.format    = ma_format_s16;
    config.playback.channels  = kPlaybackChannels;
    config.playback.shareMode = ma_share_mode_shared;
    config.sampleRate         = click_audio->sample_rate;
    config.dataCallback       = playback_callback;
    config.pUserData          = this;
    ma_result device_result   = ma_device_init(&context, &config, &playback_device);
    if (device_result != MA_SUCCESS) {
      LOG_ERROR("Failed to initialize PulseAudio sink: {}",
                ma_result_description(device_result));
      shutdown();
      return false;
    }
    playback_initialized = true;

    device_result = ma_device_start(&playback_device);
    if (device_result != MA_SUCCESS) {
      LOG_ERROR("Failed to start PulseAudio sink: {}", ma_result_description(device_result));
      shutdown();
      return false;
    }
    playback_started = true;
    LOG_INFO("Audio ready: backend={} sink='{}' source='{}'",
             ma_get_backend_name(context.backend),
             playback_name,
             capture_name);
    return true;
  }

  bool play(const std::shared_ptr<AudioData>& audio) {
    if (!initialize() || !audio) {
      return false;
    }
    std::lock_guard<std::mutex> lock(playback_mutex);
    std::atomic_store(&active_audio, audio);
    playback_frame_offset.store(0);
    playback_active.store(true);
    return true;
  }

  bool start_recording(const std::string& path) {
    if (path.empty() || !initialize()) {
      return false;
    }
    if (capture_started) {
      return true;
    }

    {
      std::lock_guard<std::mutex> lock(capture_mutex);
      capture_samples.clear();
      capture_samples.reserve(kCaptureRate * kCaptureChannels * 10);
      capture_path = path;
    }

    ma_device_config config   = ma_device_config_init(ma_device_type_capture);
    config.capture.format     = ma_format_s16;
    config.capture.channels   = kCaptureChannels;
    config.capture.shareMode  = ma_share_mode_shared;
    config.sampleRate         = kCaptureRate;
    config.dataCallback       = capture_callback;
    config.pUserData          = this;
    ma_result result          = ma_device_init(&context, &config, &capture_device);
    if (result != MA_SUCCESS) {
      LOG_ERROR("Failed to initialize PulseAudio source: {}", ma_result_description(result));
      return false;
    }
    capture_initialized = true;

    result = ma_device_start(&capture_device);
    if (result != MA_SUCCESS) {
      LOG_ERROR("Failed to start PulseAudio source: {}", ma_result_description(result));
      ma_device_uninit(&capture_device);
      capture_initialized = false;
      return false;
    }
    capture_started = true;
    LOG_INFO("Audio recording started: {} source='{}'", path, capture_name);
    return true;
  }

  bool stop_recording() {
    if (capture_started) {
      ma_device_stop(&capture_device);
      capture_started = false;
    }
    if (capture_initialized) {
      ma_device_uninit(&capture_device);
      capture_initialized = false;
    }

    std::vector<int16_t> samples;
    std::string path;
    {
      std::lock_guard<std::mutex> lock(capture_mutex);
      samples = std::move(capture_samples);
      path    = std::move(capture_path);
    }
    if (path.empty()) {
      return true;
    }
    const bool saved = write_wav_file(path, samples);
    LOG_INFO("Audio recording stopped: {} samples={} saved={}", path, samples.size(), saved);
    return saved;
  }

  void shutdown() {
    playback_active.store(false);
    (void)stop_recording();
    if (playback_started) {
      ma_device_stop(&playback_device);
      playback_started = false;
    }
    if (playback_initialized) {
      ma_device_uninit(&playback_device);
      playback_initialized = false;
    }
    std::atomic_store(&active_audio, std::shared_ptr<AudioData>{});
    click_audio.reset();
    shutter_audio.reset();
    if (context_initialized) {
      ma_context_uninit(&context);
      context_initialized = false;
    }
  }
};

AudioService::AudioService() = default;

AudioService::~AudioService() { stop(); }

void AudioService::ensure_impl_() {
  if (!impl_) {
    impl_ = std::make_unique<Impl>();
  }
}

void AudioService::start() {
  ensure_impl_();
  if (state_ == AudioServiceState::Recording) {
    return;
  }
  if (!impl_->initialize()) {
    state_          = AudioServiceState::Error;
    status_message_ = "Audio unavailable";
    return;
  }
  state_          = AudioServiceState::Ready;
  status_message_ = "Audio ready";
}

void AudioService::stop() {
  if (impl_) {
    impl_->shutdown();
  }
  state_          = AudioServiceState::Idle;
  status_message_ = "Audio idle";
  recording_path_.clear();
}

void AudioService::update(uint32_t /*delta_ms*/) {}

bool AudioService::play_shutter() {
  ensure_impl_();
  const bool ok = impl_->initialize() && impl_->play(impl_->shutter_audio);
  state_          = ok ? AudioServiceState::Ready : AudioServiceState::Error;
  status_message_ = ok ? "Shutter sound played" : "Shutter sound unavailable";
  return ok;
}

bool AudioService::play_click() {
  ensure_impl_();
  const bool ok = impl_->initialize() && impl_->play(impl_->click_audio);
  state_          = ok ? AudioServiceState::Ready : AudioServiceState::Error;
  status_message_ = ok ? "Click sound played" : "Click sound unavailable";
  return ok;
}

bool AudioService::start_recording(const std::string& path) {
  ensure_impl_();
  if (state_ == AudioServiceState::Recording) {
    return true;
  }
  if (!impl_->start_recording(path)) {
    state_          = AudioServiceState::Error;
    status_message_ = "Audio recording failed";
    return false;
  }
  recording_path_ = path;
  state_          = AudioServiceState::Recording;
  status_message_ = "Audio recording";
  return true;
}

bool AudioService::stop_recording() {
  if (state_ != AudioServiceState::Recording) {
    return true;
  }
  ensure_impl_();
  const bool ok   = impl_->stop_recording();
  state_          = ok ? AudioServiceState::Ready : AudioServiceState::Error;
  status_message_ = ok ? "Audio ready" : "Audio stop failed";
  return ok;
}

}  // namespace service
