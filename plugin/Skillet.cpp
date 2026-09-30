// Skillet — the videoskillet NTSC signal path as a Resolume FFGL effect.
// videoskillet (c) 2026 Colin Diesh, MIT License. See LICENSE-videoskillet.txt.
#include "Skillet.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "../engine/engine.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
std::wstring skilletDocumentsFolder();  // docs_folder.cpp
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
  // favorites: a shared list of starred presets with its own dropdown and buttons
  PT_FAV_ADD,
  PT_FAV_REMOVE,
  PT_FAV,
  PT_FAV_PREV,
  PT_FAV_NEXT,
  PT_FAV_RANDOM,
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
#if SKILLET_MIXER
  PT_SWAP,  // which of the host's two inputs is this layer
#endif
  // sixteen pads, each firing its own preset
  PT_PAD_PRESET0,
  PT_PAD0 = PT_PAD_PRESET0 + 16,
  // what the engine said when it started: "running on ..." or why not
  PT_STATUS = PT_PAD0 + 16,
  PT_COUNT,
};
static_assert(PT_COUNT <= 128, "values_ too small");

#if SKILLET_MIXER
// The mixer: two inputs, so Resolume lists it with the blend modes and hands
// it the layers below as well as the layer.
static CFFGLPluginInfo PluginInfo(PluginFactory<Skillet>, "VSKM", "Skillet NTSC Mixer", 2, 1, 1, 0, FF_EFFECT,
                                  "Skillet NTSC as a mixer: the layers below come in as a second picture for "
                                  "Source A/B, the preset mixes and the Confessional ghost station.",
                                  "Port of videoskillet (c) Colin Diesh, MIT License");
constexpr bool kMixer = true;
#else
static CFFGLPluginInfo PluginInfo(PluginFactory<Skillet>, "VSKL", "Skillet NTSC", 2, 1, 1, 0, FF_EFFECT,
                                  "Analog NTSC signal-path emulation: composite encode, tape, RF, sync, feedback and CRT, "
                                  "with the videoskillet preset catalogue.",
                                  "Port of videoskillet (c) Colin Diesh, MIT License");
constexpr bool kMixer = false;
#endif

// What a source slot can show besides the engine's own generators.
enum Pic : int { PIC_LAYER, PIC_BELOW, PIC_ROSE, PIC_LANCET, PIC_QUATREFOIL };

// Source A: 0 layer, 1-3 the app's generators, then tracery and (mixer) the layers below
static const char* const kSourceANames[] = {"Layer", "TV static", "Blank-tape static", "Video synth",
                                            "Tracery: rose window", "Tracery: lancet arcade", "Tracery: quatrefoil",
                                            "Layers below"};
static const int kSourceAPic[] = {PIC_LAYER, -1, -1, -1, PIC_ROSE, PIC_LANCET, PIC_QUATREFOIL, PIC_BELOW};
constexpr int kNumSourceA = kMixer ? 8 : 7;
// Source B: 0 auto, 1 off, 2-4 the app's generators, 5 bars, then pictures
static const char* const kSourceBNames[] = {"Auto", "Off", "TV static", "Blank-tape static", "Video synth", "Color bars",
                                            kMixer ? "Layer" : "Layer copy", "Tracery: rose window",
                                            "Tracery: lancet arcade", "Tracery: quatrefoil", "Layers below"};
static const int kSourceBPic[] = {-1, -1, -1, -1, -1, -1, PIC_LAYER, PIC_ROSE, PIC_LANCET, PIC_QUATREFOIL, PIC_BELOW};
constexpr int kNumSourceB = kMixer ? 11 : 10;
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

