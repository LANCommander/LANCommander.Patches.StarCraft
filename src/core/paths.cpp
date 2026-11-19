#include "core/paths.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

namespace scpatch::paths {
namespace {

std::wstring g_gameDir;
std::wstring g_userRoot;

std::wstring ResolveUserRoot() {
  std::wstring root;
  PWSTR savedGames = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, KF_FLAG_CREATE, nullptr, &savedGames))) {
    root = savedGames;
  } else {
    wchar_t profile[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    root = std::wstring(profile, n < MAX_PATH ? n : 0) + L"\\Saved Games";
  }
  CoTaskMemFree(savedGames);
  return root + L"\\Blizzard Entertainment\\StarCraft";
}

}  // namespace

void Initialize() {
  wchar_t exe[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  g_gameDir.assign(exe, n);
  g_gameDir.erase(g_gameDir.find_last_of(L'\\'));
  g_userRoot = ResolveUserRoot();
  EnsureDirectory(g_userRoot);
}

const std::wstring& GameDir() { return g_gameDir; }
const std::wstring& UserDataRoot() { return g_userRoot; }

std::wstring ConfigFile() { return g_gameDir + L"\\scpatch.ini"; }
std::wstring LogFile() { return g_userRoot + L"\\scpatch.log"; }

bool EnsureDirectory(const std::wstring& path) {
  int rc = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
  return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS;
}

}  // namespace scpatch::paths
