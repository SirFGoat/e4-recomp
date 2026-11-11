#include "calibration.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "imgui.h"

namespace {
constexpr int kRate = 48000;
constexpr int64_t kPeriodNs = 600'000'000;  // 100 BPM
constexpr int kCountIn = 4;                  // beats before taps count
constexpr int kBeats = 20;                   // beats recorded per test
constexpr int64_t kMaxErrorNs = 250'000'000;
constexpr int kMinTaps = 8;
constexpr double kMaxSpreadMs = 60.0;

// Short wood-block style click.
float ClickSample(int pos) {
  const double t = double(pos) / kRate;
  if (t > 0.05) return 0.0f;
  const double env = std::exp(-t / 0.010);
  return float(0.55 * env * (0.7 * std::sin(2 * 3.14159265 * 1100 * t) +
                             0.3 * std::sin(2 * 3.14159265 * 2300 * t)));
}

double Median(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}
}  // namespace

Calibration::~Calibration() { CloseAudio(); }

void Calibration::Open() {
  step_ = Step::kIntro;
  listen_ = watch_ = Measure{};
  start_ns_ = end_ns_ = 0;
  taps_.clear();
  OpenAudio();
  sound_on_ = false;
}

void Calibration::Demo(double listen_ms, double watch_ms) {
  Open();
  listen_.ok = watch_.ok = true;
  listen_.ms = listen_ms;
  watch_.ms = watch_ms;
  result_ms_ = int(std::lround(listen_ms - watch_ms));
  step_ = Step::kResult;
  beat0_ns_ = int64_t(SDL_GetTicksNS()) + 500'000'000;
  sound_on_ = true;
}

bool Calibration::OpenAudio() {
  if (stream_) return true;
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
  // Small device buffer: less (and steadier) delay between writing a click and hearing it.
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");
  SDL_AudioSpec spec{SDL_AUDIO_F32, 1, kRate};
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, AudioCallback, this);
  if (!stream_) return false;
  SDL_AudioSpec dev_spec;
  if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &dev_spec, &device_frames_)) {
    device_frames_ = 0;
  } else if (dev_spec.freq > 0) {
    device_frames_ = int(int64_t(device_frames_) * kRate / dev_spec.freq);  // in our samples
  }
  SDL_ResumeAudioStreamDevice(stream_);
  return true;
}

void Calibration::CloseAudio() {
  if (stream_) {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }
}

void SDLCALL Calibration::AudioCallback(void* user, SDL_AudioStream* stream, int additional, int) {
  static_cast<Calibration*>(user)->FillAudio(stream, additional);
}

void Calibration::FillAudio(SDL_AudioStream* stream, int additional_bytes) {
  const int frames = additional_bytes / int(sizeof(float));
  if (frames <= 0) return;
  std::vector<float> buf(size_t(frames), 0.0f);
  // When the first of these samples reaches the speakers: after what is already queued
  // and one device buffer. (The OS mixer adds a little more; the game's sound goes
  // through the same path, so it cancels out in the result.)
  const int queued = SDL_GetAudioStreamQueued(stream) / int(sizeof(float));
  const int64_t heard0 =
      int64_t(SDL_GetTicksNS()) + (int64_t(queued) + device_frames_) * 1'000'000'000 / kRate;
  const bool on = sound_on_.load();
  const int64_t first = beat0_ns_.load() + sound_shift_ns_.load();
  static int64_t last_first = 0;
  if (first != last_first) {
    last_first = first;
    next_click_ns_ = 0;
  }
  for (int i = 0; i < frames; ++i) {
    const int64_t t = heard0 + int64_t(i) * 1'000'000'000 / kRate;
    if (on) {
      if (next_click_ns_ == 0 || next_click_ns_ < t - kPeriodNs) {
        // Next click at or after t (the clicks start at `first`).
        const int64_t k = t <= first ? 0 : (t - first + kPeriodNs - 1) / kPeriodNs;
        next_click_ns_ = first + k * kPeriodNs;
      }
      if (t >= next_click_ns_) {
        click_pos_ = 0;
        next_click_ns_ += kPeriodNs;
      }
    }
    if (click_pos_ >= 0) {
      buf[size_t(i)] = ClickSample(click_pos_);
      if (++click_pos_ > kRate / 20) click_pos_ = -1;
    }
  }
  SDL_PutAudioStreamData(stream, buf.data(), frames * int(sizeof(float)));
}

