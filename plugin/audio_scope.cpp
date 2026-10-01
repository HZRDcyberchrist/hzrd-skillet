#include "audio_scope.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cwchar>
#include <cstdlib>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

namespace hzrdaudio {
namespace {
constexpr COLORREF BG = RGB(0,0,0), PANEL = RGB(8,8,8), GRID = RGB(35,35,35);
constexpr COLORREF INK = RGB(217,233,224), MUTED = RGB(151,178,162), GREEN = RGB(61,255,154);
constexpr COLORREF AMBER = RGB(255,179,71);
std::wstring wide(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L' ');
  if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}
void box(HDC dc, RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF c, int width = 1) {
  HPEN pen = CreatePen(PS_SOLID, width, c); HGDIOBJ old = SelectObject(dc, pen);
  MoveToEx(dc, x1, y1, nullptr); LineTo(dc, x2, y2); SelectObject(dc, old); DeleteObject(pen);
}
template<size_t N>
void traceLine(HDC dc, const std::array<POINT,N>& points, COLORREF c, int width=1) {
  HPEN pen=CreatePen(PS_SOLID,width,c); HGDIOBJ old=SelectObject(dc,pen);
  Polyline(dc,points.data(),static_cast<int>(N)); SelectObject(dc,old); DeleteObject(pen);
}
void label(HDC dc, int x, int y, const std::wstring& s, COLORREF c = INK) {
  SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT); TextOutW(dc, x, y, s.c_str(), static_cast<int>(s.size()));
}
}  // namespace

