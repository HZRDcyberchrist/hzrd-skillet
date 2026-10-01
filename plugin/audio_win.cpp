// WASAPI capture for audio.cpp. Its own file: the audio headers need the
// full Windows headers, and the rest of the plugin builds lean.
#if defined(_WIN32)
#undef WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#pragma comment(lib, "ole32.lib")

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"
#include "audio_capture.h"

namespace hzrdaudio {

namespace {

template <class T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

std::string utf8(const wchar_t* w) {
  if (!w) return "";
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return "";
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  return s;
}

std::wstring wide(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 1) return L"";
  std::wstring w(static_cast<size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}

// COM for the calling thread, for as long as this object lives.
struct ComScope {
  bool ok;
  ComScope() { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
  ~ComScope() {
    if (ok) CoUninitialize();
  }
};

void listEndpoints(IMMDeviceEnumerator* en, EDataFlow flow, const char* prefix, const char* suffix,
                   std::vector<Device>& out) {
  IMMDeviceCollection* coll = nullptr;
  if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll))) return;
  UINT count = 0;
  coll->GetCount(&count);
  for (UINT i = 0; i < count; i++) {
    IMMDevice* dev = nullptr;
    if (FAILED(coll->Item(i, &dev))) continue;
    LPWSTR id = nullptr;
    std::string name;
    if (SUCCEEDED(dev->GetId(&id))) {
      IPropertyStore* props = nullptr;
      if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) name = utf8(pv.pwszVal);
        PropVariantClear(&pv);
        release(props);
      }
      if (name.empty()) name = "Audio device";
      out.push_back({std::string(prefix) + utf8(id), name + suffix});
      CoTaskMemFree(id);
    }
    release(dev);
  }
  release(coll);
}

}  // namespace

std::vector<Device> enumerateDevices() {
  ComScope com;
  std::vector<Device> out;
  out.push_back({"default-out", "Computer audio (what's playing)"});
  IMMDeviceEnumerator* en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&en))))
    return out;
  listEndpoints(en, eCapture, "in:", "", out);
  listEndpoints(en, eRender, "out:", " (output)", out);
  release(en);
  return out;
}

namespace {

// One session on the device: open it, pull packets until told to stop or the
// device goes away. Returns why it ended ("" when asked to stop).
std::string captureSession(Capture& c, IMMDeviceEnumerator* en) {
  IMMDevice* dev = nullptr;
  bool loopback = true;
  HRESULT hr;
  if (c.key == "default-out") {
    hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
  } else if (c.key.rfind("out:", 0) == 0) {
    hr = en->GetDevice(wide(c.key.substr(4)).c_str(), &dev);
  } else if (c.key.rfind("in:", 0) == 0) {
    loopback = false;
    hr = en->GetDevice(wide(c.key.substr(3)).c_str(), &dev);
  } else {
    return "unknown audio input";
  }
  if (FAILED(hr) || !dev) return "audio device not found (unplugged?)";

  IAudioClient* client = nullptr;
  IAudioCaptureClient* cap = nullptr;
  WAVEFORMATEX* wf = nullptr;
  std::string why;
  do {
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)))) {
      why = "could not open the audio device";
      break;
    }
    if (FAILED(client->GetMixFormat(&wf)) || !wf) {
      why = "could not read the audio format";
      break;
    }
    // 200 ms of buffer, shared mode: the format is the device's own mix format
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, 2000000, 0, wf,
                                  nullptr))) {
      why = "the audio device refused capture (in use exclusively?)";
      break;
    }
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap)))) {
      why = "could not start audio capture";
      break;
    }
    bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22)
      isFloat = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf)->SubFormat.Data1 == 3;  // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    const int bits = wf->wBitsPerSample, channels = std::max<int>(1, wf->nChannels);
    const int stride = std::max<int>(1, wf->nBlockAlign);
    const double sr = wf->nSamplesPerSec > 0 ? wf->nSamplesPerSec : 48000;
    {
      std::lock_guard<std::mutex> lock(c.m);
      c.sampleRate = sr;
      c.error.clear();
    }
    if (FAILED(client->Start())) {
      why = "could not start audio capture";
      break;
    }
    std::vector<float> mono;
    auto lastData = std::chrono::steady_clock::now();
    while (!c.stop) {
      Sleep(5);
      UINT32 packet = 0;
      hr = cap->GetNextPacketSize(&packet);
      if (FAILED(hr)) {
        why = hr == AUDCLNT_E_DEVICE_INVALIDATED ? "audio device went away" : "audio capture stopped";
        break;
      }
      bool got = false;
      while (packet > 0 && !c.stop) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        mono.assign(frames, 0.0f);
        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
          for (UINT32 f = 0; f < frames; f++) {
            const BYTE* frame = data + static_cast<size_t>(f) * stride;
            double acc = 0;
            for (int ch = 0; ch < channels; ch++) {
              if (isFloat && bits == 32) {
                float v;
                std::memcpy(&v, frame + ch * 4, 4);
                acc += v;
              } else if (bits == 16) {
                int16_t v;
                std::memcpy(&v, frame + ch * 2, 2);
                acc += v / 32768.0;
              } else if (bits == 24) {
                const BYTE* b = frame + ch * 3;
                const int32_t v = (static_cast<int32_t>(b[2]) << 24 | b[1] << 16 | b[0] << 8) >> 8;
                acc += v / 8388608.0;
              } else if (bits == 32) {
                int32_t v;
                std::memcpy(&v, frame + ch * 4, 4);
                acc += v / 2147483648.0;
              }
            }
            mono[f] = static_cast<float>(acc / channels);
          }
        }
        c.push(mono.data(), mono.size());
        cap->ReleaseBuffer(frames);
        got = true;
        if (FAILED(cap->GetNextPacketSize(&packet))) break;
      }
      // Loopback delivers nothing at all while nothing plays: write the
      // silence ourselves so the picture doesn't hold the last sound.
      const auto now = std::chrono::steady_clock::now();
      if (got) {
        lastData = now;
      } else if (now - lastData > std::chrono::milliseconds(40)) {
        const size_t n = static_cast<size_t>(std::chrono::duration<double>(now - lastData).count() * sr);
        mono.assign(std::min<size_t>(n, 8192), 0.0f);
        c.push(mono.data(), mono.size());
        lastData = now;
      }
    }
    client->Stop();
  } while (false);
  release(cap);
  release(client);
  if (wf) CoTaskMemFree(wf);
  release(dev);
  return why;
}

}  // namespace

void runCapture(Capture& c) {
  ComScope com;
  IMMDeviceEnumerator* en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&en)))) {
    c.setError("Windows audio is not available");
    return;
  }
  // Keep trying while selected: a device that comes back (replugged, default
  // output switched) is picked up again within a second.
  while (!c.stop) {
    const std::string why = captureSession(c, en);
    if (c.stop) break;
    c.setError(why);
    std::vector<float> quiet(4096, 0.0f);
    c.push(quiet.data(), quiet.size());
    for (int i = 0; i < 20 && !c.stop; i++) Sleep(50);
  }
  release(en);
}

}  // namespace hzrdaudio
#endif
