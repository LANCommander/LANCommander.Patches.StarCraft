// Relocates user data (saved games, characters, replays, error logs, screenshots) out of the install folder
// into %USERPROFILE%\Saved Games\Blizzard Entertainment\StarCraft.
//
// The game builds every one of these paths as <directory of StarCraft.exe> + a literal suffix (see
// re/notes/saves.md). Rather than patching each place the game formats a path, the file APIs imported by StarCraft.exe and
// storm.dll are hooked and any path that resolves inside one of the relocated install subfolders is
// rewritten. The game keeps believing its data lives in the install folder. Redirected calls go through the
// wide-character APIs so profile paths outside the ANSI code page still work.

#include <windows.h>

#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/patch.h"
#include "core/paths.h"
#include "features/features.h"

namespace scpatch::features {
namespace {

struct Redirect {
  const wchar_t* installSubdir;  // relative to the game folder
  const wchar_t* userSubdir;     // relative to the user data root
  std::wstring target;           // resolved absolute target
};

Redirect g_redirects[] = {
    {L"save", L"Saves"},
    {L"characters", L"Characters"},
    {L"maps\\replays", L"Replays"},
    {L"Errors", L"Errors"},
};

// Screenshots are written with a bare file name, i.e. into the current directory (normally the game folder).
constexpr wchar_t kScreenshotPrefix[] = L"SCScrnShot_";
std::wstring g_screenshotDir;

std::wstring Widen(const char* s) {
  int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
  std::wstring out(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_ACP, 0, s, -1, out.data(), n);
  return out;
}

void Narrow(const wchar_t* s, char* out, int outSize) {
  if (!WideCharToMultiByte(CP_ACP, 0, s, -1, out, outSize, nullptr, nullptr) && outSize > 0) out[0] = '\0';
}

bool StartsWithDir(const std::wstring& path, const std::wstring& dir) {
  if (path.size() < dir.size() || _wcsnicmp(path.c_str(), dir.c_str(), dir.size()) != 0) return false;
  return path.size() == dir.size() || path[dir.size()] == L'\\';
}

// Returns true and fills `out` if `path` lies inside a relocated folder. Relative paths are resolved against
// the current directory, which is how the game itself would resolve them.
bool Rewrite(const char* path, std::wstring& out) {
  if (!path || !*path) return false;
  std::wstring wide = Widen(path);
  wchar_t full[MAX_PATH * 2];
  DWORD n = GetFullPathNameW(wide.c_str(), ARRAYSIZE(full), full, nullptr);
  if (n == 0 || n >= ARRAYSIZE(full)) return false;
  std::wstring absolute(full, n);

  for (const Redirect& r : g_redirects) {
    std::wstring from = paths::GameDir() + L"\\" + r.installSubdir;
    if (StartsWithDir(absolute, from)) {
      out = r.target + absolute.substr(from.size());
      return true;
    }
  }

  size_t slash = absolute.find_last_of(L'\\');
  std::wstring name = absolute.substr(slash + 1);
  if (_wcsicmp(absolute.substr(0, slash).c_str(), paths::GameDir().c_str()) == 0 &&
      _wcsnicmp(name.c_str(), kScreenshotPrefix, ARRAYSIZE(kScreenshotPrefix) - 1) == 0) {
    out = g_screenshotDir + L"\\" + name;
    return true;
  }
  return false;
}

// The kernel32 exports the hooks forward to when a path is not redirected.
struct Originals {
  decltype(&CreateFileA) createFile;
  decltype(&GetFileAttributesA) getFileAttributes;
  decltype(&SetFileAttributesA) setFileAttributes;
  decltype(&FindFirstFileA) findFirstFile;
  decltype(&DeleteFileA) deleteFile;
  decltype(&CreateDirectoryA) createDirectory;
};
Originals g_orig;

HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                               DWORD flags, HANDLE templ) {
  std::wstring redirected;
  if (Rewrite(name, redirected)) {
    return CreateFileW(redirected.c_str(), access, share, sa, disposition, flags, templ);
  }
  return g_orig.createFile(name, access, share, sa, disposition, flags, templ);
}

DWORD WINAPI Hook_GetFileAttributesA(LPCSTR name) {
  std::wstring redirected;
  if (Rewrite(name, redirected)) return GetFileAttributesW(redirected.c_str());
  return g_orig.getFileAttributes(name);
}

BOOL WINAPI Hook_SetFileAttributesA(LPCSTR name, DWORD attributes) {
  std::wstring redirected;
  if (Rewrite(name, redirected)) return SetFileAttributesW(redirected.c_str(), attributes);
  return g_orig.setFileAttributes(name, attributes);
}

HANDLE WINAPI Hook_FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data) {
  std::wstring redirected;
  if (!Rewrite(pattern, redirected)) return g_orig.findFirstFile(pattern, data);

  // FindNextFileA cannot continue a search started with FindFirstFileW, so enumerate the redirected folder
  // with the ANSI API using its short (8.3) form when the long form is not representable in the code page.
  char ansi[MAX_PATH];
  BOOL lossy = FALSE;
  WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, redirected.c_str(), -1, ansi, MAX_PATH, nullptr, &lossy);
  if (lossy) {
    std::wstring dir = redirected.substr(0, redirected.find_last_of(L'\\'));
    wchar_t shortDir[MAX_PATH];
    if (GetShortPathNameW(dir.c_str(), shortDir, MAX_PATH)) {
      std::wstring shortPattern = std::wstring(shortDir) + redirected.substr(dir.size());
      Narrow(shortPattern.c_str(), ansi, MAX_PATH);
    }
  }
  return g_orig.findFirstFile(ansi, data);
}

