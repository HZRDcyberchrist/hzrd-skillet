// Generators and face passes for the Resolume port. See extras.h.
#include "extras.h"

#include <algorithm>
#include <cmath>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_NO_STDIO_WIN32_UTF8_DEFAULT
#include "../third_party/stb/stb_image.h"

#include "signal.h"

namespace skillet {
using namespace gl;

// one triangle that covers the target; vUv runs 0..1 over it
static const char* kVs = R"GLSL(#version 410 core
out vec2 vUv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  vUv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// ── Gothic tracery ──
// Stone is pale limestone; glass takes a jewel palette and a slow travelling
// shaft of light; lead cames quarter the glass into quarries.
static const char* kTraceryFs = R"GLSL(#version 410 core
in vec2 vUv;
out vec4 o;
uniform int mode;
uniform float t;
uniform float drift;
uniform float aspect;
const float PI = 3.14159265;

float hash1(float n) { return fract(sin(n * 12.9898) * 43758.5453); }
float hash2(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash2(i), hash2(i + vec2(1, 0)), f.x), mix(hash2(i + vec2(0, 1)), hash2(i + vec2(1, 1)), f.x), f.y);
}
vec3 jewel(float id) {
  float k = hash1(id + 3.7);
  if (k < 0.30) return vec3(0.66, 0.04, 0.06);   // ruby
  if (k < 0.55) return vec3(0.05, 0.13, 0.58);   // sapphire
  if (k < 0.68) return vec3(0.04, 0.40, 0.17);   // emerald
  if (k < 0.84) return vec3(0.80, 0.52, 0.07);   // amber
  return vec3(0.36, 0.07, 0.46);                  // amethyst
}
// light through the glass: a slow diagonal shaft plus shimmer
float light(vec2 p) {
  float shaft = 0.5 + 0.5 * sin((p.x * 0.8 + p.y * 1.3) * 2.2 - t * 0.35);
  float shimmer = vnoise(p * 9.0 + vec2(t * 0.6, -t * 0.4));
  return 0.35 + 0.75 * shaft * shaft + 0.25 * shimmer;
}
vec3 glass(float id, vec2 p, vec2 q) {
  // quarries: a diamond lattice of lead cames
  vec2 d = abs(fract(vec2(q.x + q.y, q.x - q.y) * 9.0) - 0.5);
  float came = smoothstep(0.06, 0.025, min(d.x, d.y));
  vec3 c = jewel(id) * light(p) * (0.85 + 0.3 * hash2(floor(vec2(q.x + q.y, q.x - q.y) * 9.0) + id));
  return mix(c, vec3(0.02), came * 0.85);
}
vec3 stone(vec2 p, float lit) {
  float n = vnoise(p * 40.0) * 0.12 + vnoise(p * 7.0) * 0.1;
  return vec3(0.80, 0.76, 0.68) * (lit + n);
}
// A pointed (equilateral) arch of half-width a springing at y = 0: negative
// inside. Below the springing it is a straight-sided opening.
float archSd(vec2 p, float a) {
  if (p.y < 0.0) return abs(p.x) - a;
  return max(length(p - vec2(a, 0.0)) - 2.0 * a, length(p + vec2(a, 0.0)) - 2.0 * a);
}
float foilSd(vec2 p, float r, int n) {
  float d = 1e9;
  for (int i = 0; i < 8; i++) {
    if (i >= n) break;
    float a = float(i) * 2.0 * PI / float(n);
    d = min(d, length(p - r * vec2(cos(a), sin(a))) - r);
  }
  return d;
}
float line(float d, float w) { return smoothstep(w, w * 0.6, abs(d)); }

