#include "core/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace scpatch::log {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_lock;
bool g_lockReady;

void Write(const char* level, const char* format, va_list args) {
  char message[1024];
  vsnprintf(message, sizeof(message), format, args);

  SYSTEMTIME t;
  GetLocalTime(&t);
  char line[1100];
  int n = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [%s] %s\r\n", t.wHour, t.wMinute, t.wSecond,
                   t.wMilliseconds, level, message);
  if (n < 0) return;
  if (n >= static_cast<int>(sizeof(line))) n = sizeof(line) - 1;

  OutputDebugStringA(line);
  if (g_file == INVALID_HANDLE_VALUE) return;
  EnterCriticalSection(&g_lock);
  DWORD written;
  WriteFile(g_file, line, static_cast<DWORD>(n), &written, nullptr);
  LeaveCriticalSection(&g_lock);
}

}  // namespace

void Open(const std::wstring& path) {
  if (!g_lockReady) {
    InitializeCriticalSection(&g_lock);
    g_lockReady = true;
  }
  g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

#define DEFINE_LEVEL(fn, tag)    \
  void fn(const char* format, ...) { \
    va_list args;                \
    va_start(args, format);      \
    Write(tag, format, args);    \
    va_end(args);                \
  }
DEFINE_LEVEL(Info, "info")
DEFINE_LEVEL(Warn, "warn")
DEFINE_LEVEL(Error, "error")
#undef DEFINE_LEVEL

}  // namespace scpatch::log