BOOL WINAPI Hook_DeleteFileA(LPCSTR name) {
  std::wstring redirected;
  if (Rewrite(name, redirected)) return DeleteFileW(redirected.c_str());
  return g_orig.deleteFile(name);
}

BOOL WINAPI Hook_CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa) {
  std::wstring redirected;
  if (Rewrite(name, redirected)) return CreateDirectoryW(redirected.c_str(), sa);
  return g_orig.createDirectory(name, sa);
}

// Copies `from` into `to` recursively without overwriting anything already present.
void CopyTree(const std::wstring& from, const std::wstring& to, int& copied) {
  paths::EnsureDirectory(to);
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      CopyTree(from + L"\\" + name, to + L"\\" + name, copied);
    } else if (CopyFileW((from + L"\\" + name).c_str(), (to + L"\\" + name).c_str(), TRUE)) {
      ++copied;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

// One-time copy of existing data so players keep their saves after installing the patch. The originals are
// left in place; nothing in the install folder is deleted.
void Migrate(const Redirect& r) {
  std::wstring from = paths::GameDir() + L"\\" + r.installSubdir;
  DWORD attrs = GetFileAttributesW(from.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return;
  if (GetFileAttributesW(r.target.c_str()) != INVALID_FILE_ATTRIBUTES) return;  // already migrated
  int copied = 0;
  CopyTree(from, r.target, copied);
  log::Info("save paths: migrated %d file(s) from %ls to %ls", copied, from.c_str(), r.target.c_str());
}

void HookModule(HMODULE module, const char* label) {
  const struct {
    const char* name;
    void* hook;
  } entries[] = {
      {"CreateFileA", reinterpret_cast<void*>(&Hook_CreateFileA)},
      {"GetFileAttributesA", reinterpret_cast<void*>(&Hook_GetFileAttributesA)},
      {"SetFileAttributesA", reinterpret_cast<void*>(&Hook_SetFileAttributesA)},
      {"FindFirstFileA", reinterpret_cast<void*>(&Hook_FindFirstFileA)},
      {"DeleteFileA", reinterpret_cast<void*>(&Hook_DeleteFileA)},
      {"CreateDirectoryA", reinterpret_cast<void*>(&Hook_CreateDirectoryA)},
  };
  int hooked = 0;
  for (const auto& e : entries) {
    if (patch::HookImport(module, "KERNEL32.dll", e.name, e.hook, nullptr)) ++hooked;
  }
  log::Info("save paths: hooked %d file API(s) in %s", hooked, label);
}

// Exercises the hooks with the path shapes the game uses; enabled with SCPATCH_SELFTEST=1.
void SelfTest() {
  std::string game;
  {
    char buf[MAX_PATH * 2];
    Narrow(paths::GameDir().c_str(), buf, sizeof(buf));
    game = buf;
  }
  const std::string cases[] = {
      game + "\\save\\SelfTest\\selftest.snx",
      game + "\\maps\\replays\\selftest.rep",
      game + "\\characters\\selftest.spc",
      "SCScrnShot_000000_000000.pcx",  // relative, as the game writes screenshots
  };
  Hook_CreateDirectoryA((game + "\\save\\SelfTest").c_str(), nullptr);
  for (const std::string& path : cases) {
    HANDLE h = Hook_CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    bool created = h != INVALID_HANDLE_VALUE;
    if (created) CloseHandle(h);
    std::wstring redirected;
    bool rewritten = Rewrite(path.c_str(), redirected);
    bool landed = rewritten && GetFileAttributesW(redirected.c_str()) != INVALID_FILE_ATTRIBUTES;
    WIN32_FIND_DATAA fd;
    HANDLE find = Hook_FindFirstFileA(path.c_str(), &fd);
    bool found = find != INVALID_HANDLE_VALUE;
    if (found) FindClose(find);
    bool deleted = Hook_DeleteFileA(path.c_str()) != FALSE;
    log::Info("selftest %s: %s created=%d redirected=%d found=%d deleted=%d -> %ls",
              created && landed && found && deleted ? "PASS" : "FAIL", path.c_str(), created, landed, found, deleted,
              redirected.c_str());
  }
  RemoveDirectoryW((paths::UserDataRoot() + L"\\Saves\\SelfTest").c_str());
}

}  // namespace

void InstallSavePaths() {
  if (!config::GetBool(L"SavePaths", L"Enabled", true)) {
    log::Info("save paths: disabled in config");
    return;
  }

  // Bind originals up front so a hook can never call through a null pointer, whichever module calls first.
  HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
  g_orig.createFile = reinterpret_cast<decltype(&CreateFileA)>(GetProcAddress(k32, "CreateFileA"));
  g_orig.getFileAttributes = reinterpret_cast<decltype(&GetFileAttributesA)>(GetProcAddress(k32, "GetFileAttributesA"));
  g_orig.setFileAttributes = reinterpret_cast<decltype(&SetFileAttributesA)>(GetProcAddress(k32, "SetFileAttributesA"));
  g_orig.findFirstFile = reinterpret_cast<decltype(&FindFirstFileA)>(GetProcAddress(k32, "FindFirstFileA"));
  g_orig.deleteFile = reinterpret_cast<decltype(&DeleteFileA)>(GetProcAddress(k32, "DeleteFileA"));
  g_orig.createDirectory = reinterpret_cast<decltype(&CreateDirectoryA)>(GetProcAddress(k32, "CreateDirectoryA"));

  for (Redirect& r : g_redirects) {
    r.target = paths::UserDataRoot() + L"\\" + r.userSubdir;
    if (config::GetBool(L"SavePaths", L"MigrateExisting", true)) Migrate(r);
    paths::EnsureDirectory(r.target);
  }
  g_screenshotDir = paths::UserDataRoot() + L"\\Screenshots";
  paths::EnsureDirectory(g_screenshotDir);

  HookModule(GetModuleHandleW(nullptr), "StarCraft.exe");
  if (HMODULE storm = GetModuleHandleW(L"storm.dll")) HookModule(storm, "storm.dll");

  wchar_t selftest[4];
  if (GetEnvironmentVariableW(L"SCPATCH_SELFTEST", selftest, ARRAYSIZE(selftest)) && selftest[0] == L'1') SelfTest();
}

}  // namespace scpatch::features
