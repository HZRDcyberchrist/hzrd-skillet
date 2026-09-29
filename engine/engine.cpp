// The videoskillet pass graph (src/core/gpu/pipeline.ts, MIT, Colin Diesh) on
// OpenGL 4.5 compute. Pass order, bindings and gates follow the TypeScript;
// where WebGPU and GL differ (zero-initialised buffers, texture origin,
// resource hazards) the difference is handled here and named where it is.
#include "engine.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "generated/captionrom.gen.h"
#include "generated/shaders.gen.h"

namespace skillet {
using namespace gl;

// ── caption buffer layout (captionrom.ts) ──
constexpr int CC_COLS = 32, CC_ROWS = 4;
constexpr int CC_PAGE = GLYPH_COUNT * GLYPH_H;
constexpr int CC_CURSOR = CC_PAGE + CC_ROWS * CC_COLS;
constexpr int CC_BUF_LEN = CC_CURSOR + 1;
constexpr uint32_t CC_SET = 1u << 9;
constexpr int MAX_SRC_EDGE = 1536;

// buildPage: the character generator's page RAM for a caption string, word
// wrapped to 32 columns and bottom-aligned in its four rows.
static std::vector<uint32_t> buildPage(const std::string& text) {
  std::vector<uint32_t> page(CC_ROWS * CC_COLS, 0);
  std::vector<std::string> lines;
  size_t s = 0;
  while (true) {
    size_t e = text.find('\n', s);
    const std::string para = text.substr(s, e == std::string::npos ? std::string::npos : e - s);
    std::string line;
    size_t w = 0;
    bool first = true;
    while (true) {
      size_t we = para.find(' ', w);
      const std::string word = para.substr(w, we == std::string::npos ? std::string::npos : we - w);
      if (first) {
        line = word.substr(0, CC_COLS);
        first = false;
      } else if (line.empty()) {
        line = word.substr(0, CC_COLS);
      } else if (line.size() + 1 + word.size() <= static_cast<size_t>(CC_COLS)) {
        line += " " + word;
      } else {
        lines.push_back(line);
        line = word.substr(0, CC_COLS);
      }
      if (we == std::string::npos) break;
      w = we + 1;
    }
    lines.push_back(line);
    if (e == std::string::npos) break;
    s = e + 1;
  }
  const size_t shownN = std::min<size_t>(lines.size(), CC_ROWS);
  for (size_t r = 0; r < shownN; r++) {
    const std::string& line = lines[lines.size() - shownN + r];
    const size_t row = r + CC_ROWS - shownN;
    for (size_t c = 0; c < line.size() && c < static_cast<size_t>(CC_COLS); c++) {
      const unsigned char ch = static_cast<unsigned char>(line[c]);
      const uint32_t glyph = ch >= GLYPH_FIRST && ch < GLYPH_FIRST + GLYPH_COUNT ? ch - GLYPH_FIRST : 0;
      page[row * CC_COLS + c] = glyph | CC_SET;
    }
  }
  return page;
}

// ── programs ──
struct Engine::Program {
  GLuint id = 0;
  const ShaderProgram* def = nullptr;
};

// Resources a pass can name, in the binding order of pipeline.ts.
enum Engine::Res : int {
  R_PARAMS, R_FEEDA, R_FEEDB, R_FILTERS, R_UVFB, R_COMPA, R_COMPB, R_BCOMP, R_COMPPREV, R_PROGSNAP,
  R_CHROMA, R_UNDER, R_LINEINFO, R_LINEPARAMS, R_TIMING, R_SYNCMEASURE, R_AUDIO, R_CC, R_CG,
  R_PERSIST_READ, R_PERSIST_WRITE,
  R_ENC_DST,   // encodeComposite's destination: compA, or the compB scratch when A's feed is engaged
  R_ENCB_DST,  // encodeCompositeB's: bComp, or compB when B's feed is engaged
  T_SRCA, T_SRCB, T_INPUT, T_OUT, T_OUT_CRT, T_FACE, T_GRAIN, S_LINEAR,
};

struct Engine::Pass {
  int prog;
  std::vector<int> res;
  unsigned x, y;
};

static void* nullProc(const char*) { return nullptr; }

Engine::Engine(GetProcFn getProc, uint32_t seed) : chain_(new SignalChain(seed)) {
  ok_ = build(getProc ? getProc : nullProc);
}

Engine::~Engine() {
  if (ok_) destroyResources();
}

int Engine::progIndex(const char* name) const {
  for (int i = 0; i < kNumPrograms; i++)
    if (std::strcmp(kPrograms[i].name, name) == 0) return i;
  return -1;
}

// The shaders are written against GLSL 4.50. In a 4.1 context (Resolume's)
// the same code runs through ARB extensions the driver still offers, so the
// header is swapped for one that asks for them by name.
static std::string legacyHeader(GLenum stage) {
  std::string h = "#version 410 core\n#extension GL_ARB_shading_language_420pack : require\n";
  if (stage == COMPUTE_SHADER) {
    h += "#extension GL_ARB_compute_shader : require\n"
         "#extension GL_ARB_shader_storage_buffer_object : require\n"
         "#extension GL_ARB_shader_image_load_store : require\n"
         "#extension GL_ARB_shading_language_packing : enable\n"
         "#extension GL_ARB_shader_image_size : enable\n"
         "#extension GL_ARB_gpu_shader5 : enable\n";
  }
  return h;
}

unsigned Engine::compile(const char* name, GLenum stage, const char* src) {
  std::string patched;
  if (legacy_) {
    const char* v = std::strstr(src, "#version 450 core");
    if (v) {
      patched = std::string(src, v) + legacyHeader(stage) + (v + std::strlen("#version 450 core"));
      src = patched.c_str();
    }
  }
  GLuint s = CreateShader(stage);
  ShaderSource(s, 1, &src, nullptr);
  CompileShader(s);
  GLint okc = 0;
  GetShaderiv(s, COMPILE_STATUS, &okc);
  if (!okc) {
    char log[4096] = {};
    GetShaderInfoLog(s, sizeof log, nullptr, log);
    error_ = std::string("compile ") + name + ": " + log;
    DeleteShader(s);
    return 0;
  }
  return s;
}

bool Engine::buildPrograms() {
  // Compile everything and report every failure at once: a driver that
  // rejects one shader often rejects a few, and each round trip to find the
  // next is a rebuild on someone else's machine.
  programs_.reset(new Program[kNumPrograms]);
  std::string errors;
  for (int i = 0; i < kNumPrograms; i++) {
    const ShaderProgram& d = kPrograms[i];
    programs_[i].def = &d;
    if (std::strcmp(d.stage, "compute") != 0) continue;
    GLuint s = compile(d.name, COMPUTE_SHADER, d.src);
    if (!s) {
      errors += error_ + "\n";
      continue;
    }
    GLuint p = CreateProgram();
    AttachShader(p, s);
    LinkProgram(p);
    DeleteShader(s);
    GLint okl = 0;
    GetProgramiv(p, LINK_STATUS, &okl);
    if (!okl) {
      char log[4096] = {};
      GetProgramInfoLog(p, sizeof log, nullptr, log);
      errors += std::string("link ") + d.name + ": " + log + "\n";
      DeleteProgram(p);
      continue;
    }
    programs_[i].id = p;
  }
  // present: the vertex and fragment entry points share one program
  const int vs = progIndex("present.vs");
  const int fs = progIndex("present.fs");
  GLuint sv = compile("present.vs", VERTEX_SHADER, kPrograms[vs].src);
  if (!sv) errors += error_ + "\n";
  GLuint sf = compile("present.fs", FRAGMENT_SHADER, kPrograms[fs].src);
  if (!sf) errors += error_ + "\n";
  if (sv && sf) {
    GLuint p = CreateProgram();
    AttachShader(p, sv);
    AttachShader(p, sf);
    LinkProgram(p);
    GLint okl = 0;
    GetProgramiv(p, LINK_STATUS, &okl);
    if (!okl) {
      char log[4096] = {};
      GetProgramInfoLog(p, sizeof log, nullptr, log);
      errors += std::string("link present: ") + log + "\n";
      DeleteProgram(p);
    } else {
      programs_[fs].id = p;
    }
  }
  if (sv) DeleteShader(sv);
  if (sf) DeleteShader(sf);
  if (!errors.empty()) {
    error_ = errors;
    return false;
  }
  return true;
}

// Every buffer the engine makes, with its size, so a reset can zero it.
static std::vector<std::pair<GLuint, GLsizeiptr>>& bufferSizes() {
  static std::vector<std::pair<GLuint, GLsizeiptr>> v;
  return v;
}

static void zeroBuffer(GLenum target, GLuint b, GLsizeiptr size) {
  // WebGPU hands a buffer over zeroed and the signal path counts on it (the
  // flywheel's never-run servo state is zero); GL hands over garbage.
  // glClearBufferData is 4.3; uploading zeros works everywhere.
  static std::vector<uint8_t> zeros;
  if (static_cast<GLsizeiptr>(zeros.size()) < size) zeros.assign(static_cast<size_t>(size), 0);
  BindBuffer(target, b);
  BufferSubData(target, 0, size, zeros.data());
  BindBuffer(target, 0);
}

static GLuint makeBuffer(GLenum target, GLsizeiptr size) {
  GLuint b = 0;
  GenBuffers(1, &b);
  BindBuffer(target, b);
  BufferData(target, size, nullptr, DYNAMIC_DRAW);
  BindBuffer(target, 0);
  zeroBuffer(target, b, size);
  bufferSizes().push_back({b, size});
  return b;
}

static GLsizeiptr sizeOf(GLuint b) {
  for (const auto& p : bufferSizes())
    if (p.first == b) return p.second;
  return 0;
}

static GLuint makeTexture(int w, int h, GLenum fmt) {
  GLuint t = 0;
  GenTextures(1, &t);
  BindTexture(TEXTURE_2D, t);
  TexStorage2D(TEXTURE_2D, 1, fmt, w, h);
  TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
  TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
  TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
  TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
  BindTexture(TEXTURE_2D, 0);
  return t;
}

void Engine::clearTexture(unsigned tex) {
  BindFramebuffer(DRAW_FRAMEBUFFER, drawFbo_);
  FramebufferTexture2D(DRAW_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, tex, 0);
  ClearColor(0, 0, 0, 0);
  Clear(COLOR_BUFFER_BIT);
  FramebufferTexture2D(DRAW_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, 0, 0);
  BindFramebuffer(DRAW_FRAMEBUFFER, 0);
}

void Engine::createResources() {
  const GLsizeiptr N = N_SAMPLES;
  paramsUbo_ = makeBuffer(UNIFORM_BUFFER, kParamBytes);
  feedAUbo_ = makeBuffer(UNIFORM_BUFFER, kParamBytes);
  feedBUbo_ = makeBuffer(UNIFORM_BUFFER, kParamBytes);
  filterBuf_ = makeBuffer(SHADER_STORAGE_BUFFER, NUM_SECTIONS * FILTER_STRIDE * 4);
  uvfBBuf_ = makeBuffer(SHADER_STORAGE_BUFFER, N * 8);
  compA_ = makeBuffer(SHADER_STORAGE_BUFFER, N * 4);
  compB_ = makeBuffer(SHADER_STORAGE_BUFFER, N * 4);
  bComp_ = makeBuffer(SHADER_STORAGE_BUFFER, N * 4);
  compPrev_ = makeBuffer(SHADER_STORAGE_BUFFER, N * 4);
  progSnap_ = makeBuffer(SHADER_STORAGE_BUFFER, N * 4);
  chromaBuf_ = makeBuffer(SHADER_STORAGE_BUFFER, N * 4);
  underBuf_ = makeBuffer(SHADER_STORAGE_BUFFER, N * 4);
  lineInfoBuf_ = makeBuffer(SHADER_STORAGE_BUFFER, LINES * 16);
  lineParamsBuf_ = makeBuffer(SHADER_STORAGE_BUFFER, LINES * 16);
  timingBuf_ = makeBuffer(SHADER_STORAGE_BUFFER, (LINES * 2 + 13) * 4);
  syncMeasureBuf_ = makeBuffer(SHADER_STORAGE_BUFFER, LINES * 32);
  audioBuf_ = makeBuffer(SHADER_STORAGE_BUFFER, LINES * 4);
  ccBuf_ = makeBuffer(SHADER_STORAGE_BUFFER, CC_BUF_LEN * 4);
  cgBuf_ = makeBuffer(SHADER_STORAGE_BUFFER, CC_BUF_LEN * 4);
  for (GLuint b : {ccBuf_, cgBuf_}) {
    BindBuffer(SHADER_STORAGE_BUFFER, b);
    BufferSubData(SHADER_STORAGE_BUFFER, 0, sizeof kCaptionFont, kCaptionFont);
  }
  BindBuffer(SHADER_STORAGE_BUFFER, 0);
  persist_[0] = makeBuffer(SHADER_STORAGE_BUFFER, ACTIVE_WIDTH * ACTIVE_HEIGHT * 8);
  persist_[1] = makeBuffer(SHADER_STORAGE_BUFFER, ACTIVE_WIDTH * ACTIVE_HEIGHT * 8);

  texAW_ = 4;
  texAH_ = 3;
  texA_ = makeTexture(texAW_, texAH_, RGBA8);
  texB_ = makeTexture(ACTIVE_WIDTH, ACTIVE_HEIGHT, RGBA8);
  inputTex_ = makeTexture(ACTIVE_WIDTH, ACTIVE_HEIGHT, RGBA8);
  outTex_ = makeTexture(ACTIVE_WIDTH, ACTIVE_HEIGHT, RGBA8);
  // The decoded screen is stored sRGB-encoded while the gun transfer is
  // active, and crtFace reads it through an sRGB view so the sampler decodes.
  // Without glTextureView (4.3) crtFace reads the plain view; only presets
  // with the gun's cutoff/gamma up see the difference (a coarser dark end).
  if (TextureView != nullptr) {
    GenTextures(1, &outSrgb_);
    TextureView(outSrgb_, TEXTURE_2D, outTex_, SRGB8_ALPHA8, 0, 1, 0, 1);
  }
  BindTexture(TEXTURE_2D, outSrgb_);
  TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
  TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
  TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
  TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
  BindTexture(TEXTURE_2D, 0);
  faceTex_ = makeTexture(ACTIVE_WIDTH, ACTIVE_HEIGHT, RGBA8);
  grainTex_ = makeTexture(ACTIVE_WIDTH, ACTIVE_HEIGHT, R32F);

  GenSamplers(1, &sampler_);
  SamplerParameteri(sampler_, TEXTURE_MIN_FILTER, LINEAR);
  SamplerParameteri(sampler_, TEXTURE_MAG_FILTER, LINEAR);
  SamplerParameteri(sampler_, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
  SamplerParameteri(sampler_, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
  GenVertexArrays(1, &vao_);
  GenFramebuffers(1, &readFbo_);
  GenFramebuffers(1, &drawFbo_);

  for (GLuint t : {texA_, texB_, inputTex_, outTex_, faceTex_}) clearTexture(t);
}

void Engine::destroyResources() {
  GLuint bufs[] = {paramsUbo_, feedAUbo_, feedBUbo_, filterBuf_, uvfBBuf_, compA_, compB_, bComp_, compPrev_,
                   progSnap_, chromaBuf_, underBuf_, lineInfoBuf_, lineParamsBuf_, timingBuf_, syncMeasureBuf_,
                   audioBuf_, ccBuf_, cgBuf_, persist_[0], persist_[1]};
  DeleteBuffers(static_cast<GLsizei>(sizeof bufs / sizeof bufs[0]), bufs);
  bufferSizes().clear();
  GLuint texs[] = {texA_, texB_, inputTex_, outSrgb_, outTex_, faceTex_, grainTex_};
  DeleteTextures(static_cast<GLsizei>(sizeof texs / sizeof texs[0]), texs);
  DeleteSamplers(1, &sampler_);
  DeleteVertexArrays(1, &vao_);
  DeleteFramebuffers(1, &readFbo_);
  DeleteFramebuffers(1, &drawFbo_);
  for (int i = 0; i < kNumPrograms; i++)
    if (programs_[i].id) DeleteProgram(programs_[i].id);
}

bool Engine::build(GetProcFn getProc) {
  if (const char* missing = load(getProc)) {
    error_ = std::string("OpenGL function not available: ") + missing;
    return false;
  }
  auto str = [](GLenum n) {
    const GLubyte* s = GetString(n);
    return s ? std::string(reinterpret_cast<const char*>(s)) : std::string("?");
  };
  glInfo_ = str(VENDOR) + " | " + str(RENDERER) + " | " + str(VERSION);
  GLint major = 0, minor = 0;
  GetIntegerv(0x821B, &major);  // GL_MAJOR_VERSION
  GetIntegerv(0x821C, &minor);  // GL_MINOR_VERSION
  glInfo_ += " (context " + std::to_string(major) + "." + std::to_string(minor) + ")";
  if (major < 4 || (major == 4 && minor < 1)) {
    error_ = "needs OpenGL 4.1 or newer; context is " + std::to_string(major) + "." + std::to_string(minor);
    return false;
  }
  if (major == 4 && minor < 3) {
    // Compute shaders are core from 4.3; below that they have to come in as
    // extensions, which Windows drivers still offer inside a 4.1 context.
    legacy_ = true;
    GLint n = 0;
    GetIntegerv(NUM_EXTENSIONS, &n);
    auto has = [&](const char* ext) {
      for (GLint i = 0; i < n; i++) {
        const GLubyte* s = GetStringi(EXTENSIONS, static_cast<GLuint>(i));
        if (s && std::strcmp(reinterpret_cast<const char*>(s), ext) == 0) return true;
      }
      return false;
    };
    std::string missing;
    for (const char* ext : {"GL_ARB_compute_shader", "GL_ARB_shader_storage_buffer_object", "GL_ARB_shader_image_load_store",
                            "GL_ARB_shading_language_420pack", "GL_ARB_texture_storage", "GL_ARB_shading_language_packing"}) {
      if (!has(ext)) missing += std::string(missing.empty() ? "" : ", ") + ext;
    }
    if (!missing.empty()) {
      error_ = "this OpenGL " + std::to_string(major) + "." + std::to_string(minor) +
               " context lacks compute-shader support (missing " + missing + ")";
      return false;
    }
  }
  if (!buildPrograms()) return false;
  createResources();
  // The phosphor grain is fixed to the glass: baked once, read as a texel.
  const Program& gb = programs_[progIndex("grain_bake.main")];
  UseProgram(gb.id);
  BindImageTexture(0, grainTex_, 0, 0, 0, WRITE_ONLY, R32F);
  DispatchCompute((ACTIVE_WIDTH + 7) / 8, (ACTIVE_HEIGHT + 7) / 8, 1);
  MemBarrier(ALL_BARRIER_BITS);
  BindImageTexture(0, 0, 0, 0, 0, WRITE_ONLY, R32F);
  UseProgram(0);
  setCaption("VIDEO SKILLET");
  if (GetError() != NO_ERROR_) {
    // errors during setup are reported but not fatal
  }
  return true;
}

void Engine::setCaption(const std::string& text) {
  chain_->captionState.setText(text);
  const std::vector<uint32_t> page = buildPage(text);
  BindBuffer(SHADER_STORAGE_BUFFER, cgBuf_);
  BufferSubData(SHADER_STORAGE_BUFFER, CC_PAGE * 4, static_cast<GLsizeiptr>(page.size() * 4), page.data());
  BindBuffer(SHADER_STORAGE_BUFFER, 0);
}

void Engine::setSourceB(SourceB kind) {
  sourceB_ = kind;
  if (kind == SourceB::ColorBars) barsLoaded_ = false;
}

// SMPTE-style colour bars at 75%, the reference a studio would put on B.
void Engine::uploadBars() {
  std::vector<uint8_t> px(ACTIVE_WIDTH * ACTIVE_HEIGHT * 4);
  const uint8_t top[7][3] = {{191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0}, {191, 0, 191}, {191, 0, 0}, {0, 0, 191}};
  const uint8_t mid[7][3] = {{0, 0, 191}, {19, 19, 19}, {191, 0, 191}, {19, 19, 19}, {0, 191, 191}, {19, 19, 19}, {191, 191, 191}};
  const uint8_t low[6][3] = {{0, 33, 76}, {255, 255, 255}, {50, 0, 106}, {19, 19, 19}, {9, 9, 9}, {19, 19, 19}};
  for (int y = 0; y < ACTIVE_HEIGHT; y++) {
    for (int x = 0; x < ACTIVE_WIDTH; x++) {
      const uint8_t* c;
      const int bar = x * 7 / ACTIVE_WIDTH;
      if (y < ACTIVE_HEIGHT * 2 / 3) c = top[bar];
      else if (y < ACTIVE_HEIGHT * 3 / 4) c = mid[bar];
      else {
        const int q = x * 6 / ACTIVE_WIDTH;
        c = low[std::min(q, 5)];
      }
      uint8_t* p = &px[(y * ACTIVE_WIDTH + x) * 4];
      p[0] = c[0];
      p[1] = c[1];
      p[2] = c[2];
      p[3] = 255;
    }
  }
  BindTexture(TEXTURE_2D, texB_);
  PixelStorei(UNPACK_ALIGNMENT, 1);
  TexSubImage2D(TEXTURE_2D, 0, 0, 0, ACTIVE_WIDTH, ACTIVE_HEIGHT, RGBA, UNSIGNED_BYTE, px.data());
  PixelStorei(UNPACK_ALIGNMENT, 4);
  BindTexture(TEXTURE_2D, 0);
  barsLoaded_ = true;
}

// Source A: the host frame, copied into the slot texture with row 0 at the
// top (the raster's orientation), capped at 1536 on its long edge like the
// app's own uploads.
void Engine::stageInput(const InputFrame& in) {
  if (in.texture == 0 || in.width <= 0 || in.height <= 0) {
    srcAspect_ = 4.0 / 3.0;
    return;
  }
  const double s = std::min(1.0, static_cast<double>(MAX_SRC_EDGE) / std::max(in.width, in.height));
  const int w = std::max(1, static_cast<int>(std::lround(in.width * s)));
  const int h = std::max(1, static_cast<int>(std::lround(in.height * s)));
  if (w != texAW_ || h != texAH_) {
    DeleteTextures(1, &texA_);
    texA_ = makeTexture(w, h, RGBA8);
    texAW_ = w;
    texAH_ = h;
  }
  srcAspect_ = static_cast<double>(in.width) / in.height;
  BindFramebuffer(READ_FRAMEBUFFER, readFbo_);
  FramebufferTexture2D(READ_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, in.texture, 0);
  BindFramebuffer(DRAW_FRAMEBUFFER, drawFbo_);
  FramebufferTexture2D(DRAW_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, texA_, 0);
  if (in.bottomUp) BlitFramebuffer(0, 0, in.width, in.height, 0, h, w, 0, COLOR_BUFFER_BIT, LINEAR);
  else BlitFramebuffer(0, 0, in.width, in.height, 0, 0, w, h, COLOR_BUFFER_BIT, LINEAR);
  FramebufferTexture2D(READ_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, 0, 0);
  FramebufferTexture2D(DRAW_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, 0, 0);
  BindFramebuffer(READ_FRAMEBUFFER, 0);
  BindFramebuffer(DRAW_FRAMEBUFFER, 0);
}

// Source B when it is the layer itself: cover-fit to 4:3 at raster size, the
// way the app stages a picture into slot B.
void Engine::stageSourceB(const InputFrame& in) {
  if (sourceB_ == SourceB::ColorBars) {
    if (!barsLoaded_) uploadBars();
    return;
  }
  if (sourceB_ != SourceB::LayerCopy || in.texture == 0 || in.width <= 0 || in.height <= 0) return;
  const double w = in.width, h = in.height;
  const bool wide = w / h > 4.0 / 3.0;
  const double sw = wide ? h * (4.0 / 3.0) : w;
  const double sh = wide ? h : w * (3.0 / 4.0);
  const int x0 = static_cast<int>((w - sw) / 2), y0 = static_cast<int>((h - sh) / 2);
  const int x1 = static_cast<int>(x0 + sw), y1 = static_cast<int>(y0 + sh);
  BindFramebuffer(READ_FRAMEBUFFER, readFbo_);
  FramebufferTexture2D(READ_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, in.texture, 0);
  BindFramebuffer(DRAW_FRAMEBUFFER, drawFbo_);
  FramebufferTexture2D(DRAW_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, texB_, 0);
  if (in.bottomUp) BlitFramebuffer(x0, y0, x1, y1, 0, ACTIVE_HEIGHT, ACTIVE_WIDTH, 0, COLOR_BUFFER_BIT, LINEAR);
  else BlitFramebuffer(x0, y0, x1, y1, 0, 0, ACTIVE_WIDTH, ACTIVE_HEIGHT, COLOR_BUFFER_BIT, LINEAR);
  FramebufferTexture2D(READ_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, 0, 0);
  FramebufferTexture2D(DRAW_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, 0, 0);
  BindFramebuffer(READ_FRAMEBUFFER, 0);
  BindFramebuffer(DRAW_FRAMEBUFFER, 0);
}

unsigned Engine::bufOf(int r) const {
  const FrameGates& g = chain_->gates;
  switch (r) {
    case R_PARAMS: return paramsUbo_;
    case R_FEEDA: return feedAUbo_;
    case R_FEEDB: return feedBUbo_;
    case R_FILTERS: return filterBuf_;
    case R_UVFB: return uvfBBuf_;
    case R_COMPA: return compA_;
    case R_COMPB: return compB_;
    case R_BCOMP: return bComp_;
    case R_COMPPREV: return compPrev_;
    case R_PROGSNAP: return progSnap_;
    case R_CHROMA: return chromaBuf_;
    case R_UNDER: return underBuf_;
    case R_LINEINFO: return lineInfoBuf_;
    case R_LINEPARAMS: return lineParamsBuf_;
    case R_TIMING: return timingBuf_;
    case R_SYNCMEASURE: return syncMeasureBuf_;
    case R_AUDIO: return audioBuf_;
    case R_CC: return ccBuf_;
    case R_CG: return cgBuf_;
    // decode's phosphor halves swap by the parity of the frame it decodes
    case R_PERSIST_READ: return persist_[chain_->frame % 2];
    case R_PERSIST_WRITE: return persist_[(chain_->frame + 1) % 2];
    case R_ENC_DST: return g.aFeed ? compB_ : compA_;
    case R_ENCB_DST: return g.bFeed ? compB_ : bComp_;
  }
  return 0;
}

unsigned Engine::texOf(int r) const {
  switch (r) {
    case T_SRCA: return texA_;
    case T_SRCB: return texB_;
    case T_INPUT: return inputTex_;
    case T_OUT: return outTex_;
    case T_OUT_CRT: return chain_->gates.crtSrgb && outSrgb_ ? outSrgb_ : outTex_;
    case T_FACE: return faceTex_;
    case T_GRAIN: return grainTex_;
  }
  return 0;
}

void Engine::bindPass(const Pass& p, int) {
  const Program& pr = programs_[p.prog];
  UseProgram(pr.id);
  for (int i = 0; i < pr.def->numBindings; i++) {
    const ShaderBinding& b = pr.def->bindings[i];
    if (b.binding < 0 || b.binding >= static_cast<int>(p.res.size())) continue;
    const int r = p.res[b.binding];
    switch (b.kind) {
      case BindKind::Ubo: BindBufferBase(UNIFORM_BUFFER, b.unit, bufOf(r)); break;
      case BindKind::Ssbo: BindBufferBase(SHADER_STORAGE_BUFFER, b.unit, bufOf(r)); break;
      case BindKind::Tex:
        ActiveTexture(TEXTURE0 + b.unit);
        BindTexture(TEXTURE_2D, texOf(r));
        BindSampler(b.unit, sampler_);
        break;
      case BindKind::Image: {
        const GLuint t = texOf(r);
        BindImageTexture(b.unit, t, 0, 0, 0, WRITE_ONLY, t == grainTex_ ? R32F : RGBA8);
        break;
      }
      case BindKind::Sampler: break;
    }
  }
}

void Engine::dispatch(const Pass& p) {
  bindPass(p, 0);
  DispatchCompute(p.x, p.y, 1);
  // Every pass reads what the one before it wrote; WebGPU orders that between
  // compute passes on its own, GL needs the barrier said out loud.
  MemBarrier(ALL_BARRIER_BITS);
}

void Engine::runSimulation(const InputFrame& in) {
  SignalChain& ch = *chain_;
  const FrameGates& g = ch.gates;
  const unsigned perLine[2] = {(SAMPLES_PER_LINE + 63) / 64, LINES};
  const unsigned perLineT[2] = {(SAMPLES_PER_LINE + kTileWG - 1) / kTileWG, LINES};
  const unsigned perPixelT[2] = {(ACTIVE_WIDTH + kTileWG - 1) / kTileWG, ACTIVE_HEIGHT};
  const unsigned perTile[2] = {(ACTIVE_WIDTH + 7) / 8, (ACTIVE_HEIGHT + 7) / 8};
  const unsigned perRow[2] = {(LINES + 63) / 64, 1};
  auto P = [&](const char* name, std::vector<int> res, const unsigned* d) {
    return Pass{progIndex(name), std::move(res), d[0], d[1]};
  };
  const unsigned one[2] = {1, 1};

  // uploads for this frame
  BindBuffer(UNIFORM_BUFFER, paramsUbo_);
  BufferSubData(UNIFORM_BUFFER, 0, kParamBytes, ch.params);
  if (g.aFeed) {
    BindBuffer(UNIFORM_BUFFER, feedAUbo_);
    BufferSubData(UNIFORM_BUFFER, 0, kParamBytes, ch.feedParamsA);
  }
  if (g.bFeed) {
    BindBuffer(UNIFORM_BUFFER, feedBUbo_);
    BufferSubData(UNIFORM_BUFFER, 0, kParamBytes, ch.feedParamsB);
  }
  BindBuffer(UNIFORM_BUFFER, 0);
  if (ch.filtersFresh) {
    BindBuffer(SHADER_STORAGE_BUFFER, filterBuf_);
    BufferSubData(SHADER_STORAGE_BUFFER, 0, sizeof ch.filters, ch.filters);
  }
  BindBuffer(SHADER_STORAGE_BUFFER, lineParamsBuf_);
  BufferSubData(SHADER_STORAGE_BUFFER, 0, sizeof ch.lineParams[0], ch.lineParams[0]);
  BindBuffer(SHADER_STORAGE_BUFFER, 0);
  stageSourceB(in);

  // prePasses
  dispatch(P("compose.main", {R_PARAMS, T_SRCA, T_FACE, S_LINEAR, T_INPUT, R_TIMING}, perTile));
  dispatch(P("encode_composite.main", {R_PARAMS, R_FILTERS, T_INPUT, R_ENC_DST}, perLineT));
  if (g.aFeed) dispatch(P("feed.main", {R_FEEDA, R_COMPB, R_COMPA}, perLine));
  if (g.composeB) dispatch(P("compose_b.main", {R_PARAMS, T_SRCB}, perTile));
  if (g.bChain) dispatch(P("encode_chroma_b.main", {R_PARAMS, R_FILTERS, T_SRCB, R_UVFB}, perPixelT));
  if (g.bWave) dispatch(P("encode_composite_b.main", {R_PARAMS, T_SRCB, R_UVFB, R_ENCB_DST}, perLine));
  if (g.bFeed) dispatch(P("feed.main", {R_FEEDB, R_COMPB, R_BCOMP}, perLine));
  if (g.bChain) dispatch(P("mix_b.main", {R_PARAMS, T_SRCB, R_UVFB, R_COMPA, R_BCOMP, R_COMPPREV}, perLine));
  if (g.chyron) dispatch(P("chyron.main", {R_PARAMS, R_COMPA, R_CG}, perLine));
  if (g.fbComposite) {
    if (g.progSnap) {
      BindBuffer(COPY_READ_BUFFER, compA_);
      BindBuffer(COPY_WRITE_BUFFER, progSnap_);
      CopyBufferSubData(COPY_READ_BUFFER, COPY_WRITE_BUFFER, 0, 0, N_SAMPLES * 4);
      BindBuffer(COPY_READ_BUFFER, 0);
      BindBuffer(COPY_WRITE_BUFFER, 0);
      MemBarrier(ALL_BARRIER_BITS);
    }
    dispatch(P("fb_composite.main", {R_PARAMS, R_COMPPREV, R_COMPA, R_PROGSNAP}, perLine));
  }

  // loopPasses, once per tape generation
  for (int gen = 0; gen < g.gens; gen++) {
    if (gen > 0) {
      BindBuffer(UNIFORM_BUFFER, paramsUbo_);
      BufferSubData(UNIFORM_BUFFER, 0, kParamBytes, ch.genParams[gen]);
      BindBuffer(UNIFORM_BUFFER, 0);
      BindBuffer(SHADER_STORAGE_BUFFER, lineParamsBuf_);
      BufferSubData(SHADER_STORAGE_BUFFER, 0, sizeof ch.lineParams[gen], ch.lineParams[gen]);
      BindBuffer(SHADER_STORAGE_BUFFER, 0);
    }
    dispatch(P("chroma_extract.main", {R_FILTERS, R_COMPA, R_CHROMA}, perLineT));
    if (g.underDown) dispatch(P("under_down.main", {R_FILTERS, R_CHROMA, R_LINEPARAMS, R_UNDER}, perLineT));
    dispatch(P("channel.main", {R_PARAMS, R_FILTERS, R_COMPA, R_CHROMA, R_UNDER, R_LINEPARAMS, R_COMPB, R_AUDIO}, perLineT));
    dispatch(P("timebase.main", {R_LINEPARAMS, R_COMPB, R_COMPA}, perLine));
  }
  // the receiver decodes the frame, not the last tape generation
  if (g.gens > 1) {
    BindBuffer(UNIFORM_BUFFER, paramsUbo_);
    BufferSubData(UNIFORM_BUFFER, 0, kParamBytes, ch.params);
    BindBuffer(UNIFORM_BUFFER, 0);
  }

  // postPasses
  if (g.enhancer) dispatch(P("enhancer.main", {R_PARAMS, R_COMPA}, perRow));
  dispatch(P("sync_measure.main", {R_PARAMS, R_COMPA, R_TIMING, R_SYNCMEASURE}, perRow));
  dispatch(P("sync.main", {R_PARAMS, R_SYNCMEASURE, R_TIMING, R_AUDIO}, one));
  dispatch(P("line_analyze.main", {R_PARAMS, R_COMPA, R_TIMING, R_LINEINFO}, perRow));
  if (g.vir) dispatch(P("vir.main", {R_PARAMS, R_COMPA, R_LINEINFO, R_TIMING}, one));
  if (g.caption) dispatch(P("caption.main", {R_PARAMS, R_COMPA, R_TIMING, R_CC}, one));
  dispatch(P("decode.main", {R_PARAMS, R_FILTERS, R_COMPA, R_LINEINFO, R_TIMING, T_OUT, R_PERSIST_READ, R_PERSIST_WRITE, R_AUDIO, R_CC}, perPixelT));
  dispatch(P("crt_face.main", {R_PARAMS, T_OUT_CRT, S_LINEAR, T_FACE, R_TIMING, T_GRAIN}, perTile));
  if (g.storePrev) dispatch(P("store_prev.main", {R_PARAMS, R_COMPA, R_COMPPREV}, perLine));
  haveFace_ = true;
}

void Engine::present(const OutputTarget& out, double, double) {
  const int fs = progIndex("present.fs");
  UseProgram(programs_[fs].id);
  BindBufferBase(UNIFORM_BUFFER, 0, paramsUbo_);
  ActiveTexture(TEXTURE0);
  BindTexture(TEXTURE_2D, faceTex_);
  BindSampler(0, sampler_);
  BindFramebuffer(FRAMEBUFFER, out.fbo);
  Viewport(out.x, out.y, out.width, out.height);
  Disable(BLEND);
  Disable(DEPTH_TEST);
  Disable(SCISSOR_TEST);
  Disable(CULL_FACE);
  Disable(FRAMEBUFFER_SRGB);
  BindVertexArray(vao_);
  DrawArrays(TRIANGLES, 0, 3);
  BindVertexArray(0);
}

void Engine::render(const InputFrame& in, const OutputTarget& out, double nowMs) {
  if (!ok_) return;
  if (srcNoiseA_ == 0) stageInput(in);
  FrameEnv env;
  env.nowMs = nowMs;
  // Fill: tell the tube the canvas is 4:3 so it covers the whole viewport.
  env.canvasW = out.fill ? out.height * 4.0 / 3.0 : out.width;
  env.canvasH = out.height;
  env.srcAspect = srcNoiseA_ == 0 ? srcAspect_ : 4.0 / 3.0;
  env.srcMirror = mirror_ ? 1 : 0;
  env.srcNoise = srcNoiseA_;
  const int b = static_cast<int>(sourceB_);
  env.srcNoiseB = b >= 1 && b <= 3 ? b : 0;
  env.bEnabled = sourceB_ != SourceB::Off;
  if (chain_->step(env)) {
    runSimulation(in);
    renderedFrames_++;
  } else if (out.fill ? false : false) {
    // (a held frame re-presents the face as it stands)
  }
  // The canvas size lives in the uniform block; keep it current even on a
  // held frame so a resized output never draws with a stale aspect.
  {
    float cw = static_cast<float>(env.canvasW), chh = static_cast<float>(env.canvasH);
    BindBuffer(UNIFORM_BUFFER, paramsUbo_);
    BufferSubData(UNIFORM_BUFFER, P_canvasW * 4, 4, &cw);
    BufferSubData(UNIFORM_BUFFER, P_canvasH * 4, 4, &chh);
    BindBuffer(UNIFORM_BUFFER, 0);
  }
  present(out, env.canvasW, env.canvasH);

  // hand the context back in its default state (FFGL's rule), apart from the
  // caller's framebuffer, which stays bound
  UseProgram(0);
  for (int u = 0; u < 8; u++) {
    ActiveTexture(TEXTURE0 + u);
    BindTexture(TEXTURE_2D, 0);
    BindSampler(u, 0);
    BindImageTexture(u, 0, 0, 0, 0, READ_ONLY, RGBA8);
    BindBufferBase(SHADER_STORAGE_BUFFER, u, 0);
    BindBufferBase(UNIFORM_BUFFER, u, 0);
  }
  ActiveTexture(TEXTURE0);
  BindBuffer(SHADER_STORAGE_BUFFER, 0);
  BindBuffer(UNIFORM_BUFFER, 0);
}

void Engine::resetSignal() {
  if (!ok_) return;
  for (GLuint b : {paramsUbo_, feedAUbo_, feedBUbo_}) {
    BindBuffer(UNIFORM_BUFFER, b);
    zeroBuffer(UNIFORM_BUFFER, b, sizeOf(b));
  }
  BindBuffer(UNIFORM_BUFFER, 0);
  for (GLuint b : {filterBuf_, uvfBBuf_, compA_, compB_, bComp_, compPrev_, progSnap_, chromaBuf_, underBuf_,
                   lineInfoBuf_, lineParamsBuf_, timingBuf_, syncMeasureBuf_, audioBuf_, persist_[0], persist_[1]}) {
    BindBuffer(SHADER_STORAGE_BUFFER, b);
    zeroBuffer(SHADER_STORAGE_BUFFER, b, sizeOf(b));
  }
  BindBuffer(SHADER_STORAGE_BUFFER, 0);
  for (GLuint t : {inputTex_, outTex_, faceTex_}) clearTexture(t);
  // restart the CPU side with the same board and caption
  const Controls keep = chain_->controls;
  const std::string caption = chain_->captionState.text();
  chain_.reset(new SignalChain(static_cast<uint32_t>(renderedFrames_ * 2654435761u + 1)));
  chain_->controls = keep;
  chain_->captionState.setText(caption);
}

} // namespace skillet