vec3 roseWindow(vec2 uv) {
  vec2 p = (uv - 0.5) * vec2(aspect, 1.0) * 2.15;
  float spin = t * drift * 0.08;
  p = mat2(cos(spin), -sin(spin), sin(spin), cos(spin)) * p;
  float r = length(p);
  if (r > 1.0) {
    // the wall around the window: dark masonry
    vec2 b = vec2(p.x * 5.0 + floor(p.y * 9.0) * 0.5, p.y * 9.0);
    float joint = min(abs(fract(b.x) - 0.5), abs(fract(b.y) - 0.5));
    return vec3(0.09, 0.085, 0.08) * (0.7 + 0.5 * vnoise(p * 30.0)) * (joint < 0.04 ? 0.5 : 1.0);
  }
  const float N = 12.0;
  float sector = 2.0 * PI / N;
  float a = atan(p.y, p.x);
  float si = floor((a + PI) / sector);
  float as = mod(a + sector * 0.5, sector) - sector * 0.5;
  vec2 q = r * vec2(cos(as), sin(as));
  float s = 0.0;
  s = max(s, line(r - 0.97, 0.035));                                  // outer ring
  s = max(s, line(r - 0.30, 0.025));                                  // inner ring
  float spoke = r * abs(sin(abs(as) - sector * 0.5));
  if (r > 0.30) s = max(s, smoothstep(0.022, 0.012, spoke));          // mullions
  float petal = length(q - vec2(0.66, 0.0)) - 0.21;
  s = max(s, line(petal, 0.022));                                     // petal rings
  float tre = foilSd(q - vec2(0.66, 0.0), 0.085, 3);
  if (petal < 0.0) s = max(s, line(tre, 0.014));                      // trefoil in each petal
  float lancet = archSd(vec2(q.y, q.x - 0.36) / 1.0, 0.07);
  if (petal > 0.0 && r < 0.97) s = max(s, line(lancet, 0.012));       // small lancets
  float quat = foilSd(p, 0.1, 4);
  if (r < 0.30) s = max(s, line(quat, 0.014));                        // quatrefoil oculus
  float id = r < 0.30 ? (quat < 0.0 ? 1.0 : 2.0) : si * 4.0 + (petal < 0.0 ? (tre < 0.0 ? 5.0 : 6.0) : (lancet < 0.0 ? 7.0 : 8.0));
  vec3 g = glass(id, p, q);
  return mix(g, stone(p, 0.95), s);
}

vec3 lancetArcade(vec2 uv) {
  vec2 p = vec2(uv.x * aspect + t * drift * 0.05, uv.y);
  const float W = 0.46;
  float cell = floor(p.x / W);
  vec2 l = vec2(mod(p.x, W) - W * 0.5, p.y);
  float a = 0.17, y0 = 0.52;
  float big = archSd(vec2(l.x, l.y - y0), a);
  float bottom = 0.08;
  if (l.y < bottom || big > 0.0) {
    // wall: coursed ashlar, lit a little from the windows
    vec2 b = vec2(p.x * 7.0 + floor(p.y * 14.0) * 0.5, p.y * 14.0);
    float joint = min(abs(fract(b.x) - 0.5), abs(fract(b.y) - 0.5));
    vec3 w = vec3(0.30, 0.28, 0.25) * (0.75 + 0.4 * vnoise(p * 25.0));
    w *= joint < 0.045 ? 0.55 : 1.0;
    float rim = line(big, 0.02);
    return mix(w, stone(p, 1.0), rim);
  }
  // two lancets and an oculus inside the arch (Y/geometric tracery)
  float la = archSd(vec2(abs(l.x) - a * 0.5, l.y - (y0 - 0.02)), a * 0.5 - 0.02);
  vec2 oc = vec2(l.x, l.y - (y0 + 0.19));
  float ring = length(oc) - 0.075;
  float foil = foilSd(oc, 0.035, 4);
  float s = line(big, 0.02);
  s = max(s, line(la, 0.012));
  s = max(s, line(ring, 0.012));
  if (ring < 0.0) s = max(s, line(foil, 0.009));
  bool inGlass = la < 0.0 || ring < 0.0;
  float id = cell * 7.0 + (la < 0.0 ? (l.x < 0.0 ? 1.0 : 2.0) : (foil < 0.0 ? 3.0 : 4.0));
  vec3 c = inGlass ? glass(id, p, l) : stone(p, 0.7);
  return mix(c, stone(p, 1.0), s);
}

