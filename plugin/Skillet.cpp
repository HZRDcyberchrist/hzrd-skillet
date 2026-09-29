// Skillet — the videoskillet NTSC signal path as a Resolume FFGL effect.
// videoskillet (c) 2026 Colin Diesh, MIT License. See LICENSE-videoskillet.txt.
#include "Skillet.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../engine/engine.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#else
#include <dlfcn.h>
#endif

using namespace skillet;

// ── parameter layout ──
enum : unsigned {
  PT_PRESET = 0,
  PT_MORPH,
  PT_PREV,
  PT_NEXT,
  PT_RANDOM,
  PT_AMOUNT,
  // performance knobs over the preset
  PT_NOISE,
  PT_WOBBLE,
  PT_TRACKING,
  PT_ROLL,
  PT_BEND,
  PT_TINT,
  PT_COLOR,
  PT_CAMLOOP,
  PT_TIME,
  // assignable knobs: pick any of the app's controls, then play it
  PT_ASSIGN_T0,
  PT_ASSIGN_V0 = PT_ASSIGN_T0 + 4,
  // sources and the tube
  PT_SOURCE_A = PT_ASSIGN_V0 + 4,
  PT_SOURCE_B,
  PT_MIRROR,
  PT_FILL,
  PT_RESET,
  PT_CAPTION,
  // sixteen pads, each firing its own preset
  PT_PAD_PRESET0,
  PT_PAD0 = PT_PAD_PRESET0 + 16,
  PT_COUNT = PT_PAD0 + 16,
};
static_assert(PT_COUNT <= 128, "values_ too small");

static CFFGLPluginInfo PluginInfo(PluginFactory<Skillet>, "VSKL", "Skillet NTSC", 2, 1, 1, 0, FF_EFFECT,
                                  "Analog NTSC signal-path emulation: composite encode, tape, RF, sync, feedback and CRT, "
                                  "with the videoskillet preset catalogue.",
                                  "Port of videoskillet (c) Colin Diesh, MIT License");

static const char* const kSourceANames[] = {"Layer", "TV static", "Blank-tape static", "Video synth"};
static const char* const kSourceBNames[] = {"Auto", "Off", "TV static", "Blank-tape static", "Video synth", "Color bars", "Layer copy"};
// A spread across the catalogue for the pads' starting assignment.
static const char* const kPadDefaults[16] = {"vhs", "wornTape", "trackingBand", "pictureSearch", "fringeReception",
                                             "scrambledChannel", "fullCollapse", "bentScan", "mixerLoop", "spiral",
                                             "ringLoop", "meltdown", "contourLines", "rainbowStorm", "neonTube",
                                             "greenTerminal"};

static int presetByName(const char* name) {
  for (int i = 0; i < kNumPresets; i++)
    if (std::strcmp(kPresets[i].name, name) == 0) return i;
  return 0;
}

static std::string presetLabel(int i) {
  return std::string(kPresets[i].group) + " / " + kPresets[i].label;
}

static void* getProc(const char* name) {
#if defined(_WIN32)
  void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
  // wglGetProcAddress refuses the GL 1.1 entry points (and some drivers
  // answer 1, 2, 3 or -1 for "no"); those live in opengl32.dll itself.
  const intptr_t v = reinterpret_cast<intptr_t>(p);
  if (v == 0 || v == 1 || v == 2 || v == 3 || v == -1) {
    static HMODULE gl32 = LoadLibraryA("opengl32.dll");
    p = reinterpret_cast<void*>(GetProcAddress(gl32, name));
  }
  return p;
#else
  return dlsym(RTLD_DEFAULT, name);
#endif
}

