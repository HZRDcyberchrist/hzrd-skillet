// The user's Documents folder, where Windows really keeps it (OneDrive or a
// moved folder included). Its own file: shlobj.h needs the full Windows
// headers, and the rest of the plugin builds with WIN32_LEAN_AND_MEAN.
#include <string>

#if defined(_WIN32)
#undef WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

std::wstring skilletDocumentsFolder() {
  std::wstring dir;
  PWSTR p = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)) && p) dir = p;
  if (p) CoTaskMemFree(p);
  if (dir.empty()) {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) dir = std::wstring(buf) + L"\\Documents";
  }
  return dir;
}
#endif