vec3 quatrefoil(vec2 uv) {
  vec2 p = vec2(uv.x * aspect, uv.y) + t * drift * vec2(0.03, 0.02);
  vec2 g = p * 5.0;
  g = mat2(0.7071, -0.7071, 0.7071, 0.7071) * g;  // diaper runs on the diagonal
  vec2 cell = floor(g);
  vec2 q = fract(g) - 0.5;
  float d = foilSd(q, 0.17, 4);
  float s = line(d, 0.03);
  // gilded ground with a dot at each crossing
  float dot_ = smoothstep(0.07, 0.05, length(abs(q) - 0.5));
  vec3 ground = vec3(0.55, 0.40, 0.12) * (0.7 + 0.5 * vnoise(p * 30.0) + 0.3 * sin(t * 0.7 + p.x * 3.0));
  ground = mix(ground, vec3(0.85, 0.72, 0.35), dot_);
  vec3 c = d < 0.0 ? glass(cell.x * 13.0 + cell.y * 7.0, p, q) : ground;
  return mix(c, stone(p, 0.95), s);
}

void main() {
  vec3 c = mode == 1 ? roseWindow(vUv) : mode == 2 ? lancetArcade(vUv) : quatrefoil(vUv);
  o = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)GLSL";

static const char* kPatternFs = R"GLSL(#version 410 core
in vec2 vUv;
out vec4 o;
uniform sampler2D img;
uniform float t;
uniform float drift;
uniform float aspect;
uniform float imgAspect;
uniform float tiles;
void main() {
  vec2 p = vec2(vUv.x * aspect / imgAspect, vUv.y) * tiles + t * drift * vec2(0.03, 0.02);
  o = vec4(texture(img, p).rgb, 1.0);
}
)GLSL";

// ── Rose: the face wrapped into a disc ──
static const char* kRoseFs = R"GLSL(#version 410 core
in vec2 vUv;
out vec4 o;
uniform sampler2D face;
uniform float amount;
uniform float folds;
uniform float angle;
const float PI = 3.14159265;
void main() {
  vec2 p = (vUv - 0.5) * vec2(4.0 / 3.0, 1.0);
  float r = length(p) / 0.5;
  float th = atan(p.y, p.x) + angle;
  float x;
  if (folds > 0.5) {
    // mirrored sectors: each one holds the frame's width twice, flipped
    float sector = 2.0 * PI / folds;
    float a = mod(th, sector);
    a = min(a, sector - a);
    x = a / (sector * 0.5);
  } else {
    x = fract(th / (2.0 * PI) + 1.0);
  }
  vec2 polar = vec2(x, clamp(r, 0.0, 1.0));
  vec2 uv = mix(vUv, polar, amount);
  float edge = mix(1.0, smoothstep(1.0, 0.96, r), amount);
  o = vec4(texture(face, uv).rgb * edge, 1.0);
}
)GLSL";

static const char* kBurnFs = R"GLSL(#version 410 core
in vec2 vUv;
out vec4 o;
uniform sampler2D face;
uniform float weight;
void main() { o = vec4(texture(face, vUv).rgb, weight); }
)GLSL";