// Files live in the user's Documents folder (where Windows really keeps it,
// OneDrive redirection included), or $HOME elsewhere.
#if defined(_WIN32)
static std::wstring docPath(const char* name) {
  const std::wstring dir = skilletDocumentsFolder();
  std::wstring w;
  for (const char* c = name; *c; c++) w += static_cast<wchar_t>(*c);  // names are ASCII
  return dir.empty() ? w : dir + L"\\" + w;
}
static FILE* openDoc(const char* name, const char* mode) {
  std::wstring m;
  for (const char* c = mode; *c; c++) m += static_cast<wchar_t>(*c);
  return _wfopen(docPath(name).c_str(), m.c_str());
}
// Writes the whole file to a temporary and swaps it in, so a crash or a
// second Resolume mid-write never leaves it half written.
static void saveDoc(const char* name, const std::string& text) {
  const std::wstring path = docPath(name), tmp = path + L".tmp";
  FILE* f = _wfopen(tmp.c_str(), L"wb");
  if (!f) return;
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  if (std::fclose(f) == 0 && ok) MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  else DeleteFileW(tmp.c_str());
}
#else
static std::string docPath(const char* name) {
  const char* home = std::getenv("HOME");
  return std::string(home ? home : ".") + "/" + name;
}
static FILE* openDoc(const char* name, const char* mode) { return std::fopen(docPath(name).c_str(), mode); }
static void saveDoc(const char* name, const std::string& text) {
  const std::string path = docPath(name), tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return;
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  if (std::fclose(f) == 0 && ok) std::rename(tmp.c_str(), path.c_str());
  else std::remove(tmp.c_str());
}
#endif

// ── favorites ──
// One list shared by every Skillet instance in the process and kept in
// Documents\SkilletNTSC-favorites.txt (one preset name per line, in the order
// they were added), so favorites survive restarts and follow you across
// compositions. The file can be edited by hand while Resolume is closed.
namespace {
struct FavoriteStore {
  std::mutex m;
  std::vector<int> list;
  std::atomic<unsigned> version{0};  // 0 = not read from disk yet

  void loadLocked() {
    list.clear();
    if (FILE* f = openDoc("SkilletNTSC-favorites.txt", "rb")) {
      char line[256];
      while (std::fgets(line, sizeof line, f)) {
        std::string n(line);
        while (!n.empty() && (n.back() == '\n' || n.back() == '\r' || n.back() == ' ')) n.pop_back();
        for (int i = 0; i < kNumPresets; i++)
          if (n == kPresets[i].name && std::find(list.begin(), list.end(), i) == list.end()) list.push_back(i);
      }
      std::fclose(f);
    }
    version = 1;
  }
  void saveLocked() const {
    std::string text;
    for (int i : list) text += std::string(kPresets[i].name) + "\n";
    saveDoc("SkilletNTSC-favorites.txt", text);
  }
  // cheap per-frame check: has the list changed since `seen`?
  bool changedSince(unsigned seen) const { return version.load(std::memory_order_acquire) != seen || seen == 0; }
  // copy of the list, and its revision
  unsigned snapshot(std::vector<int>& out) {
    std::lock_guard<std::mutex> lock(m);
    if (version == 0) loadLocked();
    out = list;
    return version;
  }
  void add(int preset) {
    if (preset < 0 || preset >= kNumPresets) return;
    std::lock_guard<std::mutex> lock(m);
    if (version == 0) loadLocked();
    if (std::find(list.begin(), list.end(), preset) != list.end()) return;
    list.push_back(preset);
    version++;
    saveLocked();
  }
  void remove(int preset) {
    std::lock_guard<std::mutex> lock(m);
    if (version == 0) loadLocked();
    auto it = std::find(list.begin(), list.end(), preset);
    if (it == list.end()) return;
    list.erase(it);
    version++;
    saveLocked();
  }
};
FavoriteStore& favorites() {
  static FavoriteStore store;
  return store;
}
}  // namespace

// A float from the host as an index into n entries: NaN and out-of-range
// values never reach an int conversion.
static int toIndex(float v, int n) {
  if (!(v >= 0.0f)) return 0;  // also catches NaN
  if (v >= static_cast<float>(n - 1)) return n - 1;
  return static_cast<int>(v + 0.5f);
}

void Skillet::setRange(unsigned p, float lo, float hi) {
  lo_[p] = lo;
  hi_[p] = hi;
  SetParamRange(p, lo, hi);
}

int Skillet::indexOf(unsigned p, int n) const { return toIndex(values_[p], n); }

