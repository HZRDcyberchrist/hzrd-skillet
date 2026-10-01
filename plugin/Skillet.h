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

#include "audio.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace skillet {
class Engine;
struct InputFrame;
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
  bool pictureFor(int kind, int slot, const skillet::InputFrame& layer, const skillet::InputFrame& below,
                  skillet::InputFrame& out);

  std::unique_ptr<skillet::Engine> engine_;
  std::chrono::steady_clock::time_point t0_;
  // Parameter values as the host set them (clamped to their ranges), and the
  // copy the render thread works from for one frame. stateMutex_ covers
  // values_, the preset/favorites state and the element lists: Resolume can
  // set parameters from its interface thread while a frame renders.
  float values_[128] = {};
  float frame_[128] = {};
  float lo_[128] = {}, hi_[128] = {};
  std::mutex stateMutex_;
  // Resolume can set text from its interface thread; the render thread picks
  // it up at the next frame, so the engine is only touched with GL current.
  std::string caption_ = "VIDEO SKILLET";
  std::mutex captionMutex_;
  bool captionDirty_ = false;
  std::string captionShown_;  // what GetTextParameter hands the host
  std::string display_;
  std::string status_ = "Starting...";
  int currentPreset_ = 0;
  int pendingPreset_ = -1;
  bool pendingCut_ = false;
  bool pendingReset_ = false;
  unsigned rng_ = 0x2545F491u;
  unsigned nextRandom();
  void setRange(unsigned p, float lo, float hi);
  int indexOf(unsigned p, int n) const;
  // audio in: the dropdown's keys (entry i+1), the one chosen, and what the
  // render thread does with it
  void syncAudioDevices(bool force);
  void updateAudio();
  std::vector<std::string> audioKeys_;
  std::string audioKey_, audioName_;
  unsigned audioDevVersion_ = 0;
  hzrdaudio::Listener listener_;
  hzrdaudio::Analyzer analyzer_;
  float audioWindow_[hzrdaudio::WINDOW] = {};
  mutable double presetAudioGain_ = 1;  // the preset's own input trim, from the last frame
  double meterAt_ = -1e9;
  std::string meter_ = "Off";
  std::mutex meterMutex_;
  std::string meterShown_;
  std::vector<int> favs_;       // this instance's copy of the shared favorites
  unsigned favsVersion_ = 0;    // which revision of the shared list favs_ is
};