// ── Finish: shroud and vigil over the face ──
static const char* kFinishFs = R"GLSL(#version 410 core
in vec2 vUv;
out vec4 o;
uniform sampler2D face;
uniform sampler2D shroud;
uniform float level;
uniform float vigil;
uniform float flame;
void main() {
  vec3 c = texture(face, vUv).rgb;
  // the ghost: a softened, linen-toned luminance of everything burned in
  vec2 px = vec2(1.0 / 754.0, 1.0 / 480.0);
  vec3 s = texture(shroud, vUv).rgb * 0.4;
  s += (texture(shroud, vUv + vec2(px.x, 0)).rgb + texture(shroud, vUv - vec2(px.x, 0)).rgb +
        texture(shroud, vUv + vec2(0, px.y)).rgb + texture(shroud, vUv - vec2(0, px.y)).rgb) * 0.15;
  float l = dot(s, vec3(0.299, 0.587, 0.114));
  vec3 tone = l * vec3(1.0, 0.84, 0.62);
  c = 1.0 - (1.0 - c) * (1.0 - clamp(tone * level, 0.0, 1.0));
  // vigil: candle-lit, warm, breathing at the edges
  vec2 d = vUv - 0.5;
  float fall = 1.0 - (0.55 + 0.25 * flame) * dot(d, d) * 2.2;
  float dim = (0.78 + 0.18 * flame) * fall;
  vec3 warm = c * vec3(1.08, 0.88, 0.66);
  c = mix(c, warm * dim, vigil);
  o = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)GLSL";

static unsigned compileStage(GLenum stage, const char* src, std::string& log) {
  GLuint s = CreateShader(stage);
  ShaderSource(s, 1, &src, nullptr);
  CompileShader(s);
  GLint ok = 0;
  GetShaderiv(s, COMPILE_STATUS, &ok);
  if (!ok) {
    char buf[4096] = {};
    GetShaderInfoLog(s, sizeof buf, nullptr, buf);
    log = buf;
    DeleteShader(s);
    return 0;
  }
  return s;
}

unsigned Extras::program(const char* name, const char* fs, std::string& error) {
  std::string log;
  GLuint v = compileStage(VERTEX_SHADER, kVs, log);
  if (!v) {
    error += std::string("compile extras vs: ") + log + "\n";
    return 0;
  }
  GLuint f = compileStage(FRAGMENT_SHADER, fs, log);
  if (!f) {
    error += std::string("compile ") + name + ": " + log + "\n";
    DeleteShader(v);
    return 0;
  }
  GLuint p = CreateProgram();
  AttachShader(p, v);
  AttachShader(p, f);
  LinkProgram(p);
  DeleteShader(v);
  DeleteShader(f);
  GLint ok = 0;
  GetProgramiv(p, LINK_STATUS, &ok);
  if (!ok) {
    char buf[4096] = {};
    GetProgramInfoLog(p, sizeof buf, nullptr, buf);
    error += std::string("link ") + name + ": " + buf + "\n";
    DeleteProgram(p);
    return 0;
  }
  return p;
}

