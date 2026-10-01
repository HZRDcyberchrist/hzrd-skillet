// Audio into the signal path: picking an input, capturing it, and turning it
// into what the engine reads (one sample per line, plus `level` and `hit`
// envelopes). The analysis is a port of videoskillet's
// src/core/signal/audiostate.ts (MIT, Colin Diesh); the capture is new.
//
// Windows: WASAPI. "Computer audio" listens to whatever is playing through
// the default output (loopback); every output device can be listened to the
// same way, and every input (mic, line-in, interface, virtual cable) directly.
// Elsewhere there is one synthetic "Test kick" input, for the test harness.
#pragma once
#include <atomic>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace hzrdaudio {

constexpr int LINES = 525;      // one sample per scan line
constexpr int WINDOW = 2048;    // the analysis window (audiostate.ts ANALYSIS_FFT)

enum class Focus { FullMix, Kick, Bass, Mids, Highs, Custom, Count };
constexpr const char* FOCUS_NAMES[] = {"Full mix", "Kick (35-150 Hz)", "Bass (30-250 Hz)",
                                      "Mids (250-2000 Hz)", "Highs (2000-16000 Hz)", "Custom band"};
struct Settings {
  Focus focus = Focus::FullMix;
  double threshold = 0;  // 0..1, normalized RMS; zero keeps the original response
  double lowHz = 35, highHz = 250;
};
struct Band { double low, high; };
Band focusBand(const Settings& settings, double sampleRate);
double frequencyFromControl(double value);
double frequencyToControl(double hz);
constexpr int SCOPE_SAMPLES = 512, SPECTRUM_BINS = 64;

struct Device {
  std::string key;   // stable id: "default-out", "out:<endpoint>", "in:<endpoint>", "test"
  std::string name;  // what the dropdown shows
};

// The inputs on this machine. Refreshed in the background while anything is
// listening for them; `version` changes whenever the list does.
std::vector<Device> devices(unsigned& version);
// The same, but reads the list right away if it hasn't been read yet (so a
// saved selection can be restored when an instance is created).
std::vector<Device> devicesNow(unsigned& version);

struct Capture;

// One instance's hold on an input. Instances listening to the same device
// share one capture; it stops when the last of them lets go.
class Listener {
 public:
  Listener();
  ~Listener();
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;

  // "" stops listening. Cheap to call every frame with the same key.
  void select(const std::string& key);
  // The most recent WINDOW samples (mono). False when nothing is selected.
  bool latest(float* out, double& sampleRate);
  // Why the selected input isn't delivering, or "".
  std::string error() const;

 private:
  std::string key_;
  std::shared_ptr<Capture> cap_;
};

// audiostate.ts AudioState.update, for one window per frame.
class Analyzer {
 public:
  Analyzer();
  // gain: input trim. Fills lines(), level, hit.
  void update(const float* window, double sampleRate, double gain, const Settings& settings = {});
  void reset();
  const float* lines() const { return lines_; }
  double level = 0, hit = 0;
  double detectedLevel = 0;  // before the threshold gate, also used by the scope
  std::array<float, SCOPE_SAMPLES> waveform{}, focusedWaveform{};
  std::array<float, SPECTRUM_BINS> spectrum{};

 private:
  double lowEnergy(const float* window, double sampleRate);
  void transform(bool inverse);
  void filter(const float* window, double sampleRate, Band band);
  double bandEnergy(Band band, double sampleRate) const;
  float filtered_[WINDOW] = {};
  Settings lastSettings_;
  double lastSampleRate_ = 0, gate_ = 1;
  float lines_[LINES] = {};
  double peak_, lowPrev_ = 0, hitRef_;
  std::vector<double> smooth_;  // the analyser's smoothed magnitudes
  std::vector<double> re_, im_, blackman_;
};

}  // namespace hzrdaudio
