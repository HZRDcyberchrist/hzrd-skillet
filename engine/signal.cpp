// Port of videoskillet src/core/signal/*.ts, filterbank.ts and uniforms.ts
// (MIT, Colin Diesh). Kept line-for-line close to the TypeScript so the two
// can be diffed; tools/golden checks the outputs against the originals.
#include "signal.h"

#include <algorithm>
#include <cstring>

namespace skillet {

int32_t toInt32(double d) {
  if (!std::isfinite(d)) return 0;
  double t = std::trunc(d);
  double m = std::fmod(t, 4294967296.0);
  if (m < 0) m += 4294967296.0;
  return static_cast<int32_t>(static_cast<uint32_t>(m));
}

// ───────────── noise.ts ─────────────

static double hashNoise(double i, double seed) {
  int32_t h = imul(toInt32(i), 0x27d4eb2d) ^ imul(toInt32(seed), 0x165667b1);
  h = imul(static_cast<int32_t>(static_cast<uint32_t>(h) ^ (static_cast<uint32_t>(h) >> 15)), static_cast<int32_t>(0x85ebca6bu));
  h = static_cast<int32_t>(static_cast<uint32_t>(h) ^ (static_cast<uint32_t>(h) >> 13));
  return static_cast<double>(static_cast<uint32_t>(h)) / 2147483648.0 - 1;
}

double valueNoise(double t, double seed) {
  double i = std::floor(t);
  double f = t - i;
  double u = f * f * (3 - 2 * f);
  return hashNoise(i, seed) * (1 - u) + hashNoise(i + 1, seed) * u;
}

StickSlip::StickSlip(Rand* r) : rand_(r) { grip_ = nextGrip(); }

double StickSlip::nextGrip() {
  double r = (*rand_)();
  return 0.25 + 1.1 * r * r;
}

double StickSlip::step() {
  if (stuck_) {
    x_ += 0.008;
    if (x_ > grip_) stuck_ = false;
  } else {
    v_ = (v_ - 0.3 * x_) * 0.75;
    x_ += v_;
    if (std::abs(v_) < 0.02) {
      stuck_ = true;
      grip_ = nextGrip();
    }
  }
  return x_;
}

void Wow::advance(double dt) {
  for (int k = 0; k < 3; k++) {
    if ((*rand_)() < 0.01) ampTarget_[k] = 0.2 + (*rand_)();
    amps_[k] += (ampTarget_[k] - amps_[k]) * 0.02;
  }
  reelRate_ += (0.31 - reelRate_) * 0.01 + ((*rand_)() - 0.5) * 0.004;
  reelPhase_ += 2 * kPI * reelRate_ * dt;
}

double Wow::at(double t, double rowFrac) const {
  double sum = 0.4 * std::sin(reelPhase_ + rowFrac * 0.5);
  double norm = 0.4;
  for (int k = 0; k < 3; k++) {
    sum += amps_[k] * std::sin(2 * kPI * rates_[k] * t + rowFrac * spreads_[k]);
    norm += amps_[k];
  }
  return sum / norm;
}

// ───────────── crossings.ts ─────────────

double advanceCrossings(double phase, double bars) {
  const double PER_CROSSING = 0.0035, SERVO_HUNT = 0.0008, WRAP = 1024;
  return bars == 0 ? phase : jsMod(phase + bars * PER_CROSSING + SERVO_HUNT, WRAP);
}

// ───────────── linestate.ts ─────────────

static const double F_UNDER = (40 * FSC) / 227.5;
static const double F_DOWN = FSC - F_UNDER;
static const double DOWN_PER_SAMPLE = F_DOWN / SAMPLE_RATE;
static const double STRIP_OFFSET = usToSamples(2.5);
static const double BAR_HOOK = usToSamples(4);

static double hash01(double v) {
  int32_t h = imul(toInt32(v) ^ static_cast<int32_t>(0x9e3779b9u), static_cast<int32_t>(0x85ebca6bu));
  h = imul(static_cast<int32_t>(static_cast<uint32_t>(h) ^ (static_cast<uint32_t>(h) >> 13)), static_cast<int32_t>(0xc2b2ae35u));
  return static_cast<double>(static_cast<uint32_t>(h) ^ (static_cast<uint32_t>(h) >> 16)) / 4294967296.0;
}

Wow& LineState::wowFor(int gen) {
  while (static_cast<int>(wows_.size()) <= gen) wows_.emplace_back(rand_);
  return wows_[gen];
}

StickSlip& LineState::slipFor(int gen) {
  while (static_cast<int>(slips_.size()) <= gen) slips_.emplace_back(rand_);
  return slips_[gen];
}

const float* LineState::update(const LineStateControls& c, double frame) {
  if (frame == lastFrame_) {
    gen_ += 1;
  } else {
    lastFrame_ = frame;
    gen_ = 0;
    t_ += 1.0 / 60;
  }
  Wow& wow = wowFor(gen_);
  wow.advance(1.0 / 60);
  StickSlip* slip = c.tbStickNs > 0 ? &slipFor(gen_) : nullptr;
  const double stickAmp = usToSamples(c.tbStickNs * 1e-3);
  const double flutterAmp = usToSamples(c.tbJitterNs * 1e-3);
  const double wowAmp = usToSamples(c.tbWowNs * 1e-3);
  const double headShift = usToSamples(c.headSwitchShiftUs);
  const double underStep = (c.underJitterDeg * kPI) / 180;
  const double trackCenter = c.trackPos * LINES;
  const double trackHalf = 3 + 18 * c.trackAmt;
  const double trackAmp = usToSamples(6 * c.trackAmt);
  const double bars = std::abs(c.shuttleBars);
  const double frameLine = frame * LINES;
  Rand& rand = *rand_;
  for (int row = 0; row < LINES; row++) {
    const double rowFrac = static_cast<double>(row) / LINES;
    flutter_ += (rand() - 0.5) * flutterAmp * 0.7;
    flutter_ *= 0.995;
    const double wander = wowAmp == 0 ? 0 : wowAmp * wow.at(t_, rowFrac);
    const double stick = slip ? stickAmp * slip->step() : 0;
    const bool headSwitched = row >= HEAD_SWITCH_LINE;
    const double hs = headSwitched ? headShift : 0;
    const double trackDist = std::abs(row - trackCenter);
    const double track = c.trackAmt > 0 && trackDist < trackHalf
                             ? trackAmp * (1 - trackDist / trackHalf) * (0.6 + 0.8 * rand())
                             : 0;
    double shuttle = 0;
    double shuttleHue = 0;
    if (c.shuttleBars != 0) {
      const double x = rowFrac * bars + c.shuttlePhase;
      const double k = std::floor(x);
      const double f = x - k;
      const double dLines = (jmin(f, 1 - f) / bars) * LINES;
      const double half = 8;
      shuttle = STRIP_OFFSET * (hash01(k) - 0.5) + (dLines < half ? BAR_HOOK * (1 - dLines / half) * (0.5 + rand()) : 0);
      shuttleHue = 2.5 * (hash01(static_cast<double>(toInt32(k) ^ 0x3ac1)) - 0.5);
    }
    const double globalSample = (frameLine + row) * SAMPLES_PER_LINE;
    const double base = jsMod(DOWN_PER_SAMPLE * globalSample, 1);
    underWalk_ += (rand() - 0.5) * underStep;
    underWalk_ *= 0.99;
    const int o = row * 4;
    data[o] = static_cast<float>(flutter_ + wander + stick + hs + track + shuttle);
    data[o + 1] = static_cast<float>(base * 2 * kPI);
    data[o + 2] = static_cast<float>(underWalk_ + (headSwitched ? 0.9 : 0) + shuttleHue);
    data[o + 3] = static_cast<float>(rand());
  }
  return data;
}

// ───────────── servo.ts ─────────────

ServoOut TrackingServo::update(double target, double amt, double huntIn, double kick) {
  const double DT = 1.0 / 60;
  const double MAX_VEL = 3;
  const double hunt = clamp01(huntIn);
  tension_ *= 0.94;
  if (hunt == 0) {
    pos_ = target;
    vel_ = 0;
    stretch_ = 0;
    pending_ = 0;
    return {target, amt, 0};
  }
  Rand& rand = *rand_;
  stretch_ += -stretch_ * 0.02 + (rand() - 0.5) * 0.012 * hunt;
  if (rand() < 0.015 * hunt * hunt) pending_ += 0.3 + 0.7 * rand();
  const double shove = pending_ * kick;
  pending_ = 0;
  if (shove > 0) {
    vel_ += shove * (1.5 + 3 * hunt) * (rand() < 0.5 ? -1 : 1);
    tension_ += shove;
  }
  const double err = target + stretch_ - pos_;
  const double dead = 0.01 + 0.03 * hunt;
  const double k = 4 + 20 * hunt;
  const double zeta = 0.9 - 0.75 * hunt;
  const double damp = 2 * std::sqrt(k) * zeta;
  const double acc = (std::abs(err) > dead ? k * err : 0) - damp * vel_;
  vel_ = clampd(vel_ + acc * DT, -MAX_VEL, MAX_VEL);
  pos_ += vel_ * DT;
  if (pos_ < 0 || pos_ > 1) {
    pos_ = clamp01(pos_);
    vel_ = -vel_ * 0.5;
  }
  const double unrest = std::abs(err) * 8 + std::abs(vel_) * 1.2 + tension_ * 0.6;
  return {pos_, jmin(amt + hunt * jmin(unrest, 1), 1), jmin(tension_, 3) * 4 * hunt};
}

// ───────────── mixstate.ts ─────────────

DeckPause PauseDeck::update(double pause) {
  if (pause <= 0) return {0, 0, bar_, 0};
  t += 1.0 / 60;
  wow_.advance(1.0 / 60);
  const double shift = wow_.at(t, 0) * pause * 30;
  bar_ = wrap(bar_ + valueNoise(t * 0.35, chanBar_) * 0.9 * pause, LINES);
  const double k = valueNoise(t * 2.1, chanKick_);
  return {pause, shift, bar_, k > 0.55 ? jsRound(k * 4 * pause) : 0};
}

MixUniforms MixState::update(double aPause, double bLineHz, double bDetuneHz, double bRollLps, double bPause, double wipePos, double wipeRateHz) {
  const double LINE_S = 1 / F_H;
  const double shiftPerLine = (bLineHz / F_H) * SAMPLES_PER_LINE;
  hShift_ = wrap(hShift_ + shiftPerLine * LINES, SAMPLES_PER_LINE);
  scPhase_ = wrap(scPhase_ + bDetuneHz * LINE_S * LINES, 1);
  vRoll_ = wrap(vRoll_ + bRollLps, LINES);
  if (wipeRateHz != 0) {
    wipeT_ = wrap(wipeT_ + (2 * wipeRateHz) / 60, 2);
  } else if (wipePos != wipeLever_) {
    wipeT_ = 0;
  }
  wipeLever_ = wipePos;
  const double wp = wrap(wipePos + wipeT_, 2);
  MixUniforms u;
  u.a = deckA_.update(aPause);
  u.b = deckB_.update(bPause);
  double pausePhase = 0;
  if (bPause > 0) {
    parity_ ^= 1;
    pausePhase = bPause * (parity_ * 1.9 + 1.2 * valueNoise(deckB_.t * 0.8, 11));
  }
  u.wipePos = wp < 1 ? wp : 2 - wp;
  u.bShift0 = wrap(hShift_, SAMPLES_PER_LINE);
  u.bShiftLine = shiftPerLine;
  u.bPhase0 = scPhase_ * 2 * kPI + pausePhase;
  u.bPhaseLine = 2 * kPI * bDetuneHz * LINE_S;
  u.bRowOff = std::floor(vRoll_);
  return u;
}

// ───────────── rfstate.ts ─────────────

RfUniforms RfState::update(double frame) {
  const double N = N_SAMPLES;
  const double TAU = 2 * kPI;
  const double t = frame / 60;
  const double eps = 1.6e-3 * (0.3 + 0.55 * valueNoise(t * 0.17, 21) + 0.15 * valueNoise(t * 0.9, 22));
  tau_ = wrap(tau_ + eps * N, N);
  phV_ = wrap(phV_ + (TAU * 700 * valueNoise(t * 0.31, 23)) / 60, TAU);
  phS_ = wrap(phS_ + (TAU * (160 * valueNoise(t * 0.23, 24) + 45 * valueNoise(t * 2.3, 25))) / 60, TAU);
  const double cps = 0.16 + 0.045 * valueNoise(t * 0.19, 33) + 0.008 * valueNoise(t * 1.7, 34);
  phI_ = wrap(phI_ + TAU * cps * N, TAU);
  const double key = jmin(1, jmax(0, 0.2 + 1.5 * valueNoise(t * 0.34, 31) + 0.7 * valueNoise(t * 1.2, 32)));
  return {eps, tau_, phV_, phS_, cps, SAMPLES_PER_LINE * cps - std::floor(SAMPLES_PER_LINE * cps), phI_, key};
}

// ───────────── synthstate.ts ─────────────

SynthUniforms SynthState::update(double aHz, double bHz) {
  auto fract = [](double x) { return x - std::floor(x); };
  const double FRAME_SAMPLES = N_SAMPLES;
  phaseA_ = fract(phaseA_ + (aHz * FRAME_SAMPLES) / SAMPLE_RATE);
  phaseB_ = fract(phaseB_ + (bHz * FRAME_SAMPLES) / SAMPLE_RATE);
  return {fract(phaseA_), fract((aHz * SAMPLES_PER_LINE) / SAMPLE_RATE), aHz / SAMPLE_RATE,
          fract(phaseB_), fract((bHz * SAMPLES_PER_LINE) / SAMPLE_RATE), bHz / SAMPLE_RATE};
}

// ───────────── captionstate.ts ─────────────

void CaptionState::setText(const std::string& text) {
  if (text == raw_) return;
  raw_ = text;
  codes_.clear();
  if (!text.empty()) {
    // split on '\n'; every line ends with a carriage return
    size_t s = 0;
    while (true) {
      size_t e = text.find('\n', s);
      std::string line = text.substr(s, e == std::string::npos ? std::string::npos : e - s);
      for (unsigned char ch : line) {
        // (the app walks code points; the plugin's text parameter is ASCII)
        codes_.push_back(ch >= 0x20 && ch <= 0x7f ? ch : 0x20);
      }
      codes_.push_back(CC_CR);
      if (e == std::string::npos) break;
      s = e + 1;
    }
  }
  at_ = 0;
  hold_ = 0;
  carry_ = 0;
}

uint32_t CaptionState::next() {
  if (at_ >= codes_.size()) at_ = 0;
  uint32_t code = codes_[at_];
  at_ += 1;
  if (at_ >= codes_.size()) {
    at_ = 0;
    hold_ = static_cast<int>(jsRound(2.5 * 60));
  }
  return code;
}

std::pair<uint32_t, uint32_t> CaptionState::update(double vbi) {
  if (vbi == 0 || codes_.empty()) return {0, 0};
  if (hold_ > 0) {
    hold_ -= 1;
    return {0, 0};
  }
  carry_ += 30.0 / 60;
  if (carry_ < 2) return {0, 0};
  carry_ -= 2;
  uint32_t first = next();
  uint32_t second = hold_ > 0 ? 0 : next();
  return {first, second};
}

// ───────────── pulsegate.ts ─────────────

bool PulseGate::open(double hz, double ms, double nowMs) {
  if (!(hz > 0)) {
    cycle_ = -1;
    return true;
  }
  const double period = 1000 / hz;
  const double width = ms;
  const double cycle = std::floor(nowMs / period);
  if (cycle != cycle_) {
    cycle_ = cycle;
    return true;
  }
  return jsMod(nowMs, period) < width;
}

// ───────────── clip.ts ─────────────

int clipPointAt(double i) {
  double r = jsRound(i);
  if (std::isnan(r)) r = 0;
  return static_cast<int>(clampd(r, 0, kNumClipPoints - 1));
}

void ClipContact::slew(double target) {
  const double DT = 1000.0 / 60;
  const double tau = target > level_ ? 25 : 90;
  level_ += (target - level_) * jmin(1, DT / tau);
}

bool ClipContact::step(double hz, double bite, double dwellMs, double chatter, int point, Rand& rand, ClipStep& out) {
  const double DT = 1000.0 / 60;
  if (hz <= 0) {
    untilBite_ = 0;
    downFor_ = 0;
    if (level_ <= 1e-4) {
      level_ = 0;
      return false;
    }
    slew(0);
  } else {
    if (downFor_ > 0) {
      downFor_ -= DT;
    } else {
      untilBite_ -= DT;
      if (untilBite_ <= 0) {
        untilBite_ = -std::log(jmax(rand(), 1e-6)) * (1000 / hz);
        downFor_ = jmax(dwellMs, DT);
        bouncing_ = false;
      }
    }
    const bool down = downFor_ > 0;
    if (down) bouncing_ = rand() < chatter * 0.55;
    const double target = down && !bouncing_ ? clamp01(bite) : 0;
    slew(target);
  }
  if (level_ <= 1e-4) return false;
  out.point = point;
  out.depth = level_;
  return true;
}

// ───────────── glide.ts ─────────────

static int travelIndexOf(int key) {
  for (int i = 0; i < kTravelCount; i++)
    if (kTravelKeys[i] == key) return i;
  return -1;
}

// toTravel through the tabulated curve: the table is monotonic in travel.
static double tableToTravel(int ti, double v) {
  const float* t = kTravelTable[ti];
  const bool up = t[kTravelN - 1] >= t[0];
  auto before = [&](double a, double b) { return up ? a < b : a > b; };
  if (!before(t[0], v)) return 0;
  if (!before(v, t[kTravelN - 1])) return 1;
  int lo = 0, hi = kTravelN - 1;
  while (hi - lo > 1) {
    int mid = (lo + hi) / 2;
    if (before(t[mid], v) || t[mid] == v) lo = mid; else hi = mid;
  }
  const double a = t[lo], b = t[hi];
  const double f = b == a ? 0 : (v - a) / (b - a);
  return (lo + f) / (kTravelN - 1);
}

static double tableFromTravel(int ti, double x) {
  const float* t = kTravelTable[ti];
  x = clamp01(x) * (kTravelN - 1);
  int i = static_cast<int>(std::floor(x));
  if (i >= kTravelN - 1) return t[kTravelN - 1];
  double f = x - i;
  return t[i] + (t[i + 1] - t[i]) * f;
}

static bool inSet(const int* set, int n, int k) {
  for (int i = 0; i < n; i++)
    if (set[i] == k) return true;
  return false;
}

void Glide::start(const Controls& from, const Controls& to, double seconds, double nowMs) {
  running_ = true;
  from_ = from;
  to_ = to;
  seconds_ = seconds;
  startMs_ = nowMs;
  notch_ = -1;
  travel_.clear();
  coarse_.clear();
  switching_.clear();
  travelIdx_.assign(kNumControls, -1);
  for (int k = 0; k < kNumControls; k++) {
    if (inSet(kViewKeys, kViewKeysCount, k) || from[k] == to[k]) continue;
    if (inSet(kEnumKeys, kEnumKeysCount, k)) switching_.push_back(k);
    else if (inSet(kFilterKeys, kFilterKeysCount, k)) coarse_.push_back(k);
    else travel_.push_back(k);
  }
  for (int k : travel_) {
    int ti = travelIndexOf(k);
    if (ti >= 0) { travelIdx_[k] = ti; ta_[k] = tableToTravel(ti, from[k]); tb_[k] = tableToTravel(ti, to[k]); }
  }
  for (int k : coarse_) {
    int ti = travelIndexOf(k);
    if (ti >= 0) { travelIdx_[k] = ti; ta_[k] = tableToTravel(ti, from[k]); tb_[k] = tableToTravel(ti, to[k]); }
  }
}

double Glide::at(int k, double e) const {
  const int ti = travelIdx_[k];
  if (ti < 0) return from_[k] + (to_[k] - from_[k]) * e;
  return tableFromTravel(ti, ta_[k] + (tb_[k] - ta_[k]) * e);
}

bool Glide::apply(Controls& controls, double nowMs, bool& done) {
  const int COARSE_STEPS = 32;
  done = false;
  if (!running_) {
    done = true;
    return false;
  }
  const double raw = seconds_ <= 0 ? 1 : jmin(1, (nowMs - startMs_) / (seconds_ * 1000));
  if (raw >= 1) {
    for (int k : travel_) controls[k] = to_[k];
    for (int k : coarse_) controls[k] = to_[k];
    for (int k : switching_) controls[k] = to_[k];
    const bool moved = !coarse_.empty() && notch_ != COARSE_STEPS;
    running_ = false;
    done = true;
    return moved;
  }
  const double e = raw * raw * (3 - 2 * raw);
  for (int k : travel_) controls[k] = at(k, e);
  for (int k : switching_) controls[k] = e < 0.5 ? from_[k] : to_[k];
  const int notch = static_cast<int>(jsRound(e * COARSE_STEPS));
  if (notch == notch_) return false;
  notch_ = notch;
  const double ce = static_cast<double>(notch) / COARSE_STEPS;
  for (int k : coarse_) controls[k] = at(k, ce);
  return !coarse_.empty();
}

// ───────────── filters.ts / filterbank.ts ─────────────

static double blackman(int k, int taps) {
  const double x = (2 * kPI * k) / (taps - 1);
  return 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2 * x);
}