struct Scope::Impl {
  std::function<void(ScopeEdit, double)> edit;
  std::mutex mutex, lifecycle;
  ScopeFrame frame;
  std::array<float, 256> history{};
  size_t head = 0;
  std::thread thread;
  std::atomic<HWND> hwnd{nullptr};
  std::atomic<HWND> host{nullptr};
  std::atomic<bool> stopping{false};
  HFONT font = nullptr;
  HFONT titleFont = nullptr;
  HFONT archiveFont = nullptr;
  HANDLE monoResource = nullptr, titleResource = nullptr;
  HICON icon = nullptr, smallIcon = nullptr;
  HBITMAP archive = nullptr, sigil = nullptr;
  HWND confirm = nullptr;
  bool dragging = false;
  std::string headerSource;
  Settings headerSettings;
  explicit Impl(std::function<void(ScopeEdit, double)> cb) : edit(std::move(cb)) {}
  static HANDLE loadFont(HMODULE module,int id) {
    HRSRC resource=FindResourceW(module,MAKEINTRESOURCEW(id),MAKEINTRESOURCEW(10));
    if(!resource) return nullptr;
    HGLOBAL loaded=LoadResource(module,resource); DWORD count=0;
    return AddFontMemResourceEx(LockResource(loaded),SizeofResource(module,resource),nullptr,&count);
  }
  static HBITMAP transparentSigil(HBITMAP source) {
    if(!source) return nullptr;
    BITMAP image{};GetObjectW(source,sizeof image,&image);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=image.bmWidth;info.bmiHeader.biHeight=-image.bmHeight;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    std::vector<DWORD> pixels(static_cast<size_t>(image.bmWidth)*image.bmHeight);
    HDC dc=GetDC(nullptr);
    if(!GetDIBits(dc,source,0,image.bmHeight,pixels.data(),&info,DIB_RGB_COLORS)) {
      ReleaseDC(nullptr,dc);return source;
    }
    void* memory=nullptr;
    HBITMAP transparent=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&memory,nullptr,0);
    ReleaseDC(nullptr,dc);if(!transparent) return source;
    auto target=static_cast<DWORD*>(memory);
    for(size_t i=0;i<pixels.size();i++) {
      const DWORD color=pixels[i]&0x00ffffff;
      const DWORD alpha=std::max({color&255,(color>>8)&255,(color>>16)&255});
      // RGB was rendered over black: it already contains premultiplied ink.
      // Empty pixels receive zero alpha, preserving the underlying panel.
      target[i]=color|(alpha<<24);
    }
    DeleteObject(source);return transparent;
  }
  static void bitmap(HDC dc,HBITMAP image,int x,int y,int width,int height,BYTE opacity=255,bool transparent=false) {
    if(!image) return;
    BITMAP info{}; GetObjectW(image,sizeof info,&info);
    HDC source=CreateCompatibleDC(dc);HGDIOBJ old=SelectObject(source,image);
    int mode=SetStretchBltMode(dc,HALFTONE);SetBrushOrgEx(dc,0,0,nullptr);
    if(opacity<255) AlphaBlend(dc,x,y,width,height,source,0,0,info.bmWidth,info.bmHeight,
                               BLENDFUNCTION{AC_SRC_OVER,0,opacity,static_cast<BYTE>(transparent ? AC_SRC_ALPHA : 0)});
    else StretchBlt(dc,x,y,width,height,source,0,0,info.bmWidth,info.bmHeight,SRCPAINT);
    SetStretchBltMode(dc,mode);SelectObject(source,old);DeleteDC(source);
  }
  void backdrop(HDC dc,const RECT& client) {
    // Contain the full image with a margin, preserving its original aspect.
    // Every panel uses these same coordinates so the backdrop stays continuous.
    if(!sigil) return;
    BITMAP image{};GetObjectW(sigil,sizeof image,&image);
    if(image.bmWidth<=0 || image.bmHeight<=0) return;
    const RECT area{24,48,client.right-24,client.bottom-122};
    const int availableWidth=std::max(1L,area.right-area.left);
    const int availableHeight=std::max(1L,area.bottom-area.top);
    const double scale=std::min(static_cast<double>(availableWidth)/image.bmWidth,
                                static_cast<double>(availableHeight)/image.bmHeight);
    const int width=std::max(1,static_cast<int>(image.bmWidth*scale));
    const int height=std::max(1,static_cast<int>(image.bmHeight*scale));
    bitmap(dc,sigil,area.left+(availableWidth-width)/2,
           area.top+(availableHeight-height)/2,width,height,65,true);
  }
  void surface(HDC dc,const RECT& region,const RECT& client,COLORREF color) {
    const int saved=SaveDC(dc);
    IntersectClipRect(dc,region.left,region.top,region.right,region.bottom);
    box(dc,region,color);backdrop(dc,client);
    RestoreDC(dc,saved);
  }
  static BOOL CALLBACK findHost(HWND candidate, LPARAM data) {
    auto self=reinterpret_cast<Impl*>(data); DWORD process=0;
    GetWindowThreadProcessId(candidate,&process);
    wchar_t className[128]{}; GetClassNameW(candidate,className,128);
    if(process==GetCurrentProcessId() && candidate!=self->hwnd.load() &&
       IsWindowVisible(candidate) && GetWindow(candidate,GW_OWNER)==nullptr &&
       std::wcscmp(className,L"HZRDSkilletAudioScopeV12")!=0) {
      self->host=candidate; return FALSE;
    }
    return TRUE;
  }
  void rememberHost() {
    if(IsWindow(host.load())) return;
    HWND foreground=GetForegroundWindow(); DWORD process=0;
    GetWindowThreadProcessId(foreground,&process);
    wchar_t className[128]{}; GetClassNameW(foreground,className,128);
    if(process==GetCurrentProcessId() && foreground!=hwnd.load() &&
       std::wcscmp(className,L"HZRDSkilletAudioScopeV12")!=0)
      host=GetAncestor(foreground,GA_ROOT);
    else EnumWindows(findHost,reinterpret_cast<LPARAM>(this));
  }
  void restoreHost() {
    rememberHost(); HWND main=host.load();
    if(IsWindow(main) && IsIconic(main)) {
      ShowWindowAsync(main,SW_RESTORE);
      SetWindowPos(main,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_ASYNCWINDOWPOS|SWP_NOACTIVATE);
    }
  }
  void layout() {
    RECT client{}; GetClientRect(hwnd.load(),&client);
    if(confirm) MoveWindow(confirm,24,client.bottom-65,140,36,TRUE);
  }
  void paintConfirm(HDC dc,RECT r,bool pressed,bool focused) {
    box(dc,r,pressed ? RGB(28,28,28) : BG);
    line(dc,r.left,r.top,r.right,r.top,GREEN);
    line(dc,r.left,r.bottom-1,r.right,r.bottom-1,GREEN);
    line(dc,r.left,r.top,r.left,r.bottom,GREEN);
    line(dc,r.right-1,r.top,r.right-1,r.bottom,GREEN);
    HGDIOBJ old=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,GREEN);
    DrawTextW(dc,L"CONFIRM",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    if(focused) {InflateRect(&r,-4,-4);DrawFocusRect(dc,&r);}
    SelectObject(dc,old);
  }
  ~Impl() {
    stopping = true;
    if (HWND w = hwnd.load()) PostMessageW(w, WM_CLOSE, 0, 0);
    if (thread.joinable()) thread.join();
  }
  RECT historyRect() const {
    RECT client{}; GetClientRect(hwnd.load(), &client);
    return {24, 305, std::max(100L, client.right - 24), 425};
  }
  void change(ScopeEdit kind, double value) {
    { std::lock_guard<std::mutex> lock(mutex);
      if (kind == ScopeEdit::Threshold) frame.settings.threshold = value;
      if (kind == ScopeEdit::Focus) frame.settings.focus = static_cast<Focus>(static_cast<int>(value));
    }
    edit(kind, value);
  }
  void drag(int y) {
    const RECT r = historyRect();
    change(ScopeEdit::Threshold, std::clamp(static_cast<double>(r.bottom - y) / (r.bottom - r.top), 0.0, 1.0));
    RECT changed{24,281,r.right,425}; InvalidateRect(hwnd.load(), &changed, FALSE);
  }
  void refresh() {
    ScopeFrame s; { std::lock_guard<std::mutex> lock(mutex); s=frame; }
    HWND w=hwnd.load(); RECT client{}; GetClientRect(w,&client);
    // Keep fixed labels/header out of the animation region. Only the live
    // traces, level readout and bars need repainting at 30 fps.
    for(const RECT& r : {RECT{24,128,client.right-24,260},RECT{24,281,client.right-24,425},
                        RECT{24,486,client.right-24,std::max(530L,client.bottom-180)}})
      InvalidateRect(w,&r,FALSE);
    if(s.source!=headerSource || s.settings.focus!=headerSettings.focus ||
       s.settings.lowHz!=headerSettings.lowHz || s.settings.highHz!=headerSettings.highHz) {
      RECT header{24,16,client.right-24,96}; InvalidateRect(w,&header,FALSE);
      headerSource=s.source; headerSettings=s.settings;
    }
  }
  void paint(HDC dc) {
    RECT client{}; GetClientRect(hwnd.load(), &client);
    box(dc, client, BG);backdrop(dc,client);
    HGDIOBJ oldFont = SelectObject(dc, font);
    ScopeFrame s; std::array<float,256> hist; size_t first;
    { std::lock_guard<std::mutex> lock(mutex); s = frame; hist = history; first = head; }
    HGDIOBJ headingFont=SelectObject(dc,titleFont);
    label(dc, 24, 10, L"HZRD//SKILLET  /  AUDIO SCOPE", GREEN);
    SelectObject(dc,headingFont);
    RECT source{24, 40, client.right - 24, 60};
    SetTextColor(dc, MUTED); SetBkMode(dc, TRANSPARENT);
    const auto sourceName = wide(s.source);
    DrawTextW(dc, sourceName.c_str(), -1, &source, DT_SINGLELINE | DT_END_ELLIPSIS);
    const int focusIndex=std::clamp(static_cast<int>(s.settings.focus),0,static_cast<int>(Focus::Count)-1);
    std::wstring focusLabel=L"> FOCUS   "+wide(FOCUS_NAMES[focusIndex]);
    if(s.settings.focus==Focus::Custom) {
      const Band band=focusBand(s.settings,s.sampleRate); wchar_t range[64];
      std::swprintf(range,64,L"   %.0f - %.0f Hz",band.low,band.high); focusLabel+=range;
    }
    label(dc,24,72,focusLabel,GREEN);

    RECT wave{24, 128, client.right - 24, 260}; surface(dc,wave,client,PANEL);
    label(dc, 24, 105, L"WAVEFORM    gray: input    green: selected sound", MUTED);
    const int center = (wave.top + wave.bottom) / 2, half = (wave.bottom - wave.top) / 2 - 5;
    line(dc, wave.left, center, wave.right, center, GRID);
    auto trace = [&](const auto& samples, COLORREF c) {
      std::array<POINT,SCOPE_SAMPLES> points;
      for (size_t i = 0; i < samples.size(); i++) {
        points[i]={wave.left+static_cast<LONG>(i*(wave.right-wave.left)/(samples.size()-1)),
                   center-static_cast<LONG>(std::clamp(samples[i],-1.0f,1.0f)*half)};
      }
      traceLine(dc,points,c);
    };
    trace(s.waveform, RGB(69,89,77)); trace(s.focusedWaveform, GREEN);
    wchar_t caption[180];
    std::swprintf(caption, 180, L"REACTIVE LINE  %.0f%%    selected level  %.0f%%    %ls",
                  s.settings.threshold*100, s.level*100, s.hit > 0.6 ? L"HIT" : L"");
    label(dc, 24, 281, caption, AMBER);
    RECT h = historyRect();surface(dc,h,client,PANEL);
    for (int i = 1; i < 4; i++) line(dc, h.left, h.top + i*(h.bottom-h.top)/4, h.right, h.top + i*(h.bottom-h.top)/4, GRID);
    std::array<POINT,256> historyPoints;
    for (size_t i = 0; i < hist.size(); i++) {
      historyPoints[i]={h.left+static_cast<LONG>(i*(h.right-h.left)/(hist.size()-1)),
                       h.bottom-static_cast<LONG>(std::clamp(hist[(first+i)%hist.size()],0.0f,1.0f)*(h.bottom-h.top))};
    }
    traceLine(dc,historyPoints,GREEN,2);
    const int triggerY = h.bottom - static_cast<int>(s.settings.threshold*(h.bottom-h.top));
    line(dc, h.left, triggerY, h.right, triggerY, AMBER, 2);
    label(dc, 24, 435, L"Drag amber line: higher = stronger sounds only. 0% = always open.", MUTED);

    RECT spec{24, 486, client.right - 24, std::max(530L, client.bottom - 180)}; surface(dc,spec,client,PANEL);
    label(dc, 24, 463, L"FREQUENCIES    shaded area: selected band", MUTED);
    const Band b = s.settings.focus == Focus::FullMix ? Band{20,16000} : focusBand(s.settings,s.sampleRate);
    const int bandLeft = spec.left + static_cast<int>(frequencyToControl(b.low)*(spec.right-spec.left));
    const int bandRight = spec.left + static_cast<int>(frequencyToControl(b.high)*(spec.right-spec.left));
    surface(dc,RECT{bandLeft,spec.top,bandRight,spec.bottom},client,RGB(27,53,36));
    for (int i = 0; i < SPECTRUM_BINS; i++) {
      const int x = spec.left + i*(spec.right-spec.left)/SPECTRUM_BINS;
      const int right = spec.left + (i+1)*(spec.right-spec.left)/SPECTRUM_BINS - 2;
      const int top = spec.bottom - static_cast<int>(std::clamp(s.spectrum[i],0.0f,1.0f)*(spec.bottom-spec.top));
      box(dc, RECT{x,top,right,spec.bottom}, x >= bandLeft && x <= bandRight ? GREEN : RGB(70,92,78));
    }
    label(dc, spec.left, spec.bottom+6, L"20 Hz", MUTED);
    label(dc, spec.left+(spec.right-spec.left)/3, spec.bottom+6, L"bass / kick", MUTED);
    label(dc, spec.left+2*(spec.right-spec.left)/3, spec.bottom+6, L"mids", MUTED);
    label(dc, spec.right-66, spec.bottom+6, L"16 kHz", MUTED);
    line(dc,24,client.bottom-146,client.right-24,client.bottom-146,RGB(15,90,54));
    label(dc, 24, client.bottom-126, L"> settings apply live / confirm to close", MUTED);
    const int brandCenter=client.right-156;
    // The code-rendered HZRD and small white ARCHIVE share one center.
    bitmap(dc,archive,brandCenter-132,client.bottom-100,264,64);
    HGDIOBJ footerFont=SelectObject(dc,archiveFont);
    int spacing=SetTextCharacterExtra(dc,3);SIZE archiveSize{};
    GetTextExtentPoint32W(dc,L"ARCHIVE",7,&archiveSize);
    label(dc,brandCenter-archiveSize.cx/2,client.bottom-26,L"ARCHIVE",RGB(214,255,233));
    SetTextCharacterExtra(dc,spacing);SelectObject(dc,footerFont);
    paintConfirm(dc,RECT{24,client.bottom-65,164,client.bottom-29},false,false);
    SelectObject(dc, oldFont);
  }
  static LRESULT CALLBACK proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
      self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
      SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->hwnd = w;
    }
    if (!self) return DefWindowProcW(w,msg,wp,lp);
    switch (msg) {
      case WM_CREATE: {
        HMODULE module=reinterpret_cast<HMODULE>(GetWindowLongPtrW(w,GWLP_HINSTANCE));
        self->monoResource=loadFont(module,105);self->titleResource=loadFont(module,106);
        self->icon=reinterpret_cast<HICON>(LoadImageW(module,MAKEINTRESOURCEW(101),IMAGE_ICON,64,64,0));
        self->smallIcon=reinterpret_cast<HICON>(LoadImageW(module,MAKEINTRESOURCEW(101),IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),0));
        SendMessageW(w,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(self->icon));
        SendMessageW(w,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(self->smallIcon));
        self->archive=reinterpret_cast<HBITMAP>(LoadImageW(module,MAKEINTRESOURCEW(103),IMAGE_BITMAP,0,0,LR_CREATEDIBSECTION));
        self->sigil=transparentSigil(reinterpret_cast<HBITMAP>(LoadImageW(module,MAKEINTRESOURCEW(104),IMAGE_BITMAP,0,0,LR_CREATEDIBSECTION)));
        self->font = CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH,L"IBM Plex Mono");
        self->titleFont=CreateFontW(-28,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH,L"VT323");
        self->archiveFont=CreateFontW(-12,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH,L"IBM Plex Mono");
        if(HMODULE dwm=LoadLibraryW(L"dwmapi.dll")) {
          using Attribute=HRESULT(WINAPI*)(HWND,DWORD,LPCVOID,DWORD);
          auto setAttribute=reinterpret_cast<Attribute>(GetProcAddress(dwm,"DwmSetWindowAttribute"));
          if(setAttribute) {BOOL dark=TRUE;COLORREF caption=BG,text=GREEN;
            setAttribute(w,20,&dark,sizeof dark);setAttribute(w,35,&caption,sizeof caption);
            setAttribute(w,36,&text,sizeof text);}
          FreeLibrary(dwm);
        }
        self->confirm=CreateWindowExW(0,L"BUTTON",L"Confirm",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
                                     0,0,140,36,w,reinterpret_cast<HMENU>(1),
                                     reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(w,GWLP_HINSTANCE)),nullptr);
        self->layout();
        SetTimer(w,1,33,nullptr); return 0;
      }
      case WM_LBUTTONDOWN: {
        self->restoreHost();
        POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; const RECT r=self->historyRect();
        if (p.x>=r.left && p.x<=r.right && p.y>=r.top-8 && p.y<=r.bottom+8) {
          self->dragging=true; SetCapture(w); self->drag(p.y); return 0;
        }
        break;
      }
      case WM_MOUSEMOVE: if (self->dragging) { self->drag(GET_Y_LPARAM(lp)); return 0; } break;
      case WM_LBUTTONUP: if (self->dragging) { self->dragging=false; ReleaseCapture(); return 0; } break;
      case WM_CAPTURECHANGED: self->dragging=false; return 0;
      case WM_TIMER:
        // This window is only a monitor. No control values or selections are
        // rewritten on a timer; all options live in Resolume's Audio foldout.