void Calibration::StartTest(Step step) {
  step_ = step;
  taps_.clear();
  const int64_t now = int64_t(SDL_GetTicksNS());
  const int64_t beat0 = now + 800'000'000;  // a moment to get ready
  beat0_ns_ = beat0;
  start_ns_ = beat0 + kCountIn * kPeriodNs;
  end_ns_ = start_ns_ + kBeats * kPeriodNs;
  sound_shift_ns_ = 0;
  sound_on_ = step == Step::kListen;
  // Keep a gamepad / keyboard tap from also pressing a launcher button.
  ImGuiIO& io = ImGui::GetIO();
  if (!restore_nav_) {
    nav_flags_ = io.ConfigFlags;
    restore_nav_ = true;
  }
  io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
  ImGui::SetWindowFocus(nullptr);
}

void Calibration::StopTest() {
  sound_on_ = false;
  start_ns_ = end_ns_ = 0;
  if (restore_nav_) {
    ImGui::GetIO().ConfigFlags = nav_flags_;
    restore_nav_ = false;
  }
}

void Calibration::OnEvent(const SDL_Event& e) {
  if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
    SDL_OpenGamepad(e.gdevice.which);  // so its buttons send events
    return;
  }
  if (step_ == Step::kClosed) return;
  if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
    StopTest();
    CloseAudio();
    step_ = Step::kClosed;
    return;
  }
  if (start_ns_ == 0) return;  // no test running
  bool tap = false;
  if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) tap = true;
  if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) tap = true;
  if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !mouse_over_item_) tap = true;
  if (tap) taps_.push_back(int64_t(e.common.timestamp));
}

Calibration::Measure Calibration::Analyse() const {
  Measure m;
  // Keep the raw taps (ms from the nearest beat) for checking: calibration_log.csv.
  if (std::FILE* f = std::fopen("calibration_log.csv", "a")) {
    std::fprintf(f, "%s,device_frames=%d", step_ == Step::kListen ? "listen" : "watch",
                 device_frames_);
    for (int64_t t : taps_) {
      const int64_t first = beat0_ns_.load();
      const int64_t k = (t - first + kPeriodNs / 2) / kPeriodNs;
      std::fprintf(f, ",%.1f", (t - (first + k * kPeriodNs)) / 1e6);
    }
    std::fputc('\n', f);
    std::fclose(f);
  }
  std::vector<double> err;
  for (int64_t t : taps_) {
    if (t < start_ns_ - kPeriodNs / 2 || t > end_ns_ + kPeriodNs / 2) continue;
    const int64_t first = beat0_ns_.load();
    const int64_t k = (t - first + kPeriodNs / 2) / kPeriodNs;
    const int64_t e = t - (first + k * kPeriodNs);
    if (std::llabs(e) <= kMaxErrorNs) err.push_back(e / 1e6);
  }
  m.taps = int(err.size());
  if (m.taps < kMinTaps) return m;
  m.ms = Median(err);
  std::vector<double> dev;
  for (double x : err) dev.push_back(std::fabs(x - m.ms));
  m.spread = Median(dev);
  m.ok = m.spread <= kMaxSpreadMs;
  return m;
}

double Calibration::BallHeight(int64_t shown_ns, int64_t shift_ns) const {
  const int64_t rel = shown_ns - (beat0_ns_.load() + shift_ns);
  if (rel < -kPeriodNs) return 0.0;  // resting before the first bounce
  double u = double(((rel % kPeriodNs) + kPeriodNs) % kPeriodNs) / double(kPeriodNs);
  return 4.0 * u * (1.0 - u);  // parabola: ground at every beat
}

