// Synthetic signals test what drives the effect, not just what the UI draws.
#include "../plugin/audio.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <cstdlib>
using namespace hzrdaudio;
constexpr double pi = 3.14159265358979323846, sr = 48000;
void require(bool condition, const char* message) {
  if (!condition) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
void tone(float* w, double hz, double amplitude = 0.8) {
  // Use exact FFT-bin frequencies to avoid confusing filter rejection with
  // leakage at the edges of a short, overlapping capture window.
  for (int i=0;i<WINDOW;i++) w[i]=static_cast<float>(amplitude*std::sin(2*pi*hz*i/sr));
}
int main() {
  float w[WINDOW]{};
  Analyzer a; Settings s;
  a.update(w,sr,1); require(a.level==0 && a.hit==0,"silence must not react");
  const double bassHz = 3*sr/WINDOW, midHz = 43*sr/WINDOW, highHz = 256*sr/WINDOW;
  tone(w,bassHz); a.update(w,sr,1); require(a.level>0.5 && a.hit>0.5,"legacy full mix responds to a bass onset");
  s.focus=Focus::Kick; a.update(w,sr,1,s); require(a.level>0.5 && a.hit>0.5,"kick band accepts a low thump");
  tone(w,midHz); a.reset(); for(int i=0;i<20;i++) a.update(w,sr,1,s);
  require(a.level<0.001 && a.hit<0.001,"kick band rejects a mid-frequency tone");
  s.focus=Focus::Mids; a.update(w,sr,1,s); require(a.level>0.5,"mids accepts a 1 kHz tone");
  s.focus=Focus::Highs; a.update(w,sr,1,s); require(a.level<0.001,"highs rejects a 1 kHz tone");
  tone(w,highHz); a.update(w,sr,1,s); require(a.level>0.5,"highs accepts a 6 kHz tone");
  s.focus=Focus::Bass; tone(w,bassHz); s.threshold=0.9; a.reset();
  a.update(w,sr,1,s); require(a.detectedLevel>0.5 && a.level==0 && a.hit==0,"reactive line gates a sound below the line");
  for(int i=0;i<LINES;i++) require(a.lines()[i]==0,"threshold gates every waveform route");
  s.threshold=0.4; a.update(w,sr,1,s); require(a.level>0.5 && a.hit>0,"lowering the line opens the reaction");
  for(int i=0;i<WINDOW;i++) w[i]=0;
  for(int i=0;i<80;i++) a.update(w,sr,1,s);
  require(a.level==0 && a.hit==0,"gate releases to silence");
  tone(w,bassHz); s.threshold=0; a.update(w,sr,0,s);
  require(a.level==0 && a.hit==0,"zero gain mutes all envelopes");
  for(int i=0;i<LINES;i++) require(a.lines()[i]==0,"zero gain mutes every waveform sample");
  // A quiet bass under a loud lead stays quiet; filter auto-gain must not
  // turn the rejected lead or a tiny leak into a full-strength kick.
  for(int i=0;i<WINDOW;i++) w[i]=static_cast<float>(0.05*std::sin(2*pi*bassHz*i/sr)+0.8*std::sin(2*pi*midHz*i/sr));
  a.reset(); a.update(w,sr,1,s); require(a.level<0.1,"selected band uses the input's common level reference");
  s.focus=Focus::Custom; s.lowHz=800; s.highHz=1200; tone(w,midHz);
  a.update(w,sr,1,s); require(a.level>0.5,"custom band accepts the chosen frequency");
  tone(w,bassHz); a.reset(); a.update(w,sr,1,s); require(a.level<0.001,"custom band rejects out-of-band sound");
  s.lowHz=1200; s.highHz=800;
  auto b=focusBand(s,sr); require(b.low==800 && b.high==1200,"reversed host band endpoints are ordered");
  for(double hz:{20.0,35.0,250.0,2000.0,16000.0}) require(std::abs(frequencyFromControl(frequencyToControl(hz))-hz)<0.001,"frequency controls round-trip");
  w[3]=std::numeric_limits<float>::quiet_NaN(); w[7]=std::numeric_limits<float>::infinity();
  a.update(w,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),s);
  require(std::isfinite(a.level) && std::isfinite(a.hit),"invalid input stays finite");
  for(int i=0;i<LINES;i++) require(std::isfinite(a.lines()[i]),"invalid input never reaches the signal shaders");
  a.reset(); require(a.level==0 && a.hit==0 && a.spectrum[0]==0 && a.waveform[0]==0,"reset clears the scope and reaction");
  std::puts("Audio checks passed: band selection, threshold, silence, gain, custom band, invalid input.");
}