#if !defined(SKILLET_SCOPE_TEST)
        if (!IsWindowVisible(w)) return 0;
#endif
        self->refresh(); return 0;
      case WM_COMMAND:
        if(LOWORD(wp)==1 && HIWORD(wp)==BN_CLICKED) {ShowWindow(w,SW_HIDE);return 0;}
        break;
      case WM_DRAWITEM: {
        auto draw=reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if(draw->CtlID==1) {self->paintConfirm(draw->hDC,draw->rcItem,
                            (draw->itemState&ODS_SELECTED)!=0,(draw->itemState&ODS_FOCUS)!=0);return TRUE;}
        break;
      }
      case WM_KEYDOWN:
        if(wp==VK_RETURN || wp==VK_ESCAPE) {ShowWindow(w,SW_HIDE);return 0;}
        break;
      case WM_SYSCOMMAND:
        if((wp&0xfff0)==SC_RESTORE) self->restoreHost();
        break;
      case WM_ACTIVATE:
        if(LOWORD(wp)!=WA_INACTIVE) self->restoreHost();
        break;
      case WM_SIZE:
        if(wp!=SIZE_MINIMIZED) {self->layout();InvalidateRect(w,nullptr,FALSE);}
        return 0;
      case WM_GETMINMAXINFO: {
        auto limits=reinterpret_cast<MINMAXINFO*>(lp); limits->ptMinTrackSize={800,750}; return 0;
      }
      case WM_ERASEBKGND: return 1;
      case WM_PRINTCLIENT: self->paint(reinterpret_cast<HDC>(wp)); return 0;
      case WM_PAINT: {
        PAINTSTRUCT ps; HDC target=BeginPaint(w,&ps); RECT r{}; GetClientRect(w,&r);
        if (r.right>0 && r.bottom>0) {
          HDC buffer=CreateCompatibleDC(target); HBITMAP image=CreateCompatibleBitmap(target,r.right,r.bottom);
          HGDIOBJ old=SelectObject(buffer,image); self->paint(buffer); BitBlt(target,0,0,r.right,r.bottom,buffer,0,0,SRCCOPY);
          SelectObject(buffer,old); DeleteObject(image); DeleteDC(buffer);
        }
        EndPaint(w,&ps); return 0;
      }
      case WM_APP+1:
        self->restoreHost();