Skillet::Skillet() : CFFGLPlugin(false), t0_(std::chrono::steady_clock::now()) {
  SetMinInputs(1);
  SetMaxInputs(1);

  // defaults
  values_[PT_PRESET] = 0;
  values_[PT_MORPH] = 1.0f;
  values_[PT_AMOUNT] = 1.0f;
  values_[PT_COLOR] = 1.0f;
  values_[PT_TIME] = 1.0f;
  for (int i = 0; i < 4; i++) {
    values_[PT_ASSIGN_T0 + i] = 0;  // "none"
    values_[PT_ASSIGN_V0 + i] = 0.5f;
  }
  values_[PT_SOURCE_A] = 0;
  values_[PT_SOURCE_B] = 0;
  for (int i = 0; i < 16; i++) values_[PT_PAD_PRESET0 + i] = static_cast<float>(presetByName(kPadDefaults[i]));

  // Preset
  SetOptionParamInfo(PT_PRESET, "Preset", kNumPresets, 0);
  for (int i = 0; i < kNumPresets; i++) SetParamElementInfo(PT_PRESET, i, presetLabel(i).c_str(), static_cast<float>(i));
  SetParamInfo(PT_MORPH, "Morph", FF_TYPE_STANDARD, values_[PT_MORPH]);
  SetParamRange(PT_MORPH, 0, 30);
  SetParamInfo(PT_PREV, "Prev", FF_TYPE_EVENT, 0.0f);
  SetParamInfo(PT_NEXT, "Next", FF_TYPE_EVENT, 0.0f);
  SetParamInfo(PT_RANDOM, "Random", FF_TYPE_EVENT, 0.0f);
  SetParamInfo(PT_AMOUNT, "Amount", FF_TYPE_STANDARD, values_[PT_AMOUNT]);
  SetParamRange(PT_AMOUNT, 0, 2);
  for (unsigned p : {PT_PRESET, PT_MORPH, PT_PREV, PT_NEXT, PT_RANDOM, PT_AMOUNT}) SetParamGroup(p, "Preset");

  // Performance
  SetParamInfo(PT_NOISE, "Noise", FF_TYPE_STANDARD, 0.0f);
  SetParamInfo(PT_WOBBLE, "Tape wobble", FF_TYPE_STANDARD, 0.0f);
  SetParamInfo(PT_TRACKING, "Tracking", FF_TYPE_STANDARD, 0.0f);
  SetParamInfo(PT_ROLL, "Roll", FF_TYPE_STANDARD, 0.0f);
  SetParamRange(PT_ROLL, -1, 1);
  SetParamInfo(PT_BEND, "Bend", FF_TYPE_STANDARD, 0.0f);
  SetParamInfo(PT_TINT, "Tint", FF_TYPE_STANDARD, 0.0f);
  SetParamRange(PT_TINT, -180, 180);
  SetParamInfo(PT_COLOR, "Color", FF_TYPE_STANDARD, values_[PT_COLOR]);
  SetParamRange(PT_COLOR, 0, 4);
  SetParamInfo(PT_CAMLOOP, "Camera loop", FF_TYPE_STANDARD, 0.0f);
  SetParamInfo(PT_TIME, "Time", FF_TYPE_STANDARD, values_[PT_TIME]);
  for (unsigned p = PT_NOISE; p <= PT_TIME; p++) SetParamGroup(p, "Perform");

  // Assignable knobs: "none" plus every control in the app
  for (int i = 0; i < 4; i++) {
    const std::string n = std::to_string(i + 1);
    SetOptionParamInfo(PT_ASSIGN_T0 + i, ("Assign " + n).c_str(), kNumControls + 1, 0);
    SetParamElementInfo(PT_ASSIGN_T0 + i, 0, "(none)", 0);
    for (int k = 0; k < kNumControls; k++)
      SetParamElementInfo(PT_ASSIGN_T0 + i, k + 1, kSliders[k].label, static_cast<float>(k + 1));
    SetParamInfo(PT_ASSIGN_V0 + i, ("Knob " + n).c_str(), FF_TYPE_STANDARD, values_[PT_ASSIGN_V0 + i]);
    SetParamGroup(PT_ASSIGN_T0 + i, "Assign");
    SetParamGroup(PT_ASSIGN_V0 + i, "Assign");
  }

  // Sources and the tube
  SetOptionParamInfo(PT_SOURCE_A, "Source A", 4, 0);
  for (int i = 0; i < 4; i++) SetParamElementInfo(PT_SOURCE_A, i, kSourceANames[i], static_cast<float>(i));
  SetOptionParamInfo(PT_SOURCE_B, "Source B", 7, 0);
  for (int i = 0; i < 7; i++) SetParamElementInfo(PT_SOURCE_B, i, kSourceBNames[i], static_cast<float>(i));
  SetParamInfo(PT_MIRROR, "Mirror A", FF_TYPE_BOOLEAN, 0.0f);
  SetParamInfo(PT_FILL, "Fill frame", FF_TYPE_BOOLEAN, 0.0f);
  SetParamInfo(PT_RESET, "Reset signal", FF_TYPE_EVENT, 0.0f);
  SetParamInfo(PT_CAPTION, "Caption", FF_TYPE_TEXT, caption_.c_str());
  for (unsigned p : {PT_SOURCE_A, PT_SOURCE_B, PT_MIRROR, PT_FILL, PT_RESET, PT_CAPTION}) SetParamGroup(p, "Sources");

  // Pads
  for (int i = 0; i < 16; i++) {
    const std::string n = std::to_string(i + 1);
    SetOptionParamInfo(PT_PAD_PRESET0 + i, ("Pad " + n + " preset").c_str(), kNumPresets, values_[PT_PAD_PRESET0 + i]);
    for (int k = 0; k < kNumPresets; k++) SetParamElementInfo(PT_PAD_PRESET0 + i, k, presetLabel(k).c_str(), static_cast<float>(k));
    SetParamInfo(PT_PAD0 + i, ("Pad " + n).c_str(), FF_TYPE_EVENT, 0.0f);
    SetParamGroup(PT_PAD_PRESET0 + i, "Pads");
    SetParamGroup(PT_PAD0 + i, "Pads");
  }
}

