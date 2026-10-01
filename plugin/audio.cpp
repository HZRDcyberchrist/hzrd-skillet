#include "audio.h"
#include "audio_capture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace hzrdaudio {

constexpr double kPi = 3.14159265358979323846;

// ── capture: one per device, shared (audio_capture.h) ──

#if !defined(_WIN32)
std::vector<Device> enumerateDevices() { return {{"test", "Test kick (120 bpm)"}}; }

// A kick every half second (a decaying 55 Hz thump) over a quiet 220 Hz tone,
// delivered in real time, so the harness can watch hit and level move.
void runCapture(Capture& c) {
  const double sr = 48000;
  {
    std::lock_guard<std::mutex> lock(c.m);
    c.sampleRate = sr;
  }
  double t = 0;
  auto last = std::chrono::steady_clock::now();
  std::vector<float> buf;
  while (!c.stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto now = std::chrono::steady_clock::now();
    const size_t n = static_cast<size_t>(std::chrono::duration<double>(now - last).count() * sr);
    last = now;
    buf.resize(n);
    for (size_t i = 0; i < n; i++, t += 1 / sr) {
      const double beat = std::fmod(t, 0.5);
      buf[i] = static_cast<float>(0.8 * std::exp(-beat * 18) * std::sin(2 * kPi * 55 * beat) +
                                  0.08 * std::sin(2 * kPi * 220 * t));
    }
    c.push(buf.data(), n);
  }
}
#endif

namespace {
std::mutex& registryMutex() {
  static std::mutex m;
  return m;
}
std::map<std::string, std::weak_ptr<Capture>>& registry() {
  static std::map<std::string, std::weak_ptr<Capture>> r;
  return r;
}

std::shared_ptr<Capture> acquire(const std::string& key) {
  std::lock_guard<std::mutex> lock(registryMutex());
  auto& r = registry();
  if (auto existing = r[key].lock()) return existing;
  auto c = std::make_shared<Capture>();
  c->key = key;
  Capture* raw = c.get();
  c->thread = std::thread([raw] { runCapture(*raw); });
  r[key] = c;
  return c;
}

// ── device list: refreshed every few seconds while any Listener exists ──
// Each refresher thread belongs to one "generation"; when the last Listener
// goes, the generation moves on, the thread notices and ends, and is joined.
// A Listener arriving meanwhile starts a fresh thread of the new generation,
// so a join never waits on a thread that still has users.
struct Monitor {
  std::mutex m;
  std::condition_variable cv;
  std::vector<Device> list;
  unsigned version = 0;
  int users = 0;
  unsigned gen = 0;
  bool filled = false;  // the list has been read at least once
  std::thread thread;

  void loop(unsigned myGen) {
    std::unique_lock<std::mutex> lock(m);
    while (gen == myGen) {
      lock.unlock();
      std::vector<Device> now = enumerateDevices();
      lock.lock();
      if (gen != myGen) break;
      bool same = now.size() == list.size();
      for (size_t i = 0; same && i < now.size(); i++) same = now[i].key == list[i].key && now[i].name == list[i].name;
      if (!same || !filled) {
        list = std::move(now);
        version++;
        filled = true;
      }
      cv.wait_for(lock, std::chrono::seconds(3), [&] { return gen != myGen; });
    }
  }
  void addUser() {
    std::lock_guard<std::mutex> lock(m);
    if (users++ == 0) {
      const unsigned g = gen;
      thread = std::thread([this, g] { loop(g); });
    }
  }
  void removeUser() {
    std::thread done;
    {
      std::lock_guard<std::mutex> lock(m);
      if (--users > 0) return;
      gen++;
      cv.notify_all();
      done = std::move(thread);
    }
    if (done.joinable()) done.join();
  }
};
Monitor& monitor() {
  static Monitor mon;
  return mon;
}
}  // namespace

std::vector<Device> devices(unsigned& version) {
  Monitor& mon = monitor();
  std::lock_guard<std::mutex> lock(mon.m);
  version = mon.version;
  return mon.list;
}

std::vector<Device> devicesNow(unsigned& version) {
  Monitor& mon = monitor();
  {
    std::lock_guard<std::mutex> lock(mon.m);
    if (mon.filled) {
      version = mon.version;
      return mon.list;
    }
  }
  // On its own thread: the caller's thread may have COM set up differently.
  std::vector<Device> now;
  std::thread([&now] { now = enumerateDevices(); }).join();
  std::lock_guard<std::mutex> lock(mon.m);
  if (!mon.filled) {
    mon.list = std::move(now);
    mon.version++;
    mon.filled = true;
  }
  version = mon.version;
  return mon.list;
}

Listener::Listener() { monitor().addUser(); }

Listener::~Listener() {
  cap_.reset();
  monitor().removeUser();
}

void Listener::select(const std::string& key) {
  if (key == key_) return;
  key_ = key;
  cap_.reset();  // the last user of a capture stops its thread here
  if (!key.empty()) cap_ = acquire(key);
}

bool Listener::latest(float* out, double& sampleRate) {
  if (!cap_) return false;
  std::lock_guard<std::mutex> lock(cap_->m);
  const size_t n = cap_->ring.size();
  size_t at = (cap_->head + n - WINDOW) % n;
  for (int i = 0; i < WINDOW; i++, at = (at + 1) % n) out[i] = cap_->ring[at];
  sampleRate = cap_->sampleRate;
  return true;
}

