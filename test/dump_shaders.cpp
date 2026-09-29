// Writes every embedded GLSL program to <dir>/<name>.{comp,vert,frag} so a
// reference compiler (glslangValidator) can check what the drivers will see.
#include <cstdio>
#include <cstring>
#include <string>
#include "../engine/generated/shaders.gen.h"
int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : ".";
  for (int i = 0; i < skillet::kNumPrograms; i++) {
    const skillet::ShaderProgram& p = skillet::kPrograms[i];
    const char* ext = !std::strcmp(p.stage, "vertex") ? "vert" : !std::strcmp(p.stage, "fragment") ? "frag" : "comp";
    const std::string path = dir + "/" + p.name + "." + ext;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return 1;
    std::fputs(p.src, f);
    std::fclose(f);
    std::printf("%s\n", path.c_str());
  }
}
