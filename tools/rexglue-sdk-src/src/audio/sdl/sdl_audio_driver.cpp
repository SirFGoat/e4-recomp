/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/assert.h>
#include <rex/perf/sync_record.h>
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <SDL3/SDL.h>

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");
REXCVAR_DECLARE(bool, sync_monitor);
REXCVAR_DEFINE_INT32(audio_device_frames, 0, "Audio",
                     "Audio device buffer in sample frames at 48 kHz (e.g. 128 = 2.7 ms, 256 = "
                     "5.3 ms); 0 = system default. Smaller = lower latency, may crackle.");

namespace rex::audio::sdl {

namespace {
// Sync monitor state (one audio device per process).
int g_sync_device_frames = 0;
int g_sync_device_rate = 48000;
std::atomic<uint64_t> g_sync_stream_us{0};
std::atomic<uint32_t> g_sync_callbacks{0};
std::atomic<uint32_t> g_sync_underruns{0};
}  // namespace

SDLAudioDriver::SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool SDLAudioDriver::Initialize() {
  // Set audio category for proper OS audio handling
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");

  // Set app name for audio device identification
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");

  if (REXCVAR_GET(audio_device_frames) > 0) {
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES,
                std::to_string(REXCVAR_GET(audio_device_frames)).c_str());
  }

  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXAPU_ERROR("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    return false;
  }
  sdl_initialized_ = true;

  SDL_AudioSpec desired_spec = {};
  SDL_AudioSpec obtained_spec = {};
  desired_spec.freq = frame_frequency_;
  desired_spec.format = SDL_AUDIO_F32LE;
  desired_spec.channels = frame_channels_;
  sdl_device_channels_ = frame_channels_;
  sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                          SDLCallback, this);
  if (!sdl_stream_) {
    REXAPU_ERROR("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }

  SDL_AudioDeviceID sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
  if (!sdl_device) {
    REXAPU_ERROR("SDL_GetAudioStreamDevice() failed: {}", SDL_GetError());
    return false;
  }

  if (!SDL_GetAudioDeviceFormat(sdl_device, &obtained_spec, &g_sync_device_frames)) {
    REXAPU_WARN("SDL_GetAudioDeviceFormat() failed: {}", SDL_GetError());
    obtained_spec = desired_spec;
  }

  // A 1-channel device gets the stereo fold too, then SDL collapses to mono.
  // Handing it a 6ch stream instead would use SDL's own downmix.
  if (obtained_spec.channels <= 2) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
    desired_spec.channels = 2;
    sdl_device_channels_ = 2;
    sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                            SDLCallback, this);
    if (!sdl_stream_) {
      REXAPU_ERROR("SDL_OpenAudioDeviceStream() stereo fallback failed: {}", SDL_GetError());
      return false;
    }
    sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
    if (!sdl_device) {
      REXAPU_ERROR("SDL_GetAudioStreamDevice() failed after stereo fallback: {}", SDL_GetError());
      return false;
    }
  }

  // The endpoint layout decides which mix the callback runs, and it is the
  // first thing worth knowing when a report says the balance is wrong on one
  // speaker setup and right on another.
  const char* device_name = SDL_GetAudioDeviceName(sdl_device);
  REXAPU_INFO("audio endpoint '{}': {} ch, {} Hz, format 0x{:04X}; submitting {} ch; device "
              "buffer {} frames",
              device_name ? device_name : "?", obtained_spec.channels, obtained_spec.freq,
              static_cast<uint32_t>(obtained_spec.format), static_cast<int>(sdl_device_channels_),
              g_sync_device_frames);
  g_sync_device_rate = obtained_spec.freq > 0 ? obtained_spec.freq : int(frame_frequency_);

  if (!SDL_ResumeAudioDevice(sdl_device)) {
    REXAPU_ERROR("SDL_ResumeAudioDevice() failed: {}", SDL_GetError());
    return false;
  }

  return true;
}

void SDLAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, input_frame, frame_samples_ * sizeof(float));

  static uint32_t sdl_submit_count = 0;
  if (sdl_submit_count < 10) {
    REXAPU_DEBUG("SDLAudioDriver::SubmitFrame: frame_ptr={:08X} queued_count={}", frame_ptr,
                 frames_queued_.size() + 1);
    sdl_submit_count++;
  }

  size_t depth;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
    depth = frames_queued_.size();
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(frames_queued_.size()));
  }

  if (REXCVAR_GET(sync_monitor)) {
    // Audio latency: a frame submitted now plays after the frames ahead of it in our
    // queue, whatever SDL's stream still holds, and the device buffer.
    static auto window = std::chrono::steady_clock::now();
    static uint64_t submits = 0, depth_sum = 0;
    static size_t depth_min = SIZE_MAX, depth_max = 0;
    ++submits;
    depth_sum += depth;
    depth_min = std::min(depth_min, depth);
    depth_max = std::max(depth_max, depth);
    const auto now = std::chrono::steady_clock::now();
    const double window_s = std::chrono::duration<double>(now - window).count();
    if (window_s >= 2.0) {
      const double frame_ms = 1000.0 * channel_samples_ / frame_frequency_;
      const double stream_ms = double(g_sync_stream_us.exchange(0)) / 1000.0 /
                               std::max<uint32_t>(g_sync_callbacks.exchange(0), 1);
      const double device_ms = 1000.0 * g_sync_device_frames / std::max(g_sync_device_rate, 1);
      const double queue_ms = frame_ms * double(depth_sum) / double(submits);
      REXAPU_INFO(
          "Sync audio: game made {:.1f} frames/s (expect {:.1f}), queue {:.1f} ms avg ({:.1f}-"
          "{:.1f}), SDL stream {:.1f} ms, device buffer {:.1f} ms -> sound heard ~{:.0f} ms "
          "after the game plays it (+ Windows mixer), {} underruns",
          submits / window_s, 1000.0 / frame_ms, queue_ms, frame_ms * depth_min,
          frame_ms * depth_max, stream_ms, device_ms, queue_ms + stream_ms + device_ms,
          g_sync_underruns.exchange(0));
      window = now;
      submits = depth_sum = 0;
      depth_min = SIZE_MAX;
      depth_max = 0;
    }
  }
}