Skillet::Skillet() : CFFGLPlugin(false), t0_(std::chrono::steady_clock::now()) {
  SetMinInputs(kMixer ? 2 : 1);
  SetMaxInputs(kMixer ? 2 : 1);
  // every value is 0..1 unless given a range below; option lists get theirs
  // from their length
  for (int i = 0; i < 128; i++) hi_[i] = 1.0f;
  auto options = [this](unsigned p, int n) { hi_[p] = static_cast<float>(n - 1); };

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
  // Preset and pad lists: each entry's text is sized once for the "  <3"
  // mark, so marking later rewrites it in place and never frees a string the
  // host might be reading (see syncFavorites).
  SetOptionParamInfo(PT_PRESET, "Preset", kNumPresets, 0);
  for (int k = 0; k < kNumPresets; k++) {
    SetParamElementInfo(PT_PRESET, k, (presetLabel(k) + "  <3").c_str(), static_cast<float>(k));
    SetParamElementInfo(PT_PRESET, k, presetLabel(k).c_str(), static_cast<float>(k));
  }
  options(PT_PRESET, kNumPresets);
  SetParamInfo(PT_MORPH, "Morph", FF_TYPE_STANDARD, values_[PT_MORPH]);
  setRange(PT_MORPH, 0, 30);
  SetParamInfo(PT_PREV, "Prev", FF_TYPE_EVENT, 0.0f);
  SetParamInfo(PT_NEXT, "Next", FF_TYPE_EVENT, 0.0f);
  SetParamInfo(PT_RANDOM, "Random", FF_TYPE_EVENT, 0.0f);
  SetParamInfo(PT_AMOUNT, "Amount", FF_TYPE_STANDARD, values_[PT_AMOUNT]);
  setRange(PT_AMOUNT, 0, 2);
  for (unsigned p : {PT_PRESET, PT_MORPH, PT_PREV, PT_NEXT, PT_RANDOM, PT_AMOUNT}) SetParamGroup(p, "Preset");

  // Favorites. The dropdown's first entry says whether the preset that's up
  // is one of them; the rest are the favorites, rebuilt whenever the list
  // changes (syncFavorites).
  SetParamInfo(PT_FAV_ADD, "Add favorite", FF_TYPE_EVENT, 0.0f);
  SetParamInfo(PT_FAV_REMOVE, "Remove favorite", FF_TYPE_EVENT, 0.0f);
  // sized for every preset up front so the list never has to move in memory
  SetOptionParamInfo(PT_FAV, "Favorites", kNumPresets + 1, 0);
  SetParamElements(PT_FAV, {"(no favorites yet)"}, {0.0f}, false);
  SetParamInfo(PT_FAV_PREV, "Fav prev", FF_TYPE_EVENT, 0.0f);
  SetParamInfo(PT_FAV_NEXT, "Fav next", FF_TYPE_EVENT, 0.0f);
  SetParamInfo(PT_FAV_RANDOM, "Fav random", FF_TYPE_EVENT, 0.0f);
  for (unsigned p = PT_FAV_ADD; p <= PT_FAV_RANDOM; p++) SetParamGroup(p, "Favorites");

  // Performance
  SetParamInfo(PT_NOISE, "Noise", FF_TYPE_STANDARD, 0.0f);
  SetParamInfo(PT_WOBBLE, "Tape wobble", FF_TYPE_STANDARD, 0.0f);
  SetParamInfo(PT_TRACKING, "Tracking", FF_TYPE_STANDARD, 0.0f);
  SetParamInfo(PT_ROLL, "Roll", FF_TYPE_STANDARD, 0.0f);
  setRange(PT_ROLL, -1, 1);
  SetParamInfo(PT_BEND, "Bend", FF_TYPE_STANDARD, 0.0f);
  SetParamInfo(PT_TINT, "Tint", FF_TYPE_STANDARD, 0.0f);
  setRange(PT_TINT, -180, 180);
  SetParamInfo(PT_COLOR, "Color", FF_TYPE_STANDARD, values_[PT_COLOR]);
  setRange(PT_COLOR, 0, 4);
  SetParamInfo(PT_CAMLOOP, "Camera loop", FF_TYPE_STANDARD, 0.0f);
  SetParamInfo(PT_TIME, "Time", FF_TYPE_STANDARD, values_[PT_TIME]);
  for (unsigned p = PT_NOISE; p <= PT_TIME; p++) SetParamGroup(p, "Perform");

  // Assignable knobs: "none" plus every control in the app
  for (int i = 0; i < 4; i++) {
    const std::string n = std::to_string(i + 1);
    SetOptionParamInfo(PT_ASSIGN_T0 + i, ("Assign " + n).c_str(), kNumControls + 1, 0);
    options(PT_ASSIGN_T0 + i, kNumControls + 1);
    SetParamElementInfo(PT_ASSIGN_T0 + i, 0, "(none)", 0);
    for (int k = 0; k < kNumControls; k++)
      SetParamElementInfo(PT_ASSIGN_T0 + i, k + 1, kSliders[k].label, static_cast<float>(k + 1));
    SetParamInfo(PT_ASSIGN_V0 + i, ("Knob " + n).c_str(), FF_TYPE_STANDARD, values_[PT_ASSIGN_V0 + i]);
    SetParamGroup(PT_ASSIGN_T0 + i, "Assign");
    SetParamGroup(PT_ASSIGN_V0 + i, "Assign");
  }

  // Sources and the tube
  SetOptionParamInfo(PT_SOURCE_A, "Source A", kNumSourceA, 0);
  options(PT_SOURCE_A, kNumSourceA);
  for (int i = 0; i < kNumSourceA; i++) SetParamElementInfo(PT_SOURCE_A, i, kSourceANames[i], static_cast<float>(i));
  SetOptionParamInfo(PT_SOURCE_B, "Source B", kNumSourceB, 0);
  options(PT_SOURCE_B, kNumSourceB);
  for (int i = 0; i < kNumSourceB; i++) SetParamElementInfo(PT_SOURCE_B, i, kSourceBNames[i], static_cast<float>(i));
  SetParamInfo(PT_MIRROR, "Mirror A", FF_TYPE_BOOLEAN, 0.0f);
  SetParamInfo(PT_FILL, "Fill frame", FF_TYPE_BOOLEAN, 0.0f);
  SetParamInfo(PT_RESET, "Reset signal", FF_TYPE_EVENT, 0.0f);
  SetParamInfo(PT_CAPTION, "Caption", FF_TYPE_TEXT, caption_.c_str());
  for (unsigned p : {PT_SOURCE_A, PT_SOURCE_B, PT_MIRROR, PT_FILL, PT_RESET, PT_CAPTION}) SetParamGroup(p, "Sources");
#if SKILLET_MIXER
  SetParamInfo(PT_SWAP, "Swap inputs", FF_TYPE_BOOLEAN, 0.0f);
  SetParamGroup(PT_SWAP, "Sources");
#endif

  // Pads
  for (int i = 0; i < 16; i++) {
    const std::string n = std::to_string(i + 1);
    SetOptionParamInfo(PT_PAD_PRESET0 + i, ("Pad " + n + " preset").c_str(), kNumPresets, values_[PT_PAD_PRESET0 + i]);
    for (int k = 0; k < kNumPresets; k++) {
      SetParamElementInfo(PT_PAD_PRESET0 + i, k, (presetLabel(k) + "  <3").c_str(), static_cast<float>(k));
      SetParamElementInfo(PT_PAD_PRESET0 + i, k, presetLabel(k).c_str(), static_cast<float>(k));
    }
    options(PT_PAD_PRESET0 + i, kNumPresets);
    SetParamInfo(PT_PAD0 + i, ("Pad " + n).c_str(), FF_TYPE_EVENT, 0.0f);
    SetParamGroup(PT_PAD_PRESET0 + i, "Pads");
    SetParamGroup(PT_PAD0 + i, "Pads");
  }

  SetParamInfo(PT_STATUS, "Status", FF_TYPE_TEXT, status_.c_str());
  SetParamGroup(PT_STATUS, "Status");
  std::memcpy(frame_, values_, sizeof values_);
  syncFavorites(true);
}