std::string Listener::error() const {
  if (!cap_) return "";
  std::lock_guard<std::mutex> lock(cap_->m);
  return cap_->error;
}

// ── analysis (audiostate.ts) ──

// Quietest input the auto-gain still normalizes against, so silence stays
// silent; the same idea for the onset reference; and the hit's per-frame
// release (~0.2 s at 60 fps).
constexpr double PEAK_FLOOR = 0.05, HIT_FLOOR = 0.01, HIT_RELEASE = 0.82;

Analyzer::Analyzer() : peak_(PEAK_FLOOR), hitRef_(HIT_FLOOR) {
  smooth_.assign(WINDOW / 2, 0.0);
  re_.resize(WINDOW);
  im_.resize(WINDOW);
  blackman_.resize(WINDOW);
  for (int i = 0; i < WINDOW; i++) {
    const double a = 2 * kPi * i / WINDOW;
    blackman_[i] = 0.42 - 0.5 * std::cos(a) + 0.08 * std::cos(2 * a);  // the Web Audio analyser's window
  }
}

void Analyzer::reset() {
  std::fill(std::begin(lines_), std::end(lines_), 0.0f);
  std::fill(smooth_.begin(), smooth_.end(), 0.0);
  level = hit = 0;
  peak_ = PEAK_FLOOR;
  lowPrev_ = 0;
  hitRef_ = HIT_FLOOR;
}

void Analyzer::update(const float* w, double sampleRate, double gain) {
  if (!(sampleRate > 0)) sampleRate = 48000;
  if (!std::isfinite(gain)) gain = 1;
  // the most recent field's worth of audio, down to one sample per line
  const int field = static_cast<int>(std::lround(sampleRate / 60));
  const int span = std::max(1, std::min(WINDOW, field));
  const int start = WINDOW - span;
  double hi = 0, sum = 0;
  for (int row = 0; row < LINES; row++) {
    float v = w[start + static_cast<int>(std::floor(static_cast<double>(row) / LINES * span))];
    if (!std::isfinite(v)) v = 0;
    lines_[row] = v;
    hi = std::max(hi, static_cast<double>(std::abs(v)));
    sum += static_cast<double>(v) * v;
  }
  // fast attack, slow release, hard floor: a quiet passage never lets the
  // gain run away, and deflection is clamped to what the controls describe
  peak_ = std::max({hi, peak_ * 0.995, PEAK_FLOOR});
  const double norm = gain / peak_;
  for (float& v : lines_) v = static_cast<float>(std::max(-2.0, std::min(2.0, v * norm)));
  level = std::min(std::sqrt(sum / LINES) / peak_, 2.0);
  // the hit is the attack, not the level: positive low-band flux, normalized
  // against the biggest recent onset
  const double low = lowEnergy(w, sampleRate);
  const double flux = std::max(0.0, low - lowPrev_);
  hitRef_ = std::max({flux, hitRef_ * 0.995, HIT_FLOOR});
  hit = std::min(std::max(hit * HIT_RELEASE, flux / hitRef_), 1.5);
  lowPrev_ = low;
}

// Mean magnitude below ~200 Hz (kick and bass), from a Blackman-windowed FFT
// smoothed over time the way the analyser does (time constant 0.8), in dB,
// weighted 0..1 over the bottom 60 dB.
double Analyzer::lowEnergy(const float* w, double sampleRate) {
  const int N = WINDOW;
  for (int i = 0; i < N; i++) {
    re_[i] = (std::isfinite(w[i]) ? w[i] : 0.0f) * blackman_[i];
    im_[i] = 0;
  }
  // in-place radix-2 FFT
  for (int i = 1, j = 0; i < N; i++) {
    int bit = N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      std::swap(re_[i], re_[j]);
      std::swap(im_[i], im_[j]);
    }
  }
  for (int len = 2; len <= N; len <<= 1) {
    const double ang = -2 * kPi / len;
    const double wr = std::cos(ang), wi = std::sin(ang);
    for (int i = 0; i < N; i += len) {
      double cr = 1, ci = 0;
      for (int k = 0; k < len / 2; k++) {
        const int a = i + k, b = i + k + len / 2;
        const double tr = re_[b] * cr - im_[b] * ci, ti = re_[b] * ci + im_[b] * cr;
        re_[b] = re_[a] - tr;
        im_[b] = im_[a] - ti;
        re_[a] += tr;
        im_[a] += ti;
        const double ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
  const double hz = sampleRate / 2 / (N / 2);
  const int bins = std::max(1, std::min(N / 2, static_cast<int>(std::lround(200 / hz))));
  double acc = 0;
  for (int i = 0; i < bins; i++) {
    const double mag = std::sqrt(re_[i] * re_[i] + im_[i] * im_[i]) / N;
    smooth_[i] = 0.8 * smooth_[i] + 0.2 * mag;
    const double db = smooth_[i] > 0 ? 20 * std::log10(smooth_[i]) : -1000;
    acc += std::max(0.0, (db + 60) / 60);
  }
  return acc / bins;
}

}  // namespace hzrdaudio