void SDLAudioDriver::Shutdown() {
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void SDLAudioDriver::SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                 [[maybe_unused]] int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback called with nullptr.");
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int sample_count =
      static_cast<int>(channel_samples_ * std::max<uint8_t>(driver->sdl_device_channels_, 1));
  const int len = static_cast<int>(sizeof(float) * sample_count);
  float* data = SDL_stack_alloc(float, sample_count);
  if (!data) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback failed to allocate {} samples", sample_count);
    return;
  }
  // Snapshot once. A change mid-callback would split the frame across two mixes.
  const StereoFold fold = GetStereoFold();
  const SurroundMix mix = GetSurroundMix();
  const float gain = GetOutputGain();
  {
    // Sync monitor: audio already handed to SDL but not yet taken by the device.
    const int bytes_per_ms =
        int(sizeof(float)) * std::max<int>(driver->sdl_device_channels_, 1) * 48;
    const int queued = SDL_GetAudioStreamQueued(stream);
    g_sync_stream_us.fetch_add(queued > 0 ? uint64_t(queued) * 1000 / bytes_per_ms : 0);
    g_sync_callbacks.fetch_add(1);
  }
  while (additional_amount > 0) {
    static uint32_t sdl_callback_count = 0;
    std::unique_lock<std::mutex> guard(driver->frames_mutex_);
    if (driver->frames_queued_.empty()) {
      if (sdl_callback_count < 10) {
        REXAPU_DEBUG("SDLCallback: no frames queued (silence)");
        sdl_callback_count++;
      }
      if (driver->sync_started_) {
        g_sync_underruns.fetch_add(1);
      }
      std::memset(data, 0, len);
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed while filling silence: {}", SDL_GetError());
        break;
      }
      additional_amount -= len;
    } else {
      auto buffer = driver->frames_queued_.front();
      driver->frames_queued_.pop();
      driver->sync_started_ = true;
      if (REXCVAR_GET(audio_mute)) {
        std::memset(data, 0, len);
      } else {
        switch (driver->sdl_device_channels_) {
          case 2:
            conversion::sequential_6_BE_to_interleaved_2_LE(data, buffer, channel_samples_, fold,
                                                            gain);
            break;
          case 6:
            conversion::sequential_6_BE_to_interleaved_6_LE(data, buffer, channel_samples_, mix,
                                                            gain);
            break;
          default:
            assert_unhandled_case(driver->sdl_device_channels_);
            break;
        }
      }
      if (rex::sync_record::Enabled()) {
        // Loudness of this frame and when it will be heard: after what SDL still holds
        // and the device buffer.
        const int ch = std::max<int>(driver->sdl_device_channels_, 1);
        const int queued = SDL_GetAudioStreamQueued(stream);
        const double heard_ms =
            double(rex::sync_record::NowNs()) / 1e6 +
            (queued > 0 ? double(queued) / (sizeof(float) * ch * 48.0) : 0.0) +
            1000.0 * g_sync_device_frames / std::max(g_sync_device_rate, 1);
        static float low_state = 0.0f;
        double sum = 0.0, low_sum = 0.0;
        for (uint32_t i = 0; i < channel_samples_; ++i) {
          float mono = 0.0f;
          for (int c = 0; c < ch; ++c) mono += data[i * ch + c];
          mono /= float(ch);
          low_state += 0.02f * (mono - low_state);  // ~150 Hz one-pole low-pass (kick drum)
          sum += double(mono) * mono;
          low_sum += double(low_state) * low_state;
        }
        rex::sync_record::Write(
            rex::sync_record::Stream::kAudio,
            fmt::format("{:.3f},{:.5f},{:.5f}", heard_ms, std::sqrt(sum / channel_samples_),
                        std::sqrt(low_sum / channel_samples_)));
      }
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed: {}", SDL_GetError());
        driver->frames_unused_.push(buffer);
        break;
      }
      driver->frames_unused_.push(buffer);

      auto ret = driver->semaphore_->Release(1, nullptr);
      assert_true(ret);
      additional_amount -= len;
    }
  }
  SDL_stack_free(data);
}

}  // namespace rex::audio::sdl