static GLuint makeTex(int w, int h, GLenum fmt) {
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

constexpr int GEN_W = 1024, GEN_H = 768;

bool Extras::init(std::string& error) {
  std::string e;
  progTracery_ = program("tracery", kTraceryFs, e);
  progPattern_ = program("pattern", kPatternFs, e);
  progRose_ = program("rose", kRoseFs, e);
  progBurn_ = program("burn", kBurnFs, e);
  progFinish_ = program("finish", kFinishFs, e);
  if (!e.empty()) {
    error = e;
    return false;
  }
  GenVertexArrays(1, &vao_);
  GenFramebuffers(1, &fbo_);
  GenSamplers(1, &sampler_);
  SamplerParameteri(sampler_, TEXTURE_MIN_FILTER, LINEAR);
  SamplerParameteri(sampler_, TEXTURE_MAG_FILTER, LINEAR);
  SamplerParameteri(sampler_, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
  SamplerParameteri(sampler_, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
  GenSamplers(1, &repeatSampler_);
  SamplerParameteri(repeatSampler_, TEXTURE_MIN_FILTER, LINEAR);
  SamplerParameteri(repeatSampler_, TEXTURE_MAG_FILTER, LINEAR);
  SamplerParameteri(repeatSampler_, TEXTURE_WRAP_S, REPEAT);
  SamplerParameteri(repeatSampler_, TEXTURE_WRAP_T, REPEAT);
  genTex_[0] = makeTex(GEN_W, GEN_H, RGBA8);
  genTex_[1] = makeTex(GEN_W, GEN_H, RGBA8);
  roseTex_ = makeTex(ACTIVE_WIDTH, ACTIVE_HEIGHT, RGBA8);
  shroudTex_ = makeTex(ACTIVE_WIDTH, ACTIVE_HEIGHT, RGBA16F);
  displayTex_ = makeTex(ACTIVE_WIDTH, ACTIVE_HEIGHT, RGBA8);
  clearShroud();
  return true;
}

void Extras::destroy() {
  GLuint progs[] = {progTracery_, progPattern_, progRose_, progBurn_, progFinish_};
  for (GLuint p : progs)
    if (p) DeleteProgram(p);
  GLuint texs[] = {genTex_[0], genTex_[1], patternTex_, roseTex_, shroudTex_, displayTex_};
  for (GLuint t : texs)
    if (t) DeleteTextures(1, &t);
  if (vao_) DeleteVertexArrays(1, &vao_);
  if (fbo_) DeleteFramebuffers(1, &fbo_);
  if (sampler_) DeleteSamplers(1, &sampler_);
  if (repeatSampler_) DeleteSamplers(1, &repeatSampler_);
  *this = Extras();
}

void Extras::drawInto(unsigned tex, int w, int h) {
  BindFramebuffer(FRAMEBUFFER, fbo_);
  FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, tex, 0);
  Viewport(0, 0, w, h);
  Disable(DEPTH_TEST);
  Disable(SCISSOR_TEST);
  Disable(CULL_FACE);
  Disable(FRAMEBUFFER_SRGB);
  BindVertexArray(vao_);
  DrawArrays(TRIANGLES, 0, 3);
  BindVertexArray(0);
  FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, 0, 0);
  BindFramebuffer(FRAMEBUFFER, 0);
}

static void bindTex(unsigned unit, unsigned tex, unsigned sampler) {
  ActiveTexture(TEXTURE0 + unit);
  BindTexture(TEXTURE_2D, tex);
  BindSampler(unit, sampler);
}

unsigned Extras::drawGenerator(int slot, Generator g, double seconds, float drift, float tiles, int& w, int& h) {
  w = GEN_W;
  h = GEN_H;
  const unsigned dst = genTex_[slot & 1];
  const float t = static_cast<float>(std::fmod(seconds, 3600.0));
  if (g == Generator::Pattern) {
    if (!patternTex_) {
      // nothing loaded yet: a plain dark ground
      BindFramebuffer(FRAMEBUFFER, fbo_);
      FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, dst, 0);
      ClearColor(0.05f, 0.05f, 0.05f, 1);
      Clear(COLOR_BUFFER_BIT);
      FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, 0, 0);
      BindFramebuffer(FRAMEBUFFER, 0);
      return dst;
    }
    UseProgram(progPattern_);
    bindTex(0, patternTex_, repeatSampler_);
    Uniform1i(GetUniformLocation(progPattern_, "img"), 0);
    Uniform1f(GetUniformLocation(progPattern_, "t"), t);
    Uniform1f(GetUniformLocation(progPattern_, "drift"), drift);
    Uniform1f(GetUniformLocation(progPattern_, "aspect"), static_cast<float>(GEN_W) / GEN_H);
    Uniform1f(GetUniformLocation(progPattern_, "imgAspect"), static_cast<float>(patternW_) / patternH_);
    Uniform1f(GetUniformLocation(progPattern_, "tiles"), tiles);
  } else {
    UseProgram(progTracery_);
    const int mode = g == Generator::TraceryRose ? 1 : g == Generator::TraceryLancet ? 2 : 3;
    Uniform1i(GetUniformLocation(progTracery_, "mode"), mode);
    Uniform1f(GetUniformLocation(progTracery_, "t"), t);
    Uniform1f(GetUniformLocation(progTracery_, "drift"), drift);
    Uniform1f(GetUniformLocation(progTracery_, "aspect"), static_cast<float>(GEN_W) / GEN_H);
  }
  drawInto(dst, GEN_W, GEN_H);
  UseProgram(0);
  bindTex(0, 0, 0);
  return dst;
}