Skillet::~Skillet() {}

double Skillet::nowMs() const {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_).count();
}

FFResult Skillet::InitGL(const FFGLViewportStruct* vp) {
  const uint32_t seed = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  engine_.reset(new Engine(getProc, seed | 1u));
  if (!engine_->ok()) {
    FFGLLog::LogToHost(("Skillet: " + engine_->error()).c_str());
    engine_.reset();
    return FF_FAIL;
  }
  engine_->setCaption(caption_);
  loadPreset(currentPreset_, true);
  return CFFGLPlugin::InitGL(vp);
}

FFResult Skillet::DeInitGL() {
  engine_.reset();
  return FF_SUCCESS;
}

// Loading a preset morphs the resting board to it over the Morph time (the
// app's glide), except the view keys, which it sets outright.
void Skillet::loadPreset(int index, bool cut) {
  if (index < 0 || index >= kNumPresets) return;
  currentPreset_ = index;
  values_[PT_PRESET] = static_cast<float>(index);
  if (!engine_) return;
  Controls to;
  const Preset& p = kPresets[index];
  for (int i = 0; i < p.count; i++) to[kPresetPatches[p.first + i].key] = kPresetPatches[p.first + i].value;
  SignalChain& ch = engine_->chain();
  const double seconds = cut ? 0.0 : values_[PT_MORPH];
  ch.startMorph(to, seconds, nowMs());
  for (int i = 0; i < kViewKeysCount; i++) ch.controls[kViewKeys[i]] = to[kViewKeys[i]];
  // hardwired timeScale/frameLock would fight the Time knob; Time rides on top
}

static double spanClamp(int k, double v) {
  const SliderSpan& s = kSliders[k];
  const double lo = std::min(s.min, s.max), hi = std::max(s.min, s.max);
  return std::min(hi, std::max(lo, v));
}

static bool isEnumKey(int k) {
  for (int i = 0; i < kEnumKeysCount; i++)
    if (kEnumKeys[i] == k) return true;
  return false;
}

static bool isViewKey(int k) {
  for (int i = 0; i < kViewKeysCount; i++)
    if (kViewKeys[i] == k) return true;
  return false;
}

