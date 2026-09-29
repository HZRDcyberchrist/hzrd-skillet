// Headless harness: runs the engine the way the plugin does and writes what
// it would have drawn. Usage:
//   render <preset|clean> <frames> <out.ppm> [outW outH] [srcA 0..3] [srcB 0..5] [fill]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../engine/engine.h"
using namespace skillet;
extern "C" {
typedef void* EGLDisplay; typedef void* EGLContext; typedef void* EGLConfig; typedef unsigned int EGLBoolean; typedef int32_t EGLint; typedef unsigned int EGLenum;
EGLDisplay eglGetPlatformDisplay(EGLenum, void*, const intptr_t*);
EGLBoolean eglInitialize(EGLDisplay, EGLint*, EGLint*);
EGLBoolean eglBindAPI(EGLenum);
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
EGLBoolean eglMakeCurrent(EGLDisplay, void*, void*, EGLContext);
void* eglGetProcAddress(const char*);
}
static void* getProc(const char* n) { return eglGetProcAddress(n); }
int main(int argc, char** argv) {
  const char* preset = argc > 1 ? argv[1] : "clean";
  int frames = argc > 2 ? atoi(argv[2]) : 60;
  const char* outPath = argc > 3 ? argv[3] : "out.ppm";
  int OW = argc > 5 ? atoi(argv[4]) : 960, OH = argc > 5 ? atoi(argv[5]) : 720;
  int srcA = argc > 6 ? atoi(argv[6]) : 0, srcB = argc > 7 ? atoi(argv[7]) : 0;
  bool fill = argc > 8 && atoi(argv[8]) != 0;
  EGLDisplay d = eglGetPlatformDisplay(0x31DD, 0, 0); EGLint a, b; eglInitialize(d, &a, &b); eglBindAPI(0x30A2);
  EGLint ca[] = {0x3040, 0x0008, 0x3038}; EGLConfig cfg = 0; EGLint n = 0; eglChooseConfig(d, ca, &cfg, 1, &n);
  EGLint at[] = {0x3098, 4, 0x30FB, 5, 0x30FD, 1, 0x3038};
  EGLContext c = eglCreateContext(d, n ? cfg : 0, 0, at); eglMakeCurrent(d, 0, 0, c);
  Engine eng(getProc, 1234);
  if (!eng.ok()) { fprintf(stderr, "engine: %s\n", eng.error().c_str()); return 1; }
  // input: the test card, bottom-up like a host texture
  FILE* f = fopen("testcard.rgba", "rb"); std::vector<uint8_t> px(1280 * 720 * 4);
  if (!f || fread(px.data(), 1, px.size(), f) != px.size()) { fprintf(stderr, "no testcard\n"); return 1; } fclose(f);
  using namespace skillet::gl;
  GLuint inTex; GenTextures(1, &inTex); BindTexture(TEXTURE_2D, inTex); TexStorage2D(TEXTURE_2D, 1, RGBA8, 1280, 720);
  TexSubImage2D(TEXTURE_2D, 0, 0, 0, 1280, 720, RGBA, UNSIGNED_BYTE, px.data());
  GLuint outTex; GenTextures(1, &outTex); BindTexture(TEXTURE_2D, outTex); TexStorage2D(TEXTURE_2D, 1, RGBA8, OW, OH);
  BindTexture(TEXTURE_2D, 0);
  GLuint fbo; GenFramebuffers(1, &fbo); BindFramebuffer(FRAMEBUFFER, fbo);
  FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, outTex, 0);
  if (strcmp(preset, "clean") != 0) {
    int found = -1; for (int i = 0; i < kNumPresets; i++) if (!strcmp(kPresets[i].name, preset)) found = i;
    if (found < 0) { fprintf(stderr, "no preset %s\n", preset); return 1; }
    const Preset& p = kPresets[found];
    for (int i = 0; i < p.count; i++) eng.chain().controls[kPresetPatches[p.first + i].key] = kPresetPatches[p.first + i].value;
    if (p.needsB && srcB == 0) srcB = 4;
  }
  eng.setSourceA(srcA); eng.setSourceB(static_cast<SourceB>(srcB));
  InputFrame in{inTex, 1280, 720, 1280, 720, true};
  OutputTarget out{fbo, 0, 0, OW, OH, fill};
  for (int i = 0; i < frames; i++) {
    BindFramebuffer(FRAMEBUFFER, fbo);
    eng.render(in, out, i * 1000.0 / 60);
  }
  GLenum err = GetError();
  std::vector<uint8_t> o(OW * OH * 4);
  BindFramebuffer(FRAMEBUFFER, fbo); PixelStorei(PACK_ALIGNMENT, 1);
  ReadPixels(0, 0, OW, OH, RGBA, UNSIGNED_BYTE, o.data());
  FILE* w = fopen(outPath, "wb"); fprintf(w, "P6\n%d %d\n255\n", OW, OH);
  for (int y = OH - 1; y >= 0; y--) for (int x = 0; x < OW; x++) fwrite(&o[(y * OW + x) * 4], 1, 3, w);
  fclose(w);
  fprintf(stderr, "%s: %d frames, glError=0x%x\n", preset, frames, err);
  return 0;
}
