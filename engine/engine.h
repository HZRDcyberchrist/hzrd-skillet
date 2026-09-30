// The videoskillet signal path on OpenGL 4.5 compute: the pass graph of
// src/core/gpu/pipeline.ts (MIT, Colin Diesh) driving the translated shaders.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "chain.h"
#include "extras.h"
#include "glapi.h"

namespace skillet {

// What slot B carries. The app's slot B holds a second clip; in a single-input
// effect it is fed from a generator, a test pattern, or the layer itself.
enum class SourceB : int { Off = 0, TvStatic, VhsStatic, Synth, ColorBars, LayerCopy, Count };

struct InputFrame {
  unsigned texture = 0;   // GL name of the host's input texture (0 = none)
  int width = 0, height = 0;          // content size
  int hwWidth = 0, hwHeight = 0;      // allocated size (content sits at 0,0)
  bool bottomUp = true;   // GL convention: row 0 is the bottom of the picture
};

struct OutputTarget {
  unsigned fbo = 0;       // framebuffer to draw into (0 = default)
  int x = 0, y = 0, width = 0, height = 0;
  bool fill = false;      // stretch the 4:3 tube to the viewport
};

class Engine {
 public:
  // Needs a current GL 4.3+ context. On failure, ok() is false and error()
  // says why; nothing else may be called.
  Engine(gl::GetProcFn getProc, uint32_t seed);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }
  // vendor | renderer | version of the context the engine was built in
  const std::string& glInfo() const { return glInfo_; }

  SignalChain& chain() { return *chain_; }

  // Slot A: the layer (0) or a generator in its place (1 TV static,
  // 2 blank-tape static, 3 video synth) — the app's srcNoise.
  void setSourceA(int kind) { srcNoiseA_ = kind; }
  void setSourceB(SourceB kind);
  void setMirror(bool on) { mirror_ = on; }
  void setCaption(const std::string& text);

  // Runs one host frame: stages the input, advances the simulation (unless
  // timeScale is holding it), and draws the tube into `out`. Leaves the GL
  // context in its default state apart from the output framebuffer binding
  // the caller had.
  void render(const InputFrame& in, const OutputTarget& out, double nowMs) { render(in, in, out, nowMs); }
  // `inB` is the picture Source B's "layer copy" takes (the layer itself, the
  // layers below in the mixer, or a tracery generator).
  void render(const InputFrame& in, const InputFrame& inB, const OutputTarget& out, double nowMs);

  // The tracery generators (extras.h)
  Extras& extras() { return extras_; }

  // Clears every buffer and texture the path carries state in (the tape ring,
  // phosphor, the frame store, the sync flywheel) and restarts the CPU state.
  void resetSignal();

  // Debug/test access: the finished tube face at raster size, RGBA8.
  unsigned faceTexture() const { return faceTex_; }

 private:
  struct Program;
  enum Res : int;
  struct Pass;

  bool build(gl::GetProcFn getProc);
  unsigned compile(const char* name, gl::GLenum stage, const char* src);
  bool buildPrograms();
  void createResources();
  void destroyResources();
  void stageInput(const InputFrame& in);
  void stageSourceB(const InputFrame& in);
  void uploadBars();
  void bindPass(const Pass& p, int encodeTarget);
  void dispatch(const Pass& p);
  void runSimulation(const InputFrame& in);
  void present(const OutputTarget& out, unsigned tex);
  void clearTexture(unsigned tex);
  unsigned bufOf(int r) const;
  unsigned texOf(int r) const;

  bool ok_ = false;
  bool legacy_ = false;  // a 4.1 context: compute through ARB extensions
  std::string error_;
  std::string glInfo_;
  std::unique_ptr<SignalChain> chain_;

  // programs, indexed like kPrograms
  std::unique_ptr<Program[]> programs_;
  int progIndex(const char* name) const;

  // buffers
  unsigned paramsUbo_ = 0, feedAUbo_ = 0, feedBUbo_ = 0;
  unsigned filterBuf_ = 0, uvfBBuf_ = 0, compA_ = 0, compB_ = 0, bComp_ = 0, compPrev_ = 0, progSnap_ = 0;
  unsigned chromaBuf_ = 0, underBuf_ = 0, lineInfoBuf_ = 0, lineParamsBuf_ = 0, timingBuf_ = 0;
  unsigned syncMeasureBuf_ = 0, audioBuf_ = 0, ccBuf_ = 0, cgBuf_ = 0, persist_[2] = {0, 0};
  // textures
  unsigned texA_ = 0, texB_ = 0, inputTex_ = 0, outTex_ = 0, outSrgb_ = 0, faceTex_ = 0, grainTex_ = 0;
  int texAW_ = 0, texAH_ = 0;
  unsigned sampler_ = 0, vao_ = 0, readFbo_ = 0, drawFbo_ = 0;

  int srcNoiseA_ = 0;
  SourceB sourceB_ = SourceB::Off;
  bool barsLoaded_ = false;
  bool mirror_ = false;
  double srcAspect_ = 4.0 / 3.0;
  uint32_t renderedFrames_ = 0;
  bool haveFace_ = false;
  Extras extras_;
};

} // namespace skillet
