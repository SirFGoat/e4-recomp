#pragma once
// Beat calibration screen for the launcher: a 100 BPM click to tap along to, then a
// silent bouncing ball to tap along to. The difference between how late the taps land
// on the sound and on the picture is how much later this PC's speakers are than its
// screen, which is the game's beat fine-tune (e4_beat_offset_ms).

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

class Calibration {
 public:
  ~Calibration();

  void Open();
  // Straight to the result/check screen with the given numbers (for screenshots).
  void Demo(double listen_ms, double watch_ms);
  bool active() const { return step_ != Step::kClosed; }
  // Feed every SDL event (before ImGui sees it) so taps keep their exact timestamps.
  void OnEvent(const SDL_Event& e);
  // Draws the calibration screen in the current ImGui window. Returns true when the
  // player applies the result (written to fine_ms).
  bool Draw(int& fine_ms, float refresh_hz);

 private:
  enum class Step { kClosed, kIntro, kListen, kWatch, kResult };
  struct Measure {
    bool ok = false;
    double ms = 0.0;      // median tap time relative to the beat
    double spread = 0.0;  // median absolute deviation
    int taps = 0;
  };

  void StartTest(Step step);
  void StopTest();
  Measure Analyse() const;
  bool OpenAudio();
  void CloseAudio();
  static void SDLCALL AudioCallback(void* user, SDL_AudioStream* stream, int additional, int total);
  void FillAudio(SDL_AudioStream* stream, int additional_bytes);
  double BallHeight(int64_t shown_ns, int64_t shift_ns) const;  // 0 = on the ground, 1 = top

  Step step_ = Step::kClosed;
  int64_t start_ns_ = 0;  // first beat (after the count-in)
  int64_t end_ns_ = 0;
  std::vector<int64_t> taps_;
  Measure listen_, watch_;
  int result_ms_ = 0;
  bool restore_nav_ = false;
  int nav_flags_ = 0;
  bool mouse_over_item_ = false;

  // Audio (callback thread reads these).
  SDL_AudioStream* stream_ = nullptr;
  int device_frames_ = 0;
  std::atomic<bool> sound_on_{false};
  std::atomic<int64_t> beat0_ns_{0};       // click times are beat0 + k * period (heard)
  std::atomic<int64_t> sound_shift_ns_{0};  // check step: play the clicks this much later
  int64_t next_click_ns_ = 0;               // callback thread only
  int click_pos_ = -1;                       // samples into the current click, -1 = silent
};