// The live layer over the resting board, rebuilt every frame and never
// written back: Amount scales the preset's departure from a clean signal, the
// Perform knobs push on top, and the Assign knobs override outright.
void Skillet::applyOverlay(void* p) const {
  Controls& c = *static_cast<Controls*>(p);
  const double amount = values_[PT_AMOUNT];
  if (amount != 1.0) {
    for (int k = 0; k < kNumControls; k++) {
      if (isViewKey(k)) continue;
      const double d = kControlDefaults[k];
      if (isEnumKey(k)) {
        if (amount < 0.5) c[k] = d;
      } else {
        c[k] = spanClamp(k, d + (c[k] - d) * amount);
      }
    }
  }
  const double noise = values_[PT_NOISE];
  if (noise > 0) c[C_noiseIre] = spanClamp(C_noiseIre, c[C_noiseIre] + noise * 30);
  const double wob = values_[PT_WOBBLE];
  if (wob > 0) {
    c[C_tbJitterNs] = spanClamp(C_tbJitterNs, c[C_tbJitterNs] + wob * 500);
    c[C_tbWowNs] = spanClamp(C_tbWowNs, c[C_tbWowNs] + wob * 900);
  }
  const double trk = values_[PT_TRACKING];
  if (trk > 0) c[C_trackAmt] = std::max(c[C_trackAmt], trk);
  const double roll = values_[PT_ROLL];
  if (roll != 0) {
    // detune the vertical oscillator and slacken the hold that would pull it in
    c[C_vFreqHz] = spanClamp(C_vFreqHz, c[C_vFreqHz] - roll * 1.5);
    c[C_vHold] = c[C_vHold] * (1 - std::abs(roll) * 0.97);
  }
  const double bend = values_[PT_BEND];
  if (bend > 0) c[C_bendUs] = spanClamp(C_bendUs, c[C_bendUs] + bend * 10);
  const double tint = values_[PT_TINT];
  if (tint != 0) c[C_tintDeg] = spanClamp(C_tintDeg, c[C_tintDeg] + tint);
  const double color = values_[PT_COLOR];
  if (color != 1) c[C_chromaGain] = spanClamp(C_chromaGain, c[C_chromaGain] * color);
  const double cam = values_[PT_CAMLOOP];
  if (cam > 0) c[C_fbMix] = std::max(c[C_fbMix], cam * 0.9);
  const double time = values_[PT_TIME];
  if (time < 1) c[C_timeScale] = c[C_timeScale] * time;
  for (int i = 0; i < 4; i++) {
    const int t = static_cast<int>(values_[PT_ASSIGN_T0 + i] + 0.5f) - 1;
    if (t < 0 || t >= kNumControls) continue;
    const SliderSpan& s = kSliders[t];
    double v = s.min + values_[PT_ASSIGN_V0 + i] * (s.max - s.min);
    if (s.step > 0) v = s.min + std::round((v - s.min) / s.step) * s.step;
    c[t] = spanClamp(t, v);
  }
}

FFResult Skillet::ProcessOpenGL(ProcessOpenGLStruct* pGL) {
  if (!engine_ || pGL->numInputTextures < 1 || pGL->inputTextures[0] == nullptr) return FF_FAIL;
  if (pendingReset_) {
    engine_->resetSignal();
    pendingReset_ = false;
  }
  if (pendingPreset_ >= 0) {
    loadPreset(pendingPreset_, pendingCut_);
    pendingPreset_ = -1;
  }
  const FFGLTextureStruct& t = *pGL->inputTextures[0];
  const int srcA = static_cast<int>(values_[PT_SOURCE_A] + 0.5f);
  int srcB = static_cast<int>(values_[PT_SOURCE_B] + 0.5f);
  // Auto: the layer itself on B when the preset mixes a second picture
  SourceB b;
  if (srcB == 0) b = kPresets[currentPreset_].needsB ? SourceB::LayerCopy : SourceB::Off;
  else b = static_cast<SourceB>(srcB - 1);
  engine_->setSourceA(srcA);
  engine_->setSourceB(b);
  engine_->setMirror(values_[PT_MIRROR] > 0.5f);

  InputFrame in;
  in.texture = t.Handle;
  in.width = static_cast<int>(t.Width);
  in.height = static_cast<int>(t.Height);
  in.hwWidth = static_cast<int>(t.HardwareWidth);
  in.hwHeight = static_cast<int>(t.HardwareHeight);
  in.bottomUp = true;
  OutputTarget out;
  out.fbo = pGL->HostFBO;
  out.x = static_cast<int>(currentViewport.x);
  out.y = static_cast<int>(currentViewport.y);
  out.width = static_cast<int>(currentViewport.width);
  out.height = static_cast<int>(currentViewport.height);
  out.fill = values_[PT_FILL] > 0.5f;

  SignalChain& ch = engine_->chain();
  ch.overlay = [this](Controls& c) { applyOverlay(&c); };
  engine_->render(in, out, nowMs());
  return FF_SUCCESS;
}

