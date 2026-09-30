// Signal to STL: one frame of the composite waveform cast as a printable
// relief, plus a heightmap PNG for displacement in a 3D package.
#pragma once
#include <string>
#include <vector>

namespace relic {

// Where relics go: Documents\SkilletNTSC relics (created if missing).
std::string folder();

// Starts writing <folder>/skillet-relic-<date>-<time>.stl and .png from a
// 525 x 910 composite frame on a background thread (so the render thread
// never waits on the disk). Returns the path without extension.
std::string castAsync(std::vector<float> composite);

// The same, synchronously; returns "" or why it failed. (Tests use this.)
std::string cast(const std::vector<float>& composite, const std::string& base);

}  // namespace relic
