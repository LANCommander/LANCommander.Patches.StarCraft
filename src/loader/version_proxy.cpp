// version.dll proxy: forwards the real API to the system DLL and bootstraps the patch.
//
// StarCraft.exe statically imports VERSION.dll, which is not a KnownDLL, so Windows resolves it from the
// game folder first. That gives us a load point before any game code runs without touching game files.

#include <windows.h>

#include "core/main.h"

namespace {

HMODULE g_realVersion;

#define VERSION_EXPORTS(X)            \
  X(GetFileVersionInfoA)              \
  X(GetFileVersionInfoByHandle)       \
  X(GetFileVersionInfoExA)            \
  X(GetFileVersionInfoExW)            \
  X(GetFileVersionInfoSizeA)          \
  X(GetFileVersionInfoSizeExA)        \
  X(GetFileVersionInfoSizeExW)        \
  X(GetFileVersionInfoSizeW)          \
  X(GetFileVersionInfoW)              \
  X(VerFindFileA)                     \
  X(VerFindFileW)                     \
  X(VerInstallFileA)                  \
  X(VerInstallFileW)                  \
  X(VerLanguageNameA)                 \
  X(VerLanguageNameW)                 \
  X(VerQueryValueA)                   \
  X(VerQueryValueW)

}  // namespace

// Resolved targets, read by the naked stubs below.
extern "C" {
#define DECLARE_TARGET(name) FARPROC g_real_##name;
VERSION_EXPORTS(DECLARE_TARGET)
#undef DECLARE_TARGET
}

namespace {

void LoadRealVersion() {
  wchar_t path[MAX_PATH];
  UINT n = GetSystemDirectoryW(path, MAX_PATH);
  if (n == 0 || n > MAX_PATH - 16) return;
  lstrcatW(path, L"\\version.dll");
  g_realVersion = LoadLibraryW(path);
  if (!g_realVersion) return;
#define RESOLVE(name) g_real_##name = GetProcAddress(g_realVersion, #name);
  VERSION_EXPORTS(RESOLVE)
#undef RESOLVE
}

}  // namespace

// Each export is a bare jump so the caller's arguments and return address pass through untouched.
#define DEFINE_STUB(name) \
  extern "C" __declspec(naked) void Proxy_##name() { __asm { jmp dword ptr [g_real_##name] } }
VERSION_EXPORTS(DEFINE_STUB)
#undef DEFINE_STUB

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    // First: the embedded Interposer imports GetFileVersionInfoW and friends, which bind to this DLL's own
    // exports (it is version.dll), so the forwards must be live before it runs.
    LoadRealVersion();
    if (!g_realVersion) return FALSE;
    scpatch::OnProcessAttach(instance);
  } else if (reason == DLL_PROCESS_DETACH) {
    scpatch::OnProcessDetach(reserved != nullptr);
  }
  return TRUE;
}