// Taps are Float32Array in the TypeScript, so every stored tap is rounded to
// float at the point the TS stores it; reproduce that exactly.
static std::vector<float> lowpass(double cutoffHz, int taps) {
  const double m = (taps - 1) / 2.0;
  const double fc = cutoffHz / SAMPLE_RATE;
  std::vector<float> h(taps);
  double sum = 0;
  for (int k = 0; k < taps; k++) {
    const double n = k - m;
    const double sinc = n == 0 ? 2 * fc : std::sin(2 * kPI * fc * n) / (kPI * n);
    h[k] = static_cast<float>(sinc * blackman(k, taps));
    sum += h[k];
  }
  for (int k = 0; k < taps; k++) h[k] = static_cast<float>(h[k] / sum);
  return h;
}

static std::vector<float> bandpass(double centerHz, double halfWidthHz, int taps) {
  std::vector<float> lp = lowpass(halfWidthHz, taps);
  const double m = (taps - 1) / 2.0;
  const double w = (2 * kPI * centerHz) / SAMPLE_RATE;
  std::vector<float> h(taps);
  for (int k = 0; k < taps; k++) h[k] = static_cast<float>(2.0 * static_cast<double>(lp[k]) * std::cos(w * (k - m)));
  return h;
}

static std::vector<float> lowpassCausal(double cutoffHz, int taps) {
  const int m = (taps - 1) / 2;
  const double a = std::exp((-2 * kPI * cutoffHz) / SAMPLE_RATE);
  std::vector<float> h(taps, 0.0f);
  double sum = 0;
  for (int k = 0; k <= m; k++) {
    h[k] = static_cast<float>(std::pow(a, m - k));
    sum += h[k];
  }
  for (int k = 0; k <= m; k++) h[k] = static_cast<float>(h[k] / sum);
  return h;
}

