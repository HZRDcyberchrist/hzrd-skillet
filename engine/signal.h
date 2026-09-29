// CPU-side per-frame signal state, ported from videoskillet src/core/signal/
// and src/core/gpu/{uniforms,filterbank}.ts (MIT, Colin Diesh).
//
// Everything here is double precision on purpose: the TypeScript runs on JS
// numbers, and several of these accumulate across thousands of frames (tape
// time, subcarrier phase walks), so the port keeps the same arithmetic.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "generated/tables.gen.h"

namespace skillet {

// ── raster constants (signal/constants.ts) ──
constexpr double kPI = 3.14159265358979323846;
constexpr double FSC = 315e6 / 88;
constexpr double F_H = 4500000.0 / 286;
constexpr double SAMPLE_RATE = 4 * FSC;
constexpr int SAMPLES_PER_LINE = 910;
constexpr int LINES = 525;
constexpr int N_SAMPLES = SAMPLES_PER_LINE * LINES;
constexpr int ACTIVE_START = 134;
constexpr int ACTIVE_WIDTH = 754;
constexpr int ACTIVE_TOP = 22;
constexpr int ACTIVE_HEIGHT = 480;
constexpr int HEAD_SWITCH_LINE = ACTIVE_TOP + ACTIVE_HEIGHT - 8;
constexpr double IRE_VIDEO_RANGE = 100 - 7.5;
inline double usToSamples(double us) { return us * 1e-6 * SAMPLE_RATE; }

// ── JS number semantics the TypeScript leans on ──
int32_t toInt32(double d);
inline uint32_t toUint32(double d) { return static_cast<uint32_t>(toInt32(d)); }
inline int32_t imul(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)); }
// Math.max/Math.min: NaN wins, and +0 beats -0 for max (-0 beats +0 for min).
inline double jmax(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) return NAN;
  if (a == b) return std::signbit(a) ? b : a;
  return a > b ? a : b;
}
inline double jmin(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) return NAN;
  if (a == b) return std::signbit(a) ? a : b;
  return a < b ? a : b;
}
inline double jsRound(double x) { return std::floor(x + 0.5); }
inline double jsMod(double a, double b) { return std::fmod(a, b); }
inline double clampd(double v, double lo, double hi) { return jmin(hi, jmax(lo, v)); }
inline double clamp01(double v) { return clampd(v, 0, 1); }
inline double wrap(double x, double m) { return std::fmod(std::fmod(x, m) + m, m); }

using Rand = std::function<double()>;

// mulberry32 (core/rng.ts rngFor): the engine's dice.
class Mulberry32 {
 public:
  explicit Mulberry32(uint32_t seed) : a_(seed) {}
  double operator()() {
    a_ = a_ + 0x6d2b79f5u;
    uint32_t t = a_;
    t = static_cast<uint32_t>(imul(static_cast<int32_t>(t ^ (t >> 15)), static_cast<int32_t>(t | 1)));
    t ^= t + static_cast<uint32_t>(imul(static_cast<int32_t>(t ^ (t >> 7)), static_cast<int32_t>(t | 61)));
    return static_cast<double>(t ^ (t >> 14)) / 4294967296.0;
  }

 private:
  uint32_t a_;
};

// ── controls ──
struct Controls {
  std::array<double, kNumControls> v;
  Controls() { for (int i = 0; i < kNumControls; i++) v[i] = kControlDefaults[i]; }
  double& operator[](int k) { return v[k]; }
  double operator[](int k) const { return v[k]; }
};

// ── noise.ts ──
double valueNoise(double t, double seed = 0);

class StickSlip {
 public:
  explicit StickSlip(Rand* r);
  double step();

 private:
  double nextGrip();
  Rand* rand_;
  double x_ = 0, v_ = 0;
  bool stuck_ = true;
  double grip_;
};

class Wow {
 public:
  explicit Wow(Rand* r) : rand_(r) {}
  void advance(double dt);
  double at(double t, double rowFrac) const;

 private:
  Rand* rand_;
  const double rates_[3] = {0.6, 1.07, 1.73};
  const double spreads_[3] = {0.9, 1.6, 2.6};
  double amps_[3] = {0.8, 0.5, 0.35};
  double ampTarget_[3] = {0.8, 0.5, 0.35};
  double reelRate_ = 0.31;
  double reelPhase_ = 0;
};

// ── crossings.ts ──
double advanceCrossings(double phase, double bars);

// ── linestate.ts ──
struct LineStateControls {
  double tbJitterNs, tbWowNs, tbStickNs, underJitterDeg, headSwitchShiftUs;
  double trackAmt, trackPos, shuttleBars, shuttlePhase;
};

class LineState {
 public:
  explicit LineState(Rand* r) : rand_(r) {}
  const float* update(const LineStateControls& c, double frame);
  float data[LINES * 4] = {};

 private:
  Wow& wowFor(int gen);
  StickSlip& slipFor(int gen);
  Rand* rand_;
  double flutter_ = 0, underWalk_ = 0, t_ = 0, lastFrame_ = -1;
  int gen_ = 0;
  std::vector<Wow> wows_;
  std::vector<StickSlip> slips_;
};

