// The shared capture behind audio.h's Listener: a ring of the most recent
// mono samples, filled by one thread per device (runCapture). Private to
// audio.cpp and audio_win.cpp.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"

namespace hzrdaudio {

struct Capture {
  std::string key;
  std::mutex m;
  std::vector<float> ring = std::vector<float>(WINDOW * 4, 0.0f);
  size_t head = 0;  // next write position
  double sampleRate = 48000;
  std::string error;
  std::atomic<bool> stop{false};
  std::thread thread;

  void push(const float* s, size_t n) {
    std::lock_guard<std::mutex> lock(m);
    for (size_t i = 0; i < n; i++) {
      ring[head] = s[i];
      head = (head + 1) % ring.size();
    }
  }
  void setError(const std::string& e) {
    std::lock_guard<std::mutex> lock(m);
    error = e;
  }
  ~Capture() {
    stop = true;
    if (thread.joinable()) thread.join();
  }
};

// Platform half: audio_win.cpp on Windows, the test generator in audio.cpp
// elsewhere.
std::vector<Device> enumerateDevices();
void runCapture(Capture& c);

}  // namespace hzrdaudio
