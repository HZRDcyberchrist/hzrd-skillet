// Exercise our own hidden window, without controlling any user application.
#define NOMINMAX
#include <windows.h>
#include "../plugin/audio_scope.h"
#include <cstdio>
#include <cmath>
#include <mutex>
#include <cstdlib>
#include <vector>
#include <cstring>
using namespace hzrdaudio;
void require(bool ok,const char* message) { if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);} }
int main(int argc,char** argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
  // Only our own offscreen test host is restored; no real app is controlled.
  HWND host=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Skillet test host",
                            WS_OVERLAPPEDWINDOW,-10000,-10000,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  ShowWindow(host,SW_SHOWMINNOACTIVE);
  std::mutex mutex; ScopeEdit changed=ScopeEdit::Focus; double value=-1; int edits=0;
  auto scope=std::make_unique<Scope>([&](ScopeEdit kind,double v){std::lock_guard<std::mutex> lock(mutex);changed=kind;value=v;edits++;});
  ScopeFrame frame; frame.settings.focus=Focus::Kick; frame.settings.threshold=.35; frame.connected=true;
  frame.source="Preview: kick + bass with a brighter synth";
  for(int i=0;i<SCOPE_SAMPLES;i++) {
    frame.focusedWaveform[i]=static_cast<float>(.7*std::sin(i*.06));
    frame.waveform[i]=frame.focusedWaveform[i]+static_cast<float>(.2*std::sin(i*.65));
  }
  for(int i=0;i<SPECTRUM_BINS;i++) frame.spectrum[i]=static_cast<float>(.85*std::exp(-std::pow((i-15)/6.0,2))+.45*std::exp(-std::pow((i-40)/3.0,2)));
  for(int i=0;i<256;i++) {frame.level=.1+.7*std::exp(-(i%40)/8.0);scope->publish(frame);}
  scope->show(); HWND window=nullptr;
  for(int i=0;i<200 && !window;i++){Sleep(10);window=FindWindowW(L"HZRDSkilletAudioScopeV12",nullptr);}
  require(window!=nullptr,"scope window is created");
  Sleep(100);
  SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(100,365));
  SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(100,365));
  {std::lock_guard<std::mutex> lock(mutex);require(changed==ScopeEdit::Threshold && std::abs(value-.5)<.01,"dragging edits the threshold");}
  HWND confirm=GetDlgItem(window,1);
  require(confirm!=nullptr && GetWindow(window,GW_CHILD)==confirm &&
          GetWindow(confirm,GW_HWNDNEXT)==nullptr,"Confirm is the only child control");
  require((GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0,"scope is always on top");
  require(SendMessageW(window,WM_GETICON,ICON_BIG,0)!=0 &&
          SendMessageW(window,WM_GETICON,ICON_SMALL,0)!=0,"SHFTR icons are loaded");
  auto pumpHost=[&] {
    for(int i=0;i<100 && IsIconic(host);i++) {
      MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&msg);DispatchMessageW(&msg);}
      Sleep(5);
    }
  };
  pumpHost();require(!IsIconic(host),"clicking scope restores its minimized host");
  ShowWindow(host,SW_SHOWMINNOACTIVE);
  SendMessageW(window,WM_SYSCOMMAND,SC_RESTORE,0);
  pumpHost();require(!IsIconic(host),"restoring scope also restores its minimized host");
  for(int i=0;i<60;i++) SendMessageW(window,WM_TIMER,1,0);
  {std::lock_guard<std::mutex> lock(mutex);require(edits==1,"refresh never writes a parameter or resets a selection");}
  frame.settings.focus=Focus::Custom; frame.settings.lowHz=800; frame.settings.highHz=1200;
  scope->publish(frame);
  SendMessageW(window,WM_TIMER,1,0);
  // Paint the actual window client into a bitmap for visual QA.
  {
    frame.settings.threshold=.5; frame.settings.focus=Focus::Kick; scope->publish(frame);
    RECT r;GetClientRect(window,&r);const int w=r.right,h=r.bottom;
    HDC screen=GetDC(window), dc=CreateCompatibleDC(screen);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;
    info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;HBITMAP bmp=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    HGDIOBJ old=SelectObject(dc,bmp);
    SendMessageW(window,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);
    const size_t headerBytes=static_cast<size_t>(w)*96*4;
    std::vector<unsigned char> fixedHeader(static_cast<unsigned char*>(pixels),static_cast<unsigned char*>(pixels)+headerBytes);
    const DWORD before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(int i=0;i<120;i++) {
      frame.level=.1+(i%10)*.05; frame.focusedWaveform[i%SCOPE_SAMPLES]*=-1; scope->publish(frame);
      SendMessageW(window,WM_TIMER,1,0);
      SendMessageW(window,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);
      require(std::memcmp(pixels,fixedHeader.data(),headerBytes)==0,"fixed header remains identical while the waveform animates");
    }
    require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)<=before+2,"animation does not leak drawing resources");
    {std::lock_guard<std::mutex> lock(mutex);require(edits==1,"animated repaint never resets a parameter");}
    if(argc>1) {
    BITMAPFILEHEADER header{};header.bfType=0x4D42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+w*h*4;
    FILE* f=std::fopen(argv[1],"wb");require(f!=nullptr,"snapshot file opens");
    std::fwrite(&header,sizeof(header),1,f);std::fwrite(&info.bmiHeader,sizeof(BITMAPINFOHEADER),1,f);
    std::fwrite(pixels,w*h*4,1,f);std::fclose(f);
    }
    SelectObject(dc,old);DeleteObject(bmp);DeleteDC(dc);ReleaseDC(window,screen);
  }
  SendMessageW(confirm,BM_CLICK,0,0);
  require(IsWindow(window) && !IsWindowVisible(window),"Confirm hides without unloading");
  {std::lock_guard<std::mutex> lock(mutex);require(edits==1,"Confirm retains settings without rewriting them");}
  SendMessageW(window,WM_CLOSE,0,0); require(IsWindow(window),"closing hides the window without destroying its instance");
  scope->show(); scope.reset(); require(!IsWindow(window),"instance destruction closes and joins the scope");
  DestroyWindow(host);
  // Repeated rapid open/unload catches the startup/destructor race.
  for(int i=0;i<20;i++) {Scope rapid([](ScopeEdit,double){});rapid.show();}
  std::printf("Scope checks passed: branding/icons, topmost, host restoration, Confirm, drag, stable header, read-only refresh, host band changes, hide, reopen, unload, 20 rapid lifecycles (%d edits).\n",edits);
}
