#include "core/config.h"

#include <windows.h>

namespace scpatch::config {
namespace {

std::wstring g_path;

}  // namespace

void Load(const std::wstring& path) { g_path = path; }

bool GetBool(const wchar_t* section, const wchar_t* key, bool fallback) {
  return GetInt(section, key, fallback ? 1 : 0) != 0;
}

int GetInt(const wchar_t* section, const wchar_t* key, int fallback) {
  return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, g_path.c_str()));
}

std::wstring GetString(const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
  wchar_t buffer[1024];
  GetPrivateProfileStringW(section, key, fallback, buffer, ARRAYSIZE(buffer), g_path.c_str());
  return buffer;
}

}  // namespace scpatch::config