#if !defined(SKILLET_SCOPE_TEST)
        ShowWindow(w,IsIconic(w) ? SW_RESTORE : SW_SHOW);
        SetWindowPos(w,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE); SetForegroundWindow(w);
#endif
        return 0;
      case WM_CLOSE:
        if (self->stopping) DestroyWindow(w); else ShowWindow(w,SW_HIDE); return 0;
      case WM_DESTROY:
        KillTimer(w,1); if (self->font) DeleteObject(self->font); self->font=nullptr;
        if(self->titleFont) DeleteObject(self->titleFont);self->titleFont=nullptr;
        if(self->archiveFont) DeleteObject(self->archiveFont);self->archiveFont=nullptr;
        for(HBITMAP image : {self->archive,self->sigil}) if(image) DeleteObject(image);
        self->archive=self->sigil=nullptr;
        if(self->icon) DestroyIcon(self->icon);if(self->smallIcon) DestroyIcon(self->smallIcon);
        self->icon=self->smallIcon=nullptr;
        if(self->monoResource) RemoveFontMemResourceEx(self->monoResource);
        if(self->titleResource) RemoveFontMemResourceEx(self->titleResource);
        self->monoResource=self->titleResource=nullptr;self->confirm=nullptr;
        self->hwnd=nullptr; PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
  }
  void run() {
    HMODULE module=nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&proc),&module);
    const wchar_t* name=L"HZRDSkilletAudioScopeV12";
    WNDCLASSEXW klass{}; klass.cbSize=sizeof klass; klass.lpfnWndProc=proc; klass.hInstance=module;
    klass.lpszClassName=name; klass.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
    RegisterClassExW(&klass);  // repeated instances reuse this DLL's class
    HWND w=CreateWindowExW(WS_EX_APPWINDOW|WS_EX_TOPMOST,name,L"HZRD//Skillet - Audio scope",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                           CW_USEDEFAULT,CW_USEDEFAULT,900,790,nullptr,nullptr,module,this);
    if (!w) { UnregisterClassW(name,module); return; }
    if (stopping) { DestroyWindow(w); UnregisterClassW(name,module); return; }
