// A minimal FFGL host: drives the real Skillet plugin through plugMain the
// way Resolume does (initialise, instantiate, set parameters, process) and
// saves what it drew. Exercises the wrapper, presets, pads and knobs.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cmath>
#include <thread>
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
  const char* glv = getenv("SKILLET_TEST_GL_MINOR"); EGLint at[] = {0x3098, 4, 0x30FB, glv ? atoi(glv) : 5, 0x30FD, 1, 0x3038}; EGLContext c = eglCreateContext(d, n ? cfg : 0, 0, at); eglMakeCurrent(d, 0, 0, c);
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
  // a second input for the mixer: the test card mirrored left-right, standing
  // in for "the layers below"
  std::vector<uint8_t> px2(px.size());
  for (int y = 0; y < 720; y++) for (int x = 0; x < 1280; x++) std::memcpy(&px2[(y * 1280 + x) * 4], &px[(y * 1280 + (1279 - x)) * 4], 4);
  GLuint inTex2; GenTextures(1, &inTex2); BindTexture(TEXTURE_2D, inTex2); TexStorage2D(TEXTURE_2D, 1, RGBA8, 1280, 720);
  TexSubImage2D(TEXTURE_2D, 0, 0, 0, 1280, 720, RGBA, UNSIGNED_BYTE, px2.data()); BindTexture(TEXTURE_2D, 0);
  FFGLTextureStruct tex{1280, 720, 1280, 720, inTex}, tex2{1280, 720, 1280, 720, inTex2};
  FFGLTextureStruct* texs[2] = {&tex2, &tex};  // mixer order: layers below, then the layer
#if SKILLET_MIXER
  ProcessOpenGLStruct pgl{2, texs, fbo};
#else
  FFGLTextureStruct* one[1] = {&tex};
  ProcessOpenGLStruct pgl{1, one, fbo};
  (void)texs;
