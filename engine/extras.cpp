// Tracery generators for the Resolume port. See extras.h.
#include "extras.h"

#include <algorithm>
#include <cmath>
#include <vector>

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
  if (!e.empty()) {
    error = e;
    return false;
  }
  GenVertexArrays(1, &vao_);
  GenFramebuffers(1, &fbo_);
  genTex_[0] = makeTex(GEN_W, GEN_H, RGBA8);
  genTex_[1] = makeTex(GEN_W, GEN_H, RGBA8);
  return true;
}

void Extras::destroy() {
  if (progTracery_) DeleteProgram(progTracery_);
  for (GLuint t : genTex_)
    if (t) DeleteTextures(1, &t);
  if (vao_) DeleteVertexArrays(1, &vao_);
  if (fbo_) DeleteFramebuffers(1, &fbo_);
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

unsigned Extras::drawGenerator(int slot, Generator g, double seconds, float drift, int& w, int& h) {
  w = GEN_W;
  h = GEN_H;
  const unsigned dst = genTex_[slot & 1];
  UseProgram(progTracery_);
  const int mode = g == Generator::TraceryRose ? 1 : g == Generator::TraceryLancet ? 2 : 3;
  Uniform1i(GetUniformLocation(progTracery_, "mode"), mode);
  Uniform1f(GetUniformLocation(progTracery_, "t"), static_cast<float>(std::fmod(seconds, 3600.0)));
  Uniform1f(GetUniformLocation(progTracery_, "drift"), drift);
  Uniform1f(GetUniformLocation(progTracery_, "aspect"), static_cast<float>(GEN_W) / GEN_H);
  drawInto(dst, GEN_W, GEN_H);
  UseProgram(0);
  return dst;
}

} // namespace skillet
