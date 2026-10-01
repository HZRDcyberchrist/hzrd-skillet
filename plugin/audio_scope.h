#pragma once
#include "audio.h"
#include <functional>

namespace hzrdaudio {
enum class ScopeEdit { Focus, Threshold, LowHz, HighHz };
struct ScopeFrame {
  Settings settings;
  std::array<float, SCOPE_SAMPLES> waveform{}, focusedWaveform{};
  std::array<float, SPECTRUM_BINS> spectrum{};
  double level = 0, hit = 0, sampleRate = 48000;
  bool connected = false;
  std::string source = "Choose an Audio input in Resolume";
};

// A per-instance, nonmodal tuning window. It never touches GL or audio
// capture: the render thread publishes a snapshot; edits go through the
// same saved FFGL parameters as Resolume's controls.
class Scope {
 public:
  explicit Scope(std::function<void(ScopeEdit, double)> edit);
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
  void show();
  void publish(const ScopeFrame& frame);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace hzrdaudio