#endif
  // script: argv = sequence of commands: frames:N  set:<Name>=<v>  press:<Name>  events  elements:<Name>  shot:<file>
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
    } else if (cmd.rfind("text:", 0) == 0) {
      size_t eq = cmd.find('='); std::string name = cmd.substr(5, eq - 5); std::string val = cmd.substr(eq + 1);
      int p = findParam(name.c_str()); if (p < 0) { printf("no param %s\n", name.c_str()); return 1; }
      SetParameterStruct st; st.ParameterNumber = p; st.NewParameterValue.PointerValue = const_cast<char*>(val.c_str());
      call(FF_SET_PARAMETER, ptr(&st), id); printf("text %s=%s\n", name.c_str(), val.c_str());
    } else if (cmd.rfind("captionstorm:", 0) == 0) {
      // another thread edits the caption while this one renders, the way a
      // host's interface thread can
      int p = findParam("Caption"); int n = atoi(cmd.c_str() + 13);
      std::thread ui([&, p, n] {
        for (int k = 0; k < n; k++) {
          std::string v = k % 3 == 0 ? "" : std::string(k % 40 + 1, 'A' + k % 26) + (k % 5 == 0 ? "\nSECOND LINE" : "");
          SetParameterStruct st; st.ParameterNumber = p; st.NewParameterValue.PointerValue = const_cast<char*>(v.c_str());
          call(FF_SET_PARAMETER, ptr(&st), id);
          call(FF_GET_PARAMETER, u(p), id);
          std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
      });
      for (int j = 0; j < 60; j++) { BindFramebuffer(FRAMEBUFFER, fbo); Viewport(0, 0, W, H); call(FF_PROCESS_OPENGL, ptr(&pgl), id); }
      ui.join(); printf("caption storm of %d edits survived\n", n);
    } else if (cmd.rfind("uistorm:", 0) == 0) {
      // an interface thread pressing buttons, picking favorites and sending
      // junk values (NaN, infinities, huge numbers) while frames render
      int n = atoi(cmd.c_str() + 8);
      unsigned np2 = call(FF_GET_NUM_PARAMETERS, u(0), nullptr).UIntValue;
      std::thread ui([&, n, np2] {
        unsigned r = 12345;
        const float junk[] = {std::nanf(""), 1e30f, -1e30f, 9999.0f, -3.0f, 0.5f, 1.0f, 0.0f, 155.0f, 2.0f};
        const char* buttons[] = {"Add favorite", "Remove favorite", "Next", "Prev", "Random", "Fav next", "Fav prev", "Fav random", "Pad 3", "Reset signal"};
        for (int k = 0; k < n; k++) {
          r = r * 1103515245u + 12345u;
          if (k % 3 == 0) { int p = findParam(buttons[(r >> 8) % 10]); if (p >= 0) { setf(id, p, 1.0f); setf(id, p, 0.0f); } }
          else { unsigned p = (r >> 4) % np2; int t = call(FF_GET_PARAMETER_TYPE, u(p), nullptr).UIntValue; if (t != 100 && t != 14) setf(id, p, junk[(r >> 12) % 10]); }
          call(FF_GET_PARAMETER_DISPLAY, u((r >> 3) % np2), id);
          std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
      });
      for (int j = 0; j < 120; j++) { BindFramebuffer(FRAMEBUFFER, fbo); Viewport(0, 0, W, H); call(FF_PROCESS_OPENGL, ptr(&pgl), id);
        GetParamEventsStruct q{0, nullptr}; call(FF_GET_PARAMETER_EVENTS, ptr(&q), id); std::vector<ParamEventStruct> ev(q.numEvents + 64); GetParamEventsStruct g{q.numEvents, ev.data()}; call(FF_GET_PARAMETER_EVENTS, ptr(&g), id); }
      ui.join(); printf("ui storm of %d actions survived\n", n);
    } else if (cmd == "status") {
      int p = findParam("Status"); const char* t = (const char*)call(FF_GET_PARAMETER, u(p), id).PointerValue;
      printf("status: %s\n", t ? t : "");
    } else if (cmd.rfind("sleep:", 0) == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(atoi(cmd.c_str() + 6)));
    } else if (cmd == "events") {
      // what a host polls after each frame: which parameters changed on their own
      GetParamEventsStruct q{0, nullptr}; call(FF_GET_PARAMETER_EVENTS, ptr(&q), id);
      std::vector<ParamEventStruct> ev(q.numEvents); GetParamEventsStruct g{q.numEvents, ev.data()};
      call(FF_GET_PARAMETER_EVENTS, ptr(&g), id);
      for (FFUInt32 k = 0; k < g.numEvents; k++) {
        const char* pn = (const char*)call(FF_GET_PARAMETER_NAME, u(ev[k].ParameterNumber), nullptr).PointerValue;
        FFMixed v = call(FF_GET_PARAMETER, u(ev[k].ParameterNumber), id); float fv; std::memcpy(&fv, &v.UIntValue, 4);
        printf("event: %s flags=0x%llx value=%g\n", pn ? pn : "?", (unsigned long long)ev[k].eventFlags, fv);
      }
      if (g.numEvents == 0) puts("event: none");
    } else if (cmd.rfind("elements:", 0) == 0) {
      // an option parameter's current entries, as the host would list them
      int p = findParam(cmd.c_str() + 9); if (p < 0) { printf("no param %s\n", cmd.c_str() + 9); return 1; }
      unsigned ne = call(FF_GET_NUM_PARAMETER_ELEMENTS, u(p), id).UIntValue;
      FFMixed v = call(FF_GET_PARAMETER, u(p), id); float fv; std::memcpy(&fv, &v.UIntValue, 4);
      printf("%s = %g, %u entries:", cmd.c_str() + 9, fv, ne);
      for (unsigned k = 0; k < ne; k++) {
        GetParameterElementNameStruct q{(FFUInt32)p, k};
        const char* en = (const char*)call(FF_GET_PARAMETER_ELEMENT_NAME, ptr(&q), id).PointerValue;
        printf(" [%s]", en ? en : "?");
      }
      puts("");
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
