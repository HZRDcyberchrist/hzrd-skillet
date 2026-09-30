// Stages added around the videoskillet path for the Resolume port (not part
// of the app): picture generators that stand in for a layer, and the passes
// that work on the finished tube face.
//
//   Generators  Gothic tracery (rose window, lancet arcade, quatrefoil
//               diaper) and a tiled pattern image, drawn into a texture that
//               feeds slot A or B like a layer would.
//   Rose        Wraps the face into a disc, as if the beam swept in circles,
//               with optional N-way mirrored folds. Applied inside the camera
//               loop it feeds back into itself.
//   Shroud      A slow accumulation of burned-in frames shown as a warm ghost
//               over the picture.
//   Vigil       Candle-lit dimming and warmth (the supply side is done with
//               controls; this is the light).
//
// Every shader here is plain GLSL 4.10 fragment code, so it runs in the same
// contexts as the rest of the engine.
#pragma once
#include <string>

#include "glapi.h"

namespace skillet {

enum class Generator : int { None = 0, TraceryRose, TraceryLancet, TraceryQuatrefoil, Pattern, Count };

struct PostSettings {
  float rose = 0;          // 0 flat raster .. 1 fully wrapped disc
  int roseFolds = 0;       // 0 = no mirroring, otherwise the number of mirrored sectors
  float roseAngle = 0;     // radians, the disc's current turn
  bool roseInLoop = true;  // feed the disc back through the camera loop
  float shroud = 0;        // how strongly the burned-in ghost shows
  float autoBurn = 0;      // 0..1, continuous burn-in rate
  float vigil = 0;         // 0..1, candle-lit dim and warmth
  float flame = 0;         // -1..1, the candle's current flicker
};

class Extras {
 public:
  // needs the engine's GL functions loaded and a current context
  bool init(std::string& error);
  void destroy();

  // Draws a generator into its slot (0 = A, 1 = B) and returns that texture
  // (bottom-up, GL convention) with its size.
  unsigned drawGenerator(int slot, Generator g, double seconds, float drift, float tiles, int& w, int& h);
  // Loads an image for the Pattern generator; returns "" or why it failed.
  std::string loadPattern(const std::string& path);
  bool hasPattern() const { return patternTex_ != 0; }

  // Face passes. `face` is the tube face at raster size.
  void roseInto(unsigned face, unsigned dst, const PostSettings& s);
  unsigned roseTemp() const { return roseTex_; }
  void burn(unsigned face, float weight);
  void burnIn(unsigned face);  // one press of Burn in
  void clearShroud();
  // Composes shroud and vigil over `src`; returns the texture to present.
  unsigned finish(unsigned src, const PostSettings& s);
  bool finishNeeded(const PostSettings& s) const { return s.shroud > 0.001f || s.vigil > 0.001f; }

 private:
  unsigned program(const char* name, const char* fs, std::string& error);
  void drawInto(unsigned tex, int w, int h);

  unsigned vao_ = 0, fbo_ = 0, sampler_ = 0, repeatSampler_ = 0;
  unsigned progTracery_ = 0, progPattern_ = 0, progRose_ = 0, progBurn_ = 0, progFinish_ = 0;
  unsigned genTex_[2] = {0, 0};
  unsigned patternTex_ = 0;
  int patternW_ = 0, patternH_ = 0;
  unsigned roseTex_ = 0, shroudTex_ = 0, displayTex_ = 0;
  int burns_ = 0;
};

} // namespace skillet