static std::vector<float> mixTaps(const std::vector<float>& a, const std::vector<float>& b, double t) {
  std::vector<float> h(a.size());
  for (size_t k = 0; k < a.size(); k++) h[k] = static_cast<float>(static_cast<double>(a[k]) + (static_cast<double>(b[k]) - static_cast<double>(a[k])) * t);
  return h;
}

static std::vector<float> lowpassPeaked(double cutoffHz, double peak, double peakHz, int taps) {
  std::vector<float> lp = lowpass(cutoffHz, taps);
  std::vector<float> lp2 = lowpass(peakHz, taps);
  const int m = (taps - 1) / 2;
  std::vector<float> h(taps);
  for (int k = 0; k < taps; k++) {
    const double delta = k == m ? 1 : 0;
    h[k] = static_cast<float>(static_cast<double>(lp[k]) + peak * (delta - static_cast<double>(lp2[k])));
  }
  return h;
}

void designFilterBank(const Controls& c, float out[NUM_SECTIONS * FILTER_STRIDE]) {
  for (int i = 0; i < NUM_SECTIONS * FILTER_STRIDE; i++) out[i] = 0;
  auto put = [&](int sec, const std::vector<float>& taps) {
    for (size_t k = 0; k < taps.size(); k++) out[sec * FILTER_STRIDE + k] = taps[k];
  };
  put(0, lowpass(c[C_encChromaMHz] * 1e6, 33));
  put(1, mixTaps(lowpass(c[C_demodMHz] * 1e6, 41), lowpassCausal(c[C_demodMHz] * 1e6, 41), c[C_chromaTail]));
  put(2, lowpassPeaked(c[C_lumaMHz] * 1e6, c[C_lumaPeak], c[C_lumaMHz] * 0.75e6, 49));
  put(3, bandpass(FSC, 0.6e6, 55));
  put(4, lowpass(1.2e6, 55));
}

