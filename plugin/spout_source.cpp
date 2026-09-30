#include "spout_source.h"

#if defined(_WIN32) && defined(SKILLET_SPOUT)
#include "SpoutReceiver.h"

struct SpoutSource::Impl {
  SpoutReceiver rx;
  std::string name;
  bool named = false;
  GLuint tex = 0;
  int w = 0, h = 0;
  bool haveFrame = false;

  void size(int nw, int nh) {
    if (tex == 0) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, nw, nh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    w = nw;
    h = nh;
  }
};

SpoutSource::SpoutSource() : impl_(new Impl) {}
SpoutSource::~SpoutSource() { release(); }

bool SpoutSource::available() { return true; }

std::vector<std::string> SpoutSource::senders() {
  static SpoutReceiver probe;
  std::vector<std::string> out;
  const int n = probe.GetSenderCount();
  char name[256];
  for (int i = 0; i < n; i++)
    if (probe.GetSender(i, name, 256)) out.push_back(name);
  return out;
}

bool SpoutSource::receive(const std::string& name, unsigned hostFbo, unsigned& tex, int& w, int& h) {
  Impl& s = *impl_;
  if (!s.named || name != s.name) {
    s.rx.ReleaseReceiver();
    s.rx.SetReceiverName(name.empty() ? nullptr : name.c_str());
    s.name = name;
    s.named = true;
    s.haveFrame = false;
  }
  if (s.tex == 0) s.size(16, 16);
  if (s.rx.ReceiveTexture(s.tex, GL_TEXTURE_2D, false, hostFbo)) {
    if (s.rx.IsUpdated()) {
      // the sender changed size (or we just connected): resize and wait a frame
      s.size(static_cast<int>(s.rx.GetSenderWidth()), static_cast<int>(s.rx.GetSenderHeight()));
      s.haveFrame = false;
    } else {
      s.haveFrame = true;
    }
  }
  tex = s.tex;
  w = s.w;
  h = s.h;
  return s.haveFrame;
}

void SpoutSource::release() {
  if (!impl_) return;
  impl_->rx.ReleaseReceiver();
  if (impl_->tex) glDeleteTextures(1, &impl_->tex);
  impl_->tex = 0;
  impl_->named = false;
  impl_->haveFrame = false;
}

#else

struct SpoutSource::Impl {};
SpoutSource::SpoutSource() {}
SpoutSource::~SpoutSource() {}
bool SpoutSource::available() { return false; }
std::vector<std::string> SpoutSource::senders() { return {}; }
bool SpoutSource::receive(const std::string&, unsigned, unsigned& tex, int& w, int& h) {
  tex = 0;
  w = h = 0;
  return false;
}
void SpoutSource::release() {}

#endif