#if !defined(SKILLET_SCOPE_TEST)
    ShowWindow(w,SW_SHOWNORMAL);
#endif
    MSG msg;
    while (GetMessageW(&msg,nullptr,0,0)>0) {
      if (!IsDialogMessageW(w,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    UnregisterClassW(name,module);
  }
};
Scope::Scope(std::function<void(ScopeEdit,double)> cb) : impl_(new Impl(std::move(cb))) {}
Scope::~Scope() = default;
void Scope::show() {
  std::lock_guard<std::mutex> lock(impl_->lifecycle);
  impl_->rememberHost();
  if (HWND w=impl_->hwnd.load()) PostMessageW(w,WM_APP+1,0,0);
  else if (!impl_->thread.joinable()) {
    Impl* owned = impl_.get();  // the Impl destructor joins before freeing it
    impl_->thread=std::thread([owned]{owned->run();});
  }
}
void Scope::publish(const ScopeFrame& frame) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->frame=frame;
  impl_->history[impl_->head]=frame.connected ? static_cast<float>(frame.level) : 0;
  impl_->head=(impl_->head+1)%impl_->history.size();
}
}  // namespace hzrdaudio
#else
namespace hzrdaudio {
struct Scope::Impl {};
Scope::Scope(std::function<void(ScopeEdit,double)>) : impl_(new Impl) {}
Scope::~Scope() = default;
void Scope::show() {}
void Scope::publish(const ScopeFrame&) {}
}
#endif