// ───────────── uniforms.ts ─────────────

double loRadPerSample(double detuneKHz) { return (2 * kPI * detuneKHz * 1e3) / SAMPLE_RATE; }

double impulseStorm(double t) {
  const double e = jmax(0, 0.4 + 1.3 * valueNoise(t * 0.6, 5));
  return e * e * (1 + 0.4 * valueNoise(t * 2.7, 9));
}

static double noiseGrainPx(double bwMHz) {
  return ((SAMPLE_RATE / (2 * jmax(bwMHz, 0.05) * 1e6)) * ACTIVE_WIDTH) / SAMPLES_PER_LINE;
}

static double bandSigmaPx(double bwMHz) {
  return bwMHz > 0 ? jmin((0.1325 * SAMPLE_RATE) / (bwMHz * 1e6), 8) : 0;
}

static void noiseTiltWeights(double tilt, double& lo, double& hi) {
  const double RHO = 1 / (2 * std::sqrt(3.0));
  const double t = clamp01(tilt);
  const double norm = 1 / std::sqrt((1 - t) * (1 - t) + t * t + 2 * t * (1 - t) * RHO);
  lo = (1 - t) * norm;
  hi = t * norm;
}

void uniformValues(const Controls& c, const UniformEnv& env, double p[kNumParams]) {
  double noiseLoW, noiseHiW;
  noiseTiltWeights(c[C_noiseTilt], noiseLoW, noiseHiW);
#include "generated/uniforms.gen.inc"
}

void packParams(const double p[kNumParams], uint8_t out[kParamBytes]) {
  for (int i = 0; i < kParamBytes; i++) out[i] = 0;
  for (int i = 0; i < kNumParams; i++) {
    if (kParamIsU32[i]) {
      const uint32_t u = toUint32(p[i]);
      std::memcpy(out + i * 4, &u, 4);
    } else {
      const float f = static_cast<float>(p[i]);
      std::memcpy(out + i * 4, &f, 4);
    }
  }
}

} // namespace skillet