// Rebuilds the Favorites dropdown when the shared list has changed (here or
// in another instance), then points it at the preset that's up. Render
// thread (and the constructor) only, with stateMutex_ held.
void Skillet::syncFavorites(bool force) {
  if (!force && !favorites().changedSince(favsVersion_)) {
    syncFavoriteValue();
    return;
  }
  std::vector<int> now;
  const unsigned v = favorites().snapshot(now);
  if (force || v != favsVersion_) {
    const std::vector<int> was = force ? std::vector<int>() : favs_;
    favsVersion_ = v;
    favs_ = now;
    std::vector<std::string> names;
    std::vector<float> vals;
    names.push_back(favs_.empty() ? "(no favorites yet)" : "(not a favorite)");
    vals.push_back(0);
    for (size_t i = 0; i < favs_.size(); i++) {
      names.push_back(presetLabel(favs_[i]));
      vals.push_back(static_cast<float>(i + 1));
    }
    SetParamElements(PT_FAV, names, vals, !force);
    hi_[PT_FAV] = static_cast<float>(favs_.size());
    values_[PT_FAV] = -1;  // force the value event below

    // Mark favorites with "  <3" in the Preset and Pad preset dropdowns:
    // only the entries whose mark changed, rewritten in place.
    auto isFav = [](const std::vector<int>& l, int i) { return std::find(l.begin(), l.end(), i) != l.end(); };
    bool changed = false;
    for (int i = 0; i < kNumPresets; i++) {
      const bool fav = isFav(favs_, i);
      if (!force && fav == isFav(was, i)) continue;
      if (force && !fav) continue;
      const std::string label = fav ? presetLabel(i) + "  <3" : presetLabel(i);
      SetParamElementInfo(PT_PRESET, i, label.c_str(), static_cast<float>(i));
      for (int pad = 0; pad < 16; pad++) SetParamElementInfo(PT_PAD_PRESET0 + pad, i, label.c_str(), static_cast<float>(i));
      changed = true;
    }
    if (changed && !force) {
      RaiseParamEvent(PT_PRESET, FF_EVENT_FLAG_ELEMENTS);
      for (int pad = 0; pad < 16; pad++) RaiseParamEvent(PT_PAD_PRESET0 + pad, FF_EVENT_FLAG_ELEMENTS);
    }
  }
  syncFavoriteValue();
}