std::string Extras::loadPattern(const std::string& path) {
  if (patternTex_) {
    DeleteTextures(1, &patternTex_);
    patternTex_ = 0;
  }
  if (path.empty()) return "";
  int w = 0, h = 0, n = 0;
  stbi_set_flip_vertically_on_load(1);  // GL rows run bottom-up
  unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
  if (!px) return std::string("could not read ") + path + " (" + stbi_failure_reason() + ")";
  patternTex_ = makeTex(w, h, RGBA8);
  BindTexture(TEXTURE_2D, patternTex_);
  PixelStorei(UNPACK_ALIGNMENT, 1);
  TexSubImage2D(TEXTURE_2D, 0, 0, 0, w, h, RGBA, UNSIGNED_BYTE, px);
  PixelStorei(UNPACK_ALIGNMENT, 4);
  BindTexture(TEXTURE_2D, 0);
  stbi_image_free(px);
  patternW_ = w;
  patternH_ = h;
  return "";
}

void Extras::roseInto(unsigned face, unsigned dst, const PostSettings& s) {
  UseProgram(progRose_);
  bindTex(0, face, sampler_);
  Uniform1i(GetUniformLocation(progRose_, "face"), 0);
  Uniform1f(GetUniformLocation(progRose_, "amount"), s.rose);
  Uniform1f(GetUniformLocation(progRose_, "folds"), static_cast<float>(s.roseFolds));
  Uniform1f(GetUniformLocation(progRose_, "angle"), s.roseAngle);
  drawInto(dst, ACTIVE_WIDTH, ACTIVE_HEIGHT);
  UseProgram(0);
  bindTex(0, 0, 0);
}

void Extras::burn(unsigned face, float weight) {
  if (weight <= 0) return;
  UseProgram(progBurn_);
  bindTex(0, face, sampler_);
  Uniform1i(GetUniformLocation(progBurn_, "face"), 0);
  Uniform1f(GetUniformLocation(progBurn_, "weight"), std::min(1.0f, weight));
  Enable(BLEND);
  BlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA);
  drawInto(shroudTex_, ACTIVE_WIDTH, ACTIVE_HEIGHT);
  Disable(BLEND);
  UseProgram(0);
  bindTex(0, 0, 0);
}

// Each press joins a running average of every press so far, so the shroud
// holds all of them equally (the first press fills it outright). Past 48
// presses each new one still counts for a forty-eighth.
void Extras::burnIn(unsigned face) {
  burn(face, 1.0f / static_cast<float>(std::min(burns_, 47) + 1));
  burns_++;
}

void Extras::clearShroud() {
  BindFramebuffer(FRAMEBUFFER, fbo_);
  FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, shroudTex_, 0);
  ClearColor(0, 0, 0, 0);
  Clear(COLOR_BUFFER_BIT);
  FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, 0, 0);
  BindFramebuffer(FRAMEBUFFER, 0);
  burns_ = 0;
}

unsigned Extras::finish(unsigned src, const PostSettings& s) {
  UseProgram(progFinish_);
  bindTex(0, src, sampler_);
  bindTex(1, shroudTex_, sampler_);
  Uniform1i(GetUniformLocation(progFinish_, "face"), 0);
  Uniform1i(GetUniformLocation(progFinish_, "shroud"), 1);
  Uniform1f(GetUniformLocation(progFinish_, "level"), s.shroud);
  Uniform1f(GetUniformLocation(progFinish_, "vigil"), s.vigil);
  Uniform1f(GetUniformLocation(progFinish_, "flame"), s.flame);
  drawInto(displayTex_, ACTIVE_WIDTH, ACTIVE_HEIGHT);
  UseProgram(0);
  bindTex(1, 0, 0);
  bindTex(0, 0, 0);
  return displayTex_;
}

} // namespace skillet
