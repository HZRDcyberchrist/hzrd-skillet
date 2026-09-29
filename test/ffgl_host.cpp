// A minimal FFGL host: drives the real Skillet plugin through plugMain the
// way Resolume does (initialise, instantiate, set parameters, process) and
// saves what it drew. Exercises the wrapper, presets, pads and knobs.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <FFGL.h>
#include "../engine/glapi.h"
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
void glewShimInit();
static void* gp(const char* n) { return eglGetProcAddress(n); }
static FFMixed call(FFUInt32 code, FFMixed in, FFInstanceID id) { return plugMain(code, in, id); }
static FFMixed ptr(void* p) { FFMixed m; m.PointerValue = p; return m; }
static FFMixed u(FFUInt32 v) { FFMixed m; m.PointerValue = nullptr; m.UIntValue = v; return m; }
static void setf(FFInstanceID id, unsigned idx, float v) {
  SetParameterStruct s; s.ParameterNumber = idx; std::memcpy(&s.NewParameterValue.UIntValue, &v, 4); call(FF_SET_PARAMETER, ptr(&s), id);
}
static int findParam(const char* name) {
  unsigned n = call(FF_GET_NUM_PARAMETERS, u(0), nullptr).UIntValue;
  for (unsigned i = 0; i < n; i++) { const char* pn = (const char*)call(FF_GET_PARAMETER_NAME, u(i), nullptr).PointerValue; if (pn && !strncmp(pn, name, 16) ) return (int)i; }
  return -1;
}
int main(int argc, char** argv) {
  EGLDisplay d = eglGetPlatformDisplay(0x31DD, 0, 0); EGLint a, b; eglInitialize(d, &a, &b); eglBindAPI(0x30A2);
  EGLint ca[] = {0x3040, 0x0008, 0x3038}; EGLConfig cfg = 0; EGLint n = 0; eglChooseConfig(d, ca, &cfg, 1, &n);
  EGLint at[] = {0x3098, 4, 0x30FB, 5, 0x30FD, 1, 0x3038}; EGLContext c = eglCreateContext(d, n ? cfg : 0, 0, at); eglMakeCurrent(d, 0, 0, c);
  using namespace skillet::gl; load(gp); glewShimInit();
  PluginInfoStruct* info = (PluginInfoStruct*)call(FF_GET_INFO, u(0), nullptr).PointerValue;
  printf("plugin: %.4s '%.16s' type=%u\n", (const char*)info->PluginUniqueID, (const char*)info->PluginName, info->PluginType);
  if (call(FF_INITIALISE_V2, u(0), nullptr).UIntValue != FF_SUCCESS) { puts("init failed"); return 1; }
  unsigned np = call(FF_GET_NUM_PARAMETERS, u(0), nullptr).UIntValue; printf("parameters: %u\n", np);
  const int W = 1280, H = 720;
  FFGLViewportStruct vp{0, 0, W, H};
  FFInstanceID id = call(FF_INSTANTIATE_GL, ptr(&vp), nullptr).PointerValue;
  if (!id || (uintptr_t)id == FF_FAIL) { puts("instantiate failed"); return 1; }
  // input texture: the test card
  FILE* f = fopen("testcard.rgba", "rb"); std::vector<uint8_t> px(1280 * 720 * 4); if (!f || fread(px.data(), 1, px.size(), f) != px.size()) return 1; fclose(f);
  GLuint inTex; GenTextures(1, &inTex); BindTexture(TEXTURE_2D, inTex); TexStorage2D(TEXTURE_2D, 1, RGBA8, 1280, 720);
  TexSubImage2D(TEXTURE_2D, 0, 0, 0, 1280, 720, RGBA, UNSIGNED_BYTE, px.data()); BindTexture(TEXTURE_2D, 0);
  GLuint outTex; GenTextures(1, &outTex); BindTexture(TEXTURE_2D, outTex); TexStorage2D(TEXTURE_2D, 1, RGBA8, W, H); BindTexture(TEXTURE_2D, 0);
  GLuint fbo; GenFramebuffers(1, &fbo); BindFramebuffer(FRAMEBUFFER, fbo); FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, outTex, 0);
  FFGLTextureStruct tex{1280, 720, 1280, 720, inTex}; FFGLTextureStruct* texs[1] = {&tex};
  ProcessOpenGLStruct pgl{1, texs, fbo};
  // script: argv = sequence of commands: frames:N  set:<Name>=<v>  press:<Name>  shot:<file>
  for (int i = 1; i < argc; i++) {
    std::string cmd = argv[i];
    if (cmd.rfind("frames:", 0) == 0) {
      int k = atoi(cmd.c_str() + 7);
      for (int j = 0; j < k; j++) { BindFramebuffer(FRAMEBUFFER, fbo); Viewport(0, 0, W, H); if (call(FF_PROCESS_OPENGL, ptr(&pgl), id).UIntValue != FF_SUCCESS) { puts("process failed"); return 1; } }
    } else if (cmd.rfind("set:", 0) == 0) {
      size_t eq = cmd.find('='); std::string name = cmd.substr(4, eq - 4); float v = atof(cmd.c_str() + eq + 1);
      int p = findParam(name.c_str()); if (p < 0) { printf("no param %s\n", name.c_str()); return 1; } setf(id, p, v);
      const char* disp = (const char*)call(FF_GET_PARAMETER_DISPLAY, u(p), id).PointerValue;
      printf("set %s=%g (display '%s')\n", name.c_str(), v, disp ? disp : "");
    } else if (cmd.rfind("press:", 0) == 0) {
      int p = findParam(cmd.c_str() + 6); if (p < 0) { printf("no param %s\n", cmd.c_str() + 6); return 1; }
      setf(id, p, 1.0f); setf(id, p, 0.0f); printf("pressed %s\n", cmd.c_str() + 6);
    } else if (cmd.rfind("shot:", 0) == 0) {
      std::vector<uint8_t> o(W * H * 4); BindFramebuffer(FRAMEBUFFER, fbo); PixelStorei(PACK_ALIGNMENT, 1); ReadPixels(0, 0, W, H, RGBA, UNSIGNED_BYTE, o.data());
      FILE* w = fopen(cmd.c_str() + 5, "wb"); fprintf(w, "P6\n%d %d\n255\n", W, H);
      for (int y = H - 1; y >= 0; y--) for (int x = 0; x < W; x++) fwrite(&o[(y * W + x) * 4], 1, 3, w);
      fclose(w);
      unsigned pp = findParam("Preset"); FFMixed g = call(FF_GET_PARAMETER, u(pp), id); float pv; std::memcpy(&pv, &g.UIntValue, 4);
      printf("shot %s (preset index %g, glError 0x%x)\n", cmd.c_str() + 5, pv, GetError());
    }
  }
  call(FF_DEINSTANTIATE_GL, u(0), id); call(FF_DEINITIALISE, u(0), nullptr);
  puts("done");
}
