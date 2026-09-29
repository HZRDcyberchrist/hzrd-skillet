// Prints what SignalChain packs per frame, in ref_chain.mjs's format.
#include <cstdio>
#include <cstring>
#include <string>
#include "../../engine/chain.h"
using namespace skillet;
static std::string hex(const void* p, size_t n) {
  static const char* d = "0123456789abcdef"; std::string s; const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; i++) { s += d[b[i] >> 4]; s += d[b[i] & 15]; } return s;
}
int main(int argc, char** argv) {
  const char* name = argc > 1 ? argv[1] : "vhs";
  int frames = argc > 2 ? atoi(argv[2]) : 20;
  uint32_t seed = argc > 3 ? (uint32_t)atoll(argv[3]) : 1234;
  bool bEnabled = argc > 4 ? atoi(argv[4]) != 0 : true;
  SignalChain ch(seed);
  if (strcmp(name, "clean") != 0) {
    int found = -1;
    for (int i = 0; i < kNumPresets; i++) if (strcmp(kPresets[i].name, name) == 0) found = i;
    if (found < 0) { fprintf(stderr, "no preset %s\n", name); return 1; }
    const Preset& p = kPresets[found];
    for (int i = 0; i < p.count; i++) ch.controls[kPresetPatches[p.first + i].key] = kPresetPatches[p.first + i].value;
  }
  if (const char* cap = getenv("CAPTION")) ch.captionState.setText(cap);
  FrameEnv env; env.canvasW = 1920; env.canvasH = 1080; env.srcNoiseB = bEnabled ? 1 : 0; env.bEnabled = bEnabled;
  for (int r = 0; r < frames; r++) {
    env.nowMs = r * 1000.0 / 60;
    if (!ch.step(env)) continue;
    if (ch.filtersFresh) printf("filters %s\n", hex(ch.filters, sizeof ch.filters).c_str());
    printf("params %u %s\n", ch.frame, hex(ch.params, kParamBytes).c_str());
    if (ch.gates.aFeed) printf("feedA %u %s\n", ch.frame, hex(ch.feedParamsA, kParamBytes).c_str());
    if (ch.gates.bFeed) printf("feedB %u %s\n", ch.frame, hex(ch.feedParamsB, kParamBytes).c_str());
    for (int g = 0; g < ch.gates.gens; g++) printf("line%d %u %s\n", g, ch.frame, hex(ch.lineParams[g], sizeof ch.lineParams[g]).c_str());
  }
}
