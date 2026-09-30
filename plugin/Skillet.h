// Skillet — the videoskillet NTSC signal path as a Resolume FFGL effect.
//
// videoskillet (c) 2026 Colin Diesh, MIT License — https://github.com/cmdcolin/videoskillet
// FFGL SDK (c) Resolume, BSD — https://github.com/resolume/ffgl
//
// Every parameter here is an ordinary FFGL parameter, so Resolume can map any
// of them to MIDI (right-click > Edit MIDI shortcut). The design for playing
// presets from a controller:
//   - Preset: the whole catalogue as one dropdown (a knob or fader scrolls it)
//   - Pad 1-16: sixteen buttons, each firing the preset chosen in its slot
//     (map these to the pads of a controller)
//   - Prev / Next / Random: buttons
//   - Morph: how long a preset change glides, 0 = cut
//   - Amount and the performance knobs: live controls over whatever preset is up
#pragma once
#include <FFGLSDK.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace skillet {
class Engine;
}

class Skillet : public CFFGLPlugin {
 public:
  Skillet();
  ~Skillet() override;

  FFResult InitGL(const FFGLViewportStruct* vp) override;
  FFResult ProcessOpenGL(ProcessOpenGLStruct* pGL) override;
  FFResult DeInitGL() override;

  FFResult SetFloatParameter(unsigned int index, float value) override;
  float GetFloatParameter(unsigned int index) override;
  FFResult SetTextParameter(unsigned int index, const char* value) override;
  char* GetTextParameter(unsigned int index) override;
  char* GetParameterDisplay(unsigned int index) override;

 private:
  double nowMs() const;
  void loadPreset(int index, bool cut);
  void applyOverlay(void* controls) const;
  int basePreset() const { return pendingPreset_ >= 0 ? pendingPreset_ : currentPreset_; }
  void syncFavorites(bool force);
  void syncFavoriteValue();

  std::unique_ptr<skillet::Engine> engine_;
  std::chrono::steady_clock::time_point t0_;
  float values_[128] = {};
  std::string caption_ = "VIDEO SKILLET";
  std::string display_;
  std::string status_ = "Starting...";
  int currentPreset_ = 0;
  int pendingPreset_ = -1;
  bool pendingCut_ = false;
  bool pendingReset_ = false;
  unsigned rng_ = 0x2545F491u;
  unsigned nextRandom();
  std::vector<int> favs_;       // this instance's copy of the shared favorites
  unsigned favsVersion_ = 0;    // which revision of the shared list favs_ is
};