// The Favorites dropdown shows the preset that's up when it is a favorite,
// and "(not a favorite)" when it isn't.
void Skillet::syncFavoriteValue() {
  const int preset = basePreset();
  float want = 0;
  for (size_t i = 0; i < favs_.size(); i++)
    if (favs_[i] == preset) want = static_cast<float>(i + 1);
  if (values_[PT_FAV] != want) {
    values_[PT_FAV] = want;
    RaiseParamEvent(PT_FAV, FF_EVENT_FLAG_VALUE);
  }
}

unsigned Skillet::nextRandom() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return rng_;
}

// Everything the engine reported at startup, written where a user can find
// it: Documents\\SkilletNTSC-log.txt on Windows. A plugin that fails in a host
// otherwise fails silently, and this is the only way to see why.
static void writeLog(const std::string& text) {
  if (FILE* f = openDoc("SkilletNTSC-log.txt", "wb")) {
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
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
    status_ = "FAILED: " + engine_->error().substr(0, 400);
    writeLog("Skillet NTSC could not start.\nOpenGL: " + engine_->glInfo() + "\n\n" + engine_->error());
    FFGLLog::LogToHost(("Skillet NTSC: " + engine_->error()).c_str());
    engine_.reset();
    RaiseParamEvent(PT_STATUS, FF_EVENT_FLAG_VALUE);
    // Stay loaded so the Status field can say why (the effect draws nothing).
    return CFFGLPlugin::InitGL(vp);
  }
  status_ = "Running on " + engine_->glInfo();
  writeLog("Skillet NTSC started.\nOpenGL: " + engine_->glInfo() + "\n");
  RaiseParamEvent(PT_STATUS, FF_EVENT_FLAG_VALUE);
  {
    std::lock_guard<std::mutex> lock(captionMutex_);
    engine_->setCaption(caption_);
    captionDirty_ = false;
  }
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    loadPreset(currentPreset_, true);
  }
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
  // Tell the host the dropdown's value moved, so Prev/Next/Random and the
  // pads show the preset they landed on (FFGL value-change event).
  RaiseParamEvent(PT_PRESET, FF_EVENT_FLAG_VALUE);
  syncFavoriteValue();
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
  const double amount = frame_[PT_AMOUNT];
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
  const double noise = frame_[PT_NOISE];
  if (noise > 0) c[C_noiseIre] = spanClamp(C_noiseIre, c[C_noiseIre] + noise * 30);
  const double wob = frame_[PT_WOBBLE];
  if (wob > 0) {
    c[C_tbJitterNs] = spanClamp(C_tbJitterNs, c[C_tbJitterNs] + wob * 500);
    c[C_tbWowNs] = spanClamp(C_tbWowNs, c[C_tbWowNs] + wob * 900);
  }
  const double trk = frame_[PT_TRACKING];
  if (trk > 0) c[C_trackAmt] = std::max(c[C_trackAmt], trk);
  const double roll = frame_[PT_ROLL];
  if (roll != 0) {
    // detune the vertical oscillator and slacken the hold that would pull it in
    c[C_vFreqHz] = spanClamp(C_vFreqHz, c[C_vFreqHz] - roll * 1.5);
    c[C_vHold] = c[C_vHold] * (1 - std::abs(roll) * 0.97);
  }
  const double bend = frame_[PT_BEND];
  if (bend > 0) c[C_bendUs] = spanClamp(C_bendUs, c[C_bendUs] + bend * 10);
  const double tint = frame_[PT_TINT];
  if (tint != 0) c[C_tintDeg] = spanClamp(C_tintDeg, c[C_tintDeg] + tint);
  const double color = frame_[PT_COLOR];
  if (color != 1) c[C_chromaGain] = spanClamp(C_chromaGain, c[C_chromaGain] * color);
  const double cam = frame_[PT_CAMLOOP];
  if (cam > 0) c[C_fbMix] = std::max(c[C_fbMix], cam * 0.9);
  const double time = frame_[PT_TIME];
  if (time < 1) c[C_timeScale] = c[C_timeScale] * time;
  for (int i = 0; i < 4; i++) {
    const int t = toIndex(frame_[PT_ASSIGN_T0 + i], kNumControls + 1) - 1;
    if (t < 0) continue;
    const SliderSpan& s = kSliders[t];
    double v = s.min + frame_[PT_ASSIGN_V0 + i] * (s.max - s.min);
    if (s.step > 0) v = s.min + std::round((v - s.min) / s.step) * s.step;
    c[t] = spanClamp(t, v);
  }
}

