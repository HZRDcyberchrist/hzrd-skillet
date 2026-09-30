#include "relic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"

namespace relic {

constexpr int SPL = 910, LINES = 525;

// Print size: the raster's 4:3 face at 160 x 120 mm on a 3 mm base, with up
// to 12 mm of relief from blacker-than-black sync tips to peak white.
constexpr float WIDTH_MM = 160, DEPTH_MM = 120, BASE_MM = 3, RELIEF_MM = 12;

std::string folder() {
#if defined(_WIN32)
  char buf[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableA("USERPROFILE", buf, MAX_PATH);
  std::string dir = n > 0 && n < MAX_PATH ? std::string(buf) + "\\Documents\\SkilletNTSC relics" : "SkilletNTSC relics";
  CreateDirectoryA(dir.c_str(), nullptr);
  return dir;
#else
  const char* home = std::getenv("HOME");
  std::string dir = std::string(home ? home : ".") + "/SkilletNTSC relics";
  mkdir(dir.c_str(), 0755);
  return dir;
#endif
}

static std::string stamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
  return buf;
}

// Signal level to 0..1: the 0.5th to 99.5th percentile spans the relief, so a
// stray spike can't flatten the rest of the frame.
static std::vector<float> normalise(const std::vector<float>& v) {
  std::vector<float> s(v);
  for (float& x : s)
    if (!std::isfinite(x)) x = 0;
  std::vector<float> sorted(s);
  const size_t lo = sorted.size() / 200, hi = sorted.size() - 1 - sorted.size() / 200;
  std::nth_element(sorted.begin(), sorted.begin() + lo, sorted.end());
  const float a = sorted[lo];
  std::nth_element(sorted.begin(), sorted.begin() + hi, sorted.end());
  const float b = sorted[hi];
  const float span = b - a > 1e-6f ? b - a : 1.0f;
  for (float& x : s) x = std::min(1.0f, std::max(0.0f, (x - a) / span));
  return s;
}

namespace {
struct Stl {
  FILE* f;
  uint32_t count = 0;
  void tri(const float* a, const float* b, const float* c) {
    float n[3] = {0, 0, 0};
    const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    n[0] = u[1] * v[2] - u[2] * v[1];
    n[1] = u[2] * v[0] - u[0] * v[2];
    n[2] = u[0] * v[1] - u[1] * v[0];
    const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l > 0)
      for (float& x : n) x /= l;
    std::fwrite(n, 4, 3, f);
    std::fwrite(a, 4, 3, f);
    std::fwrite(b, 4, 3, f);
    std::fwrite(c, 4, 3, f);
    const uint16_t attr = 0;
    std::fwrite(&attr, 2, 1, f);
    count++;
  }
};
}  // namespace

std::string cast(const std::vector<float>& composite, const std::string& base) {
  if (composite.size() < static_cast<size_t>(SPL * LINES)) return "no signal to cast";
  const std::vector<float> h = normalise(composite);

  // heightmap PNG at full resolution: one row per line, one pixel per sample
  {
    std::vector<uint8_t> px(SPL * LINES);
    for (int i = 0; i < SPL * LINES; i++) px[i] = static_cast<uint8_t>(std::lround(h[i] * 255));
    if (!stbi_write_png((base + ".png").c_str(), SPL, LINES, 1, px.data(), SPL)) return "could not write " + base + ".png";
  }

  // STL: one field (every other line) at half the sample rate keeps the file
  // near 25 MB while every scan line stays a ridge you can feel.
  const int W = SPL / 2, H = (LINES + 1) / 2;
  auto top = [&](int x, int y, float* p) {
    const float v = h[(y * 2) * SPL + x * 2];
    p[0] = WIDTH_MM * x / (W - 1);
    p[1] = DEPTH_MM * (1.0f - static_cast<float>(y) / (H - 1));  // line 0 at the back
    p[2] = BASE_MM + RELIEF_MM * v;
  };
  auto bot = [&](int x, int y, float* p) {
    p[0] = WIDTH_MM * x / (W - 1);
    p[1] = DEPTH_MM * (1.0f - static_cast<float>(y) / (H - 1));
    p[2] = 0;
  };
  FILE* f = std::fopen((base + ".stl").c_str(), "wb");
  if (!f) return "could not write " + base + ".stl";
  char header[80] = {};
  std::snprintf(header, sizeof header, "Skillet NTSC relic: one field of composite video");
  std::fwrite(header, 1, 80, f);
  uint32_t zero = 0;
  std::fwrite(&zero, 4, 1, f);  // count, patched below
  Stl s{f};
  float a[3], b[3], c[3], d[3];
  for (int y = 0; y + 1 < H; y++) {
    for (int x = 0; x + 1 < W; x++) {
      top(x, y, a), top(x + 1, y, b), top(x, y + 1, c), top(x + 1, y + 1, d);
      s.tri(a, c, b);
      s.tri(b, c, d);
      bot(x, y, a), bot(x + 1, y, b), bot(x, y + 1, c), bot(x + 1, y + 1, d);
      s.tri(a, b, c);
      s.tri(b, d, c);
    }
  }
  // walls, sharing the grid's edge vertices so the solid is closed
  for (int x = 0; x + 1 < W; x++) {
    top(x, 0, a), top(x + 1, 0, b), bot(x, 0, c), bot(x + 1, 0, d);
    s.tri(a, b, c), s.tri(b, d, c);
    top(x, H - 1, a), top(x + 1, H - 1, b), bot(x, H - 1, c), bot(x + 1, H - 1, d);
    s.tri(a, c, b), s.tri(b, c, d);
  }
  for (int y = 0; y + 1 < H; y++) {
    top(0, y, a), top(0, y + 1, b), bot(0, y, c), bot(0, y + 1, d);
    s.tri(a, c, b), s.tri(b, c, d);
    top(W - 1, y, a), top(W - 1, y + 1, b), bot(W - 1, y, c), bot(W - 1, y + 1, d);
    s.tri(a, b, c), s.tri(b, d, c);
  }
  std::fseek(f, 80, SEEK_SET);
  std::fwrite(&s.count, 4, 1, f);
  std::fclose(f);
  return "";
}

std::string castAsync(std::vector<float> composite) {
  const std::string base = folder() +
#if defined(_WIN32)
                           "\\"
#else
                           "/"
#endif
                           + "skillet-relic-" + stamp();
  std::thread([composite = std::move(composite), base]() { cast(composite, base); }).detach();
  return base;
}

}  // namespace relic