// ── servo.ts ──
struct ServoOut { double pos, amt, flagUs; };
class TrackingServo {
 public:
  explicit TrackingServo(Rand* r) : rand_(r) {}
  void kick(double size) { pending_ += size; }
  ServoOut update(double target, double amt, double hunt, double kick);

 private:
  Rand* rand_;
  double pos_ = 0.85, vel_ = 0, stretch_ = 0, tension_ = 0, pending_ = 0;
};

// ── mixstate.ts ──
struct DeckPause { double pause, shift, bar, row; };

class PauseDeck {
 public:
  PauseDeck(double bar0, double chanBar, double chanKick, Rand* r) : wow_(r), bar_(bar0), chanBar_(chanBar), chanKick_(chanKick) {}
  DeckPause update(double pause);
  double t = 0;

 private:
  Wow wow_;
  double bar_;
  double chanBar_, chanKick_;
};

struct MixUniforms {
  double bShift0, bShiftLine, bPhase0, bPhaseLine, bRowOff, wipePos;
  DeckPause a, b;
};

class MixState {
 public:
  explicit MixState(Rand* r) : deckA_(0.4 * LINES, 5, 29, r), deckB_(0.75 * LINES, 7, 23, r) {}
  MixUniforms update(double aPause, double bLineHz, double bDetuneHz, double bRollLps, double bPause, double wipePos, double wipeRateHz);

 private:
  double hShift_ = 0, scPhase_ = 0, vRoll_ = 0, wipeT_ = 0, wipeLever_ = 0;
  PauseDeck deckA_, deckB_;
  int parity_ = 0;
};

// ── rfstate.ts ──
struct RfUniforms { double rfAdjEps, rfAdjTau, rfAdjPhase, rfAdjPhaseS, ingressCps, ingressRowCyc, ingressPhase, ingressKey; };
class RfState {
 public:
  RfUniforms update(double frame);

 private:
  double tau_ = 0, phV_ = 0, phS_ = 0, phI_ = 0;
};

// ── synthstate.ts ──
struct SynthUniforms { double phaseA, perLineA, perSampleA, phaseB, perLineB, perSampleB; };
class SynthState {
 public:
  SynthUniforms update(double aHz, double bHz);

 private:
  double phaseA_ = 0, phaseB_ = 0;
};

// ── captionstate.ts ──
constexpr int CC_CR = 0x0d;
class CaptionState {
 public:
  void setText(const std::string& text);
  const std::string& text() const { return raw_; }
  std::pair<uint32_t, uint32_t> update(double vbi);

 private:
  uint32_t next();
  std::string raw_;
  std::vector<uint32_t> codes_;
  size_t at_ = 0;
  int hold_ = 0;
  double carry_ = 0;
};

// ── pulsegate.ts / strobe.ts ──
class PulseGate {
 public:
  bool open(double hz, double ms, double nowMs);

 private:
  double cycle_ = -1;
};
class StrobeGate {
 public:
  double step(double hz, double ms, double nowMs) { return gate_.open(hz, ms, nowMs) ? 0 : 1; }

 private:
  PulseGate gate_;
};

// ── clip.ts ──
struct ClipStep { int point; double depth; };
class ClipContact {
 public:
  // Returns false when the clip is fully off (no step).
  bool step(double hz, double bite, double dwellMs, double chatter, int point, Rand& rand, ClipStep& out);

 private:
  void slew(double target);
  double untilBite_ = 0, downFor_ = 0, level_ = 0;
  bool bouncing_ = false;
};
int clipPointAt(double i);

// ── glide.ts (morph) ──
class Glide {
 public:
  bool running() const { return running_; }
  // seconds <= 0 cuts
  void start(const Controls& from, const Controls& to, double seconds, double nowMs);
  void stop() { running_ = false; }
  // Walks `controls` toward the target. Returns true if a filter key moved.
  bool apply(Controls& controls, double nowMs, bool& done);
  const Controls& target() const { return to_; }

 private:
  double at(int k, double e) const;
  bool running_ = false;
  Controls from_, to_;
  double seconds_ = 0, startMs_ = 0;
  int notch_ = -1;
  std::vector<int> travel_, coarse_, switching_;
  std::vector<int> travelIdx_; // per key: index into kTravelTable or -1
  std::array<double, kNumControls> ta_{}, tb_{};
};

// ── filters.ts + filterbank.ts ──
constexpr int FILTER_STRIDE = 64;
constexpr int NUM_SECTIONS = 5;
void designFilterBank(const Controls& c, float out[NUM_SECTIONS * FILTER_STRIDE]);

// ── uniforms.ts ──
struct UniformEnv {
  double frame, canvasW, canvasH, srcAspect, srcMirror, tubeTurn, srcNoise, srcNoiseB, srcFrame, beamBlank;
  double scPhase, cfbCarrierPhase, audioHit, audioLevel, impulseTrainPos, impulseTrainStep, shuttlePhase;
  double trackPos, trackAmt, flagUs, dbgView;
};
double loRadPerSample(double detuneKHz);
double impulseStorm(double t);
void uniformValues(const Controls& c, const UniformEnv& env, double p[kNumParams]);
void packParams(const double p[kNumParams], uint8_t out[kParamBytes]);

} // namespace skillet