// The picture a source slot shows for anything that isn't one of the
// engine's own generators.
bool Skillet::pictureFor(int kind, int slot, const InputFrame& layer, const InputFrame& below, InputFrame& out) {
  out = InputFrame();
  const double sec = nowMs() / 1000.0;
  switch (kind) {
    case PIC_LAYER: out = layer; return out.texture != 0;
    case PIC_BELOW: out = below; return out.texture != 0;
    case PIC_ROSE:
    case PIC_LANCET:
    case PIC_QUATREFOIL: {
      const Generator g = kind == PIC_ROSE ? Generator::TraceryRose
                          : kind == PIC_LANCET ? Generator::TraceryLancet
                                               : Generator::TraceryQuatrefoil;
      int w = 0, h = 0;
      out.texture = engine_->extras().drawGenerator(slot, g, sec, 0.3f, w, h);
      out.width = out.hwWidth = w;
      out.height = out.hwHeight = h;
      out.bottomUp = true;
      return true;
    }
  }
  return false;
}

FFResult Skillet::ProcessOpenGL(ProcessOpenGLStruct* pGL) {
  const unsigned needInputs = kMixer ? 2 : 1;
  if (!engine_ || pGL->numInputTextures < needInputs) return FF_FAIL;
  for (unsigned i = 0; i < needInputs; i++)
    if (pGL->inputTextures[i] == nullptr) return FF_FAIL;
  std::unique_lock<std::mutex> state(stateMutex_);
  syncFavorites(false);  // another instance may have changed the list
  {
    std::lock_guard<std::mutex> lock(captionMutex_);
    if (captionDirty_) {
      engine_->setCaption(caption_);
      captionDirty_ = false;
    }
  }
  const double now = nowMs();
  if (pendingReset_) {
    engine_->resetSignal();
    pendingReset_ = false;
  }
  if (pendingPreset_ >= 0) {
    loadPreset(pendingPreset_, pendingCut_);
    pendingPreset_ = -1;
  }
  // the host's inputs: in the mixer, one is this layer and one is the layers
  // below (Resolume's order is destination first; Swap inputs flips it)
  auto frameOf = [](const FFGLTextureStruct& t) {
    InputFrame f;
    f.texture = t.Handle;
    f.width = static_cast<int>(t.Width);
    f.height = static_cast<int>(t.Height);
    f.hwWidth = static_cast<int>(t.HardwareWidth);
    f.hwHeight = static_cast<int>(t.HardwareHeight);
    f.bottomUp = true;
    return f;
  };
  InputFrame layer, below;
#if SKILLET_MIXER
  const bool swap = values_[PT_SWAP] > 0.5f;
  layer = frameOf(*pGL->inputTextures[swap ? 0 : 1]);
  below = frameOf(*pGL->inputTextures[swap ? 1 : 0]);
#else
  layer = frameOf(*pGL->inputTextures[0]);
#endif

  // Source A
  const int srcA = indexOf(PT_SOURCE_A, kNumSourceA);
  InputFrame inA = layer;
  if (kSourceAPic[srcA] < 0) {
    engine_->setSourceA(srcA);  // the app's generators
  } else {
    engine_->setSourceA(0);
    if (!pictureFor(kSourceAPic[srcA], 0, layer, below, inA)) inA = layer;
  }
  // Source B. Auto feeds presets that mix two pictures: the layers below in
  // the mixer, the layer itself otherwise.
  const int srcB = indexOf(PT_SOURCE_B, kNumSourceB);
  InputFrame inB = layer;
  SourceB b = SourceB::Off;
  if (srcB == 0) {
    if (kPresets[currentPreset_].needsB) {
      b = SourceB::LayerCopy;
      inB = kMixer ? below : layer;
    }
  } else if (kSourceBPic[srcB] < 0) {
    b = static_cast<SourceB>(srcB - 1);  // off, the generators, bars
  } else {
    b = SourceB::LayerCopy;
    if (!pictureFor(kSourceBPic[srcB], 1, layer, below, inB)) b = SourceB::Off;
  }
  engine_->setSourceB(b);
  engine_->setMirror(values_[PT_MIRROR] > 0.5f);

  OutputTarget out;
  out.fbo = pGL->HostFBO;
  out.x = static_cast<int>(currentViewport.x);
  out.y = static_cast<int>(currentViewport.y);
  out.width = static_cast<int>(currentViewport.width);
  out.height = static_cast<int>(currentViewport.height);
  out.fill = values_[PT_FILL] > 0.5f;
  // the render works from this frame's copy; the host may set values
  // meanwhile without waiting for the frame
  std::memcpy(frame_, values_, sizeof values_);
  state.unlock();

  SignalChain& ch = engine_->chain();
  ch.overlay = [this](Controls& c) { applyOverlay(&c); };
  engine_->render(inA, inB, out, now);

  return FF_SUCCESS;
}