bool Calibration::Draw(int& fine_ms, float refresh_hz) {
  bool applied = false;
  const int64_t now = int64_t(SDL_GetTicksNS());
  // The frame being drawn reaches the screen about one and a half refreshes from now.
  const double hz = refresh_hz > 20.0f ? refresh_hz : 60.0;
  const int64_t shown = now + int64_t(1.5e9 / hz);
  const float w = ImGui::GetContentRegionAvail().x;
  const float line = ImGui::GetTextLineHeightWithSpacing();
  const ImVec4 warn(1.0f, 0.55f, 0.35f, 1.0f);

  auto big = [](const char* text) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
  };
  auto draw_ball = [&](int64_t shift_ns, bool moving) {
    const float h = line * 9.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = line * 0.75f;
    const float ground = p.y + h - line * 0.5f;
    const float x = p.x + w * 0.5f;
    const double height = moving ? BallHeight(shown, shift_ns) : 0.0;
    const float y = ground - r - float(height) * (h - 2.5f * r - line * 0.5f);
    dl->AddLine(ImVec2(p.x + w * 0.2f, ground), ImVec2(p.x + w * 0.8f, ground),
                IM_COL32(150, 150, 170, 255), 3.0f);
    // Squash a little when touching the ground.
    const float squash = height < 0.02 ? 1.25f : 1.0f;
    dl->AddEllipseFilled(ImVec2(x, y + (squash - 1.0f) * r * 0.5f), ImVec2(r * squash, r / squash),
                         IM_COL32(235, 60, 130, 255));
    ImGui::Dummy(ImVec2(w, h));
  };
  auto progress = [&] {
    if (now < start_ns_) {
      const int beats_left = int((start_ns_ - now + kPeriodNs - 1) / kPeriodNs);
      ImGui::Text("Get ready... %d", std::max(1, std::min(beats_left, kCountIn)));
    } else {
      ImGui::ProgressBar(float(double(now - start_ns_) / double(end_ns_ - start_ns_)),
                         ImVec2(w, 0), "");
    }
  };

  switch (step_) {
    case Step::kClosed:
      return false;

    case Step::kIntro:
      big("Beat calibration");
      ImGui::TextWrapped(
          "Measures how much later this PC's sound reaches you than its picture, so the "
          "beat bar and vibration land on the music.");
      ImGui::Spacing();
      ImGui::TextWrapped(
          "1. Listen: tap along to a click (any key, mouse button or controller button).\n"
          "2. Watch: tap each time a silent ball hits the ground.\n"
          "Use the speakers/headphones and screen you play on. About 15 seconds each.");
      if (!stream_) ImGui::TextColored(warn, "No sound device: %s", SDL_GetError());
      ImGui::Spacing();
      ImGui::BeginDisabled(!stream_);
      if (ImGui::Button("Start: Listen", ImVec2(w * 0.5f, line * 1.8f))) StartTest(Step::kListen);
      ImGui::EndDisabled();
      break;

    case Step::kListen:
    case Step::kWatch: {
      const bool listen = step_ == Step::kListen;
      if (start_ns_ == 0) {
        // Between tests (or after a failed one).
        const Measure& m = listen ? listen_ : watch_;
        big(listen ? "1. Listen" : "2. Watch");
        if (m.taps > 0 && !m.ok) {
          ImGui::TextColored(warn,
                             m.taps < kMinTaps ? "Not enough taps on the beat - try again."
                                               : "The taps were too uneven - try again.");
        }
        ImGui::TextWrapped(listen ? "Tap along to the click. There is nothing to watch."
                                  : "Tap each time the ball hits the ground. There is no sound.");
        if (ImGui::Button("Start", ImVec2(w * 0.5f, line * 1.8f))) StartTest(step_);
        if (!listen) draw_ball(0, false);
        break;
      }
      big(listen ? "1. Listen - tap along to the click" : "2. Watch - tap when the ball lands");
      progress();
      ImGui::Text("Taps: %d", int(taps_.size()));
      if (!listen) draw_ball(0, true);
      if (now > end_ns_ + kPeriodNs / 2) {
        Measure m = Analyse();
        StopTest();
        if (listen) {
          listen_ = m;
          // Passed: go on to Watch (waits for Start). Failed: offer Listen again.
          if (m.ok) step_ = Step::kWatch;
        } else {
          watch_ = m;
          if (m.ok) {
            result_ms_ = int(std::lround(listen_.ms - watch_.ms));
            step_ = Step::kResult;
            // Check: click and ball together, the ball delayed by the result like the game's bar.
            beat0_ns_ = int64_t(SDL_GetTicksNS()) + 500'000'000;
            sound_shift_ns_ = 0;
            sound_on_ = true;
          }
        }
      }
      break;
    }

    case Step::kResult: {
      big("Result");
      ImGui::Text("Taps on the click:  %+.0f ms (spread %.0f)", listen_.ms, listen_.spread);
      ImGui::Text("Taps on the ball:   %+.0f ms (spread %.0f)", watch_.ms, watch_.spread);
      ImGui::Spacing();
      ImGui::TextWrapped(result_ms_ >= 0 ? "Your sound reaches you %d ms after the picture."
                                         : "Your sound reaches you %d ms before the picture.",
                         std::abs(result_ms_));
      const int clamped = std::clamp(result_ms_, -150, 150);
      ImGui::Text("Beat fine-tune: %+d ms", clamped);
      ImGui::TextDisabled("Check: the ball should land exactly on each click.");
      draw_ball(int64_t(clamped) * 1'000'000, true);
      if (ImGui::Button("Apply", ImVec2(w * 0.3f, line * 1.8f))) {
        fine_ms = clamped;
        applied = true;
        sound_on_ = false;
        CloseAudio();
        step_ = Step::kClosed;
      }
      ImGui::SameLine();
      if (ImGui::Button("Redo", ImVec2(w * 0.3f, line * 1.8f))) {
        sound_on_ = false;
        Open();
      }
      break;
    }
  }

  if (step_ != Step::kClosed) {
    ImGui::Spacing();
    if (ImGui::Button("Close (Esc)")) {
      StopTest();
      sound_on_ = false;
      CloseAudio();
      step_ = Step::kClosed;
    }
  }
  mouse_over_item_ = ImGui::IsAnyItemHovered();
  return applied;
}
