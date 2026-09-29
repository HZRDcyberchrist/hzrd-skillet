#include "glapi.h"

#include <cstring>

namespace skillet {
namespace gl {

#define SKILLET_DEFINE(name, ret, params) PFN_##name name = nullptr;
SKILLET_GL_FUNCS(SKILLET_DEFINE)
#undef SKILLET_DEFINE

const char* load(GetProcFn getProc) {
#define SKILLET_LOAD(name, ret, params)                                            \
  {                                                                                \
    const char* glName = std::strcmp(#name, "MemBarrier") == 0 ? "glMemoryBarrier" : "gl" #name; \
    name = reinterpret_cast<PFN_##name>(getProc(glName));                          \
    if (name == nullptr) return glName;                                            \
  }
  SKILLET_GL_FUNCS(SKILLET_LOAD)
#undef SKILLET_LOAD
  return nullptr;
}

} // namespace gl
} // namespace skillet