FFResult Skillet::SetFloatParameter(unsigned int index, float value) {
  if (index >= PT_COUNT) return FF_FAIL;
  if (!std::isfinite(value)) return FF_FAIL;
  value = std::min(hi_[index], std::max(lo_[index], value));
  std::lock_guard<std::mutex> lock(stateMutex_);
  const float prev = values_[index];
  values_[index] = value;
  const bool pressed = value > 0.5f && prev <= 0.5f;
  switch (index) {
    case PT_PRESET: {
      const int p = toIndex(value, kNumPresets);
      if (p != currentPreset_) pendingPreset_ = p;
      break;
    }
    case PT_PREV:
      if (pressed) pendingPreset_ = (basePreset() + kNumPresets - 1) % kNumPresets;
      break;
    case PT_NEXT:
      if (pressed) pendingPreset_ = (basePreset() + 1) % kNumPresets;
      break;
    case PT_RANDOM:
      if (pressed) {
        int p = static_cast<int>(nextRandom() % (kNumPresets - 1)) + 1;  // never the clean board
        if (p == basePreset()) p = (p % (kNumPresets - 1)) + 1;
        pendingPreset_ = p;
      }
      break;
    // the render thread picks the change up and rebuilds the lists
    case PT_FAV_ADD:
      if (pressed) favorites().add(basePreset());
      break;
    case PT_FAV_REMOVE:
      if (pressed) favorites().remove(basePreset());
      break;
    case PT_FAV: {
      // entry 0 is the "(not a favorite)" marker; picking it does nothing
      const int k = toIndex(value, static_cast<int>(favs_.size()) + 1);
      if (k >= 1 && k <= static_cast<int>(favs_.size()) && favs_[k - 1] != basePreset()) pendingPreset_ = favs_[k - 1];
      else {
        values_[PT_FAV] = prev;  // snap back to what's actually up
        RaiseParamEvent(PT_FAV, FF_EVENT_FLAG_VALUE);
      }
      break;
    }
    case PT_FAV_PREV:
    case PT_FAV_NEXT:
      if (pressed && !favs_.empty()) {
        const int n = static_cast<int>(favs_.size());
        const int at = static_cast<int>(std::find(favs_.begin(), favs_.end(), basePreset()) - favs_.begin());
        int k;
        if (at == n) k = index == PT_FAV_NEXT ? 0 : n - 1;  // not on a favorite: start at an end
        else k = index == PT_FAV_NEXT ? (at + 1) % n : (at + n - 1) % n;
        if (favs_[k] != basePreset()) pendingPreset_ = favs_[k];
      }
      break;
    case PT_FAV_RANDOM:
      if (pressed && !favs_.empty()) {
        const int n = static_cast<int>(favs_.size());
        int k = static_cast<int>(nextRandom() % n);
        if (n > 1 && favs_[k] == basePreset()) k = (k + 1 + static_cast<int>(nextRandom() % (n - 1))) % n;
        if (favs_[k] != basePreset()) pendingPreset_ = favs_[k];
      }
      break;
    case PT_RESET:
      if (pressed) pendingReset_ = true;
      break;
    default:
      if (index >= PT_PAD0 && index < PT_PAD0 + 16 && pressed) {
        const int slot = static_cast<int>(index - PT_PAD0);
        pendingPreset_ = indexOf(PT_PAD_PRESET0 + slot, kNumPresets);
      }
      break;
  }
  if (pendingPreset_ >= 0) {
    pendingCut_ = false;
    syncFavoriteValue();
  }
  return FF_SUCCESS;
}

