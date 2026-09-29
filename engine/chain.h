// The CPU half of one videoskillet frame: everything Engine.render() and
// Engine.renderFrame() in src/core/gpu/pipeline.ts compute before they record
// GPU work. Kept free of GL so it can be checked against the TypeScript.
#pragma once
#include <cstdint>
#include <string>

#include "signal.h"

namespace skillet {

constexpr int MAX_GENS = 4;

struct FrameEnv {
  double nowMs = 0;       // wall clock for the strobe (and morphs)
  double canvasW = 1920, canvasH = 1080;
  double srcAspect = 4.0 / 3.0;
  double srcMirror = 0, tubeTurn = 0;
  double srcNoise = 0, srcNoiseB = 0;
  bool bEnabled = false;  // whether a source B is patched in
};

// The pass gates the GPU side needs, decided once per frame from the board as
// it stood while the frame was packed (feedgates.ts + pipeline.ts predicates).
struct FrameGates {
  bool aFeed, bChain, bWave, bFeed, composeB, chyron, fbComposite, progSnap;
  bool underDown, enhancer, vir, caption, storePrev, crtSrgb;
  int gens;
};

class SignalChain {
 public:
  explicit SignalChain(uint32_t seed);

  Controls controls;            // the resting board
  Glide glide;
  CaptionState captionState;

  // Advances one display refresh. Returns true when a simulation step ran
  // (outputs below are fresh); false when timeScale held the frame.
  bool step(const FrameEnv& env);

  // outputs of the last simulation step
  uint8_t params[kParamBytes];
  uint8_t genParams[MAX_GENS][kParamBytes];
  uint8_t feedParamsA[kParamBytes];
  uint8_t feedParamsB[kParamBytes];
  float lineParams[MAX_GENS][LINES * 4];
  float filters[NUM_SECTIONS * FILTER_STRIDE];
  bool filtersFresh = false;    // filters[] changed this step; upload them
  FrameGates gates{};
  uint32_t frame = 0;           // frame number of the last step's params

  void markFiltersDirty() { filtersDirty_ = true; }
  void startMorph(const Controls& to, double seconds, double nowMs) { glide.start(controls, to, seconds, nowMs); }

  // A live layer over the resting board for one frame (the host's knobs),
  // applied after the morph walks the board and before the paperclip bites,
  // the place the app applies its modulation bay. Never written back.
  std::function<void(Controls&)> overlay;

 private:
  void renderFrameCpu(const FrameEnv& env, const Controls& c);
  void packFeed(bool isA, const double vals[kNumParams], const Controls& c, const DeckPause& deck, uint8_t out[kParamBytes]);

  Mulberry32 dice_;
  Rand rand_;
  LineState lineState_;
  MixState mixState_;
  RfState rfState_;
  SynthState synthState_;
  StrobeGate strobeGate_;
  ClipContact clip_;
  TrackingServo servo_;
  bool filtersDirty_ = true;
  double lastFilterVals_[5] = {NAN, NAN, NAN, NAN, NAN};
  double scPhase_ = 0, cfbCarrierPhase_ = 0, shuttlePhase_ = 0, lastShuttleX_ = 1;
  ServoOut track_{0.85, 0, 0};
  double tapeA_ = 0, tapeB_ = 0;
  double impulseTrainPos_ = 0, impulseTrainStep_ = 0;
  double simAcc_ = 0;
  double frameCounter_ = 0;
};

// feedgates.ts
bool feedFaults(const Controls& c, bool isA);
bool aFeedOn(const Controls& c);
bool bWaveOn(const Controls& c, bool bEnabled);
bool bFeedOn(const Controls& c, bool bEnabled);
bool bOn(const Controls& c, bool bEnabled);

} // namespace skillet
