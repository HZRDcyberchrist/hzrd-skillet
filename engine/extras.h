// Picture generators added for the Resolume port (not part of the app):
// Gothic tracery (rose window, lancet arcade, quatrefoil diaper) drawn into a
// texture that feeds slot A or B like a layer would. Plain GLSL 4.10 fragment
// code, so it runs in the same contexts as the rest of the engine.
#pragma once
#include <string>

#include "glapi.h"

namespace skillet {

enum class Generator : int { None = 0, TraceryRose, TraceryLancet, TraceryQuatrefoil, Count };

class Extras {
 public:
  // needs the engine's GL functions loaded and a current context
  bool init(std::string& error);
  void destroy();

  // Draws a generator into its slot (0 = A, 1 = B) and returns that texture
  // (bottom-up, GL convention) with its size.
  unsigned drawGenerator(int slot, Generator g, double seconds, float drift, int& w, int& h);

 private:
  unsigned program(const char* name, const char* fs, std::string& error);
  void drawInto(unsigned tex, int w, int h);

  unsigned vao_ = 0, fbo_ = 0;
  unsigned progTracery_ = 0;
  unsigned genTex_[2] = {0, 0};
};

} // namespace skillet