FFResult Skillet::SetFloatParameter(unsigned int index, float value) {
  if (index >= PT_COUNT) return FF_FAIL;
  const float prev = values_[index];
  values_[index] = value;
  const bool pressed = value > 0.5f && prev <= 0.5f;
  switch (index) {
    case PT_PRESET: {
      const int p = std::max(0, std::min(kNumPresets - 1, static_cast<int>(value + 0.5f)));
      if (p != currentPreset_) pendingPreset_ = p;
      break;
    }
    case PT_PREV:
      if (pressed) pendingPreset_ = (currentPreset_ + kNumPresets - 1) % kNumPresets;
      break;
    case PT_NEXT:
      if (pressed) pendingPreset_ = (currentPreset_ + 1) % kNumPresets;
      break;
    case PT_RANDOM:
      if (pressed) {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        int p = static_cast<int>(rng_ % (kNumPresets - 1)) + 1;  // never the clean board
        if (p == currentPreset_) p = (p % (kNumPresets - 1)) + 1;
        pendingPreset_ = p;
      }
      break;
    case PT_RESET:
      if (pressed) pendingReset_ = true;
      break;
    default:
      if (index >= PT_PAD0 && index < PT_PAD0 + 16 && pressed) {
        const int slot = static_cast<int>(index - PT_PAD0);
        pendingPreset_ = static_cast<int>(values_[PT_PAD_PRESET0 + slot] + 0.5f);
      }
      break;
  }
  if (pendingPreset_ >= 0) pendingCut_ = false;
  return FF_SUCCESS;
}

float Skillet::GetFloatParameter(unsigned int index) {
  if (index >= PT_COUNT) return 0.0f;
  if (index == PT_PRESET) return static_cast<float>(pendingPreset_ >= 0 ? pendingPreset_ : currentPreset_);
  return values_[index];
}

FFResult Skillet::SetTextParameter(unsigned int index, const char* value) {
  if (index != PT_CAPTION || value == nullptr) return FF_FAIL;
  caption_ = value;
  if (engine_) engine_->setCaption(caption_);
  return FF_SUCCESS;
}

char* Skillet::GetTextParameter(unsigned int index) {
  if (index != PT_CAPTION) return nullptr;
  return const_cast<char*>(caption_.c_str());
}

char* Skillet::GetParameterDisplay(unsigned int index) {
  char buf[64];
  switch (index) {
    case PT_MORPH:
      if (values_[PT_MORPH] < 0.05f) std::snprintf(buf, sizeof buf, "cut");
      else std::snprintf(buf, sizeof buf, "%.1f s", values_[PT_MORPH]);
      break;
    case PT_TINT: std::snprintf(buf, sizeof buf, "%+.0f deg", values_[PT_TINT]); break;
    case PT_AMOUNT: std::snprintf(buf, sizeof buf, "%.0f%%", values_[PT_AMOUNT] * 100); break;
    case PT_COLOR: std::snprintf(buf, sizeof buf, "x%.2f", values_[PT_COLOR]); break;
    default:
      if (index >= PT_ASSIGN_V0 && index < PT_ASSIGN_V0 + 4) {
        const int t = static_cast<int>(values_[PT_ASSIGN_T0 + (index - PT_ASSIGN_V0)] + 0.5f) - 1;
        if (t >= 0 && t < kNumControls) {
          const SliderSpan& s = kSliders[t];
          const double v = s.min + values_[index] * (s.max - s.min);
          std::snprintf(buf, sizeof buf, "%.3g", v);
          break;
        }
        std::snprintf(buf, sizeof buf, "-");
        break;
      }
      return CFFGLPlugin::GetParameterDisplay(index);
  }
  display_ = buf;
  return const_cast<char*>(display_.c_str());
}