float Skillet::GetFloatParameter(unsigned int index) {
  if (index >= PT_COUNT) return 0.0f;
  std::lock_guard<std::mutex> lock(stateMutex_);
  if (index == PT_PRESET) return static_cast<float>(pendingPreset_ >= 0 ? pendingPreset_ : currentPreset_);
  return values_[index];
}

FFResult Skillet::SetTextParameter(unsigned int index, const char* value) {
  if (index == PT_STATUS) return FF_SUCCESS;  // read-only: the plugin owns it
  if (index != PT_CAPTION || value == nullptr) return FF_FAIL;
  std::lock_guard<std::mutex> lock(captionMutex_);
  caption_ = value;
  captionDirty_ = true;
  return FF_SUCCESS;
}

char* Skillet::GetTextParameter(unsigned int index) {
  if (index == PT_STATUS) return const_cast<char*>(status_.c_str());
  if (index != PT_CAPTION) return nullptr;
  std::lock_guard<std::mutex> lock(captionMutex_);
  captionShown_ = caption_;
  return const_cast<char*>(captionShown_.c_str());
}

// Every value is formatted here: the SDK's fallback shares one unterminated
// 16-byte buffer between all instances.
char* Skillet::GetParameterDisplay(unsigned int index) {
  if (index >= PT_COUNT) return nullptr;
  std::lock_guard<std::mutex> lock(stateMutex_);
  char buf[64];
  const float v = values_[index];
  switch (index) {
    case PT_MORPH:
      if (v < 0.05f) std::snprintf(buf, sizeof buf, "cut");
      else std::snprintf(buf, sizeof buf, "%.1f s", v);
      break;
    case PT_TINT: std::snprintf(buf, sizeof buf, "%+.0f deg", v); break;
    case PT_AMOUNT: std::snprintf(buf, sizeof buf, "%.0f%%", v * 100); break;
    case PT_COLOR: std::snprintf(buf, sizeof buf, "x%.2f", v); break;
    default:
      if (index >= PT_ASSIGN_V0 && index < PT_ASSIGN_V0 + 4) {
        const int t = indexOf(PT_ASSIGN_T0 + (index - PT_ASSIGN_V0), kNumControls + 1) - 1;
        if (t >= 0) {
          const SliderSpan& s = kSliders[t];
          std::snprintf(buf, sizeof buf, "%.3g", s.min + v * (s.max - s.min));
        } else {
          std::snprintf(buf, sizeof buf, "-");
        }
        break;
      }
      std::snprintf(buf, sizeof buf, "%.2f", v);
      break;
  }
  display_ = buf;
  return const_cast<char*>(display_.c_str());
}
