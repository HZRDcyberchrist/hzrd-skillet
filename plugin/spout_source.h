// Receives a Spout sender as a picture source (Windows). Resolume Arena can
// send any layer over Spout (Advanced Output: a Spout screen whose slice
// takes that layer as input), which is how Skillet reaches another layer.
// On other platforms this is an empty stand-in.
#pragma once
#include <memory>
#include <string>
#include <vector>

class SpoutSource {
 public:
  SpoutSource();
  ~SpoutSource();

  static bool available();
  // the senders running on this machine right now
  static std::vector<std::string> senders();

  // Receives the named sender ("" = whichever is active) into a texture this
  // object owns. Needs the GL context current; `hostFbo` is restored after.
  // Returns false until a frame has arrived.
  bool receive(const std::string& name, unsigned hostFbo, unsigned& tex, int& w, int& h);
  void release();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
