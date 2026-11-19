#include "core/crash.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

#include "core/log.h"

namespace scpatch::crash {
namespace {

std::atomic<int> g_reports{0};
constexpr int kMaxReports = 3;

// "module+offset" for an address, or the raw address if it is not inside a loaded module.
void Describe(uintptr_t address, char* out, size_t size) {
  HMODULE module = nullptr;
  char name[MAX_PATH];
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(address), &module) &&
      GetModuleFileNameA(module, name, MAX_PATH)) {
    const char* base = strrchr(name, '\\');
    snprintf(out, size, "%08X (%s+%X)", static_cast<unsigned>(address), base ? base + 1 : name,
             static_cast<unsigned>(address - reinterpret_cast<uintptr_t>(module)));
  } else {
    snprintf(out, size, "%08X", static_cast<unsigned>(address));
  }
}

bool InCodeModule(uintptr_t address) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) return false;
  return mbi.Type == MEM_IMAGE &&
         (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
  DWORD code = info->ExceptionRecord->ExceptionCode;
  bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
               code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW ||
               code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
  if (!fatal || g_reports.fetch_add(1) >= kMaxReports) return EXCEPTION_CONTINUE_SEARCH;

  const CONTEXT* c = info->ContextRecord;
  char where[160];
  Describe(reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress), where, sizeof(where));
  log::Error("crash: exception %08X at %s", static_cast<unsigned>(code), where);
  if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
    ULONG_PTR kind = info->ExceptionRecord->ExceptionInformation[0];
    log::Error("crash: %s of address %08X", kind == 0 ? "read" : kind == 1 ? "write" : "execute",
               static_cast<unsigned>(info->ExceptionRecord->ExceptionInformation[1]));
  }
  log::Error("crash: EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X EBP=%08X ESP=%08X",
             static_cast<unsigned>(c->Eax), static_cast<unsigned>(c->Ebx), static_cast<unsigned>(c->Ecx),
             static_cast<unsigned>(c->Edx), static_cast<unsigned>(c->Esi), static_cast<unsigned>(c->Edi),
             static_cast<unsigned>(c->Ebp), static_cast<unsigned>(c->Esp));

  // Likely return addresses: stack slots pointing into executable images.
  auto* stack = reinterpret_cast<const uintptr_t*>(c->Esp);
  int found = 0;
  for (int i = 0; i < 256 && found < 16; ++i) {
    uintptr_t value;
    __try {
      value = stack[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      break;
    }
    if (!InCodeModule(value)) continue;
    char text[160];
    Describe(value, text, sizeof(text));
    log::Error("crash:   [esp+%03X] %s", i * 4, text);
    ++found;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallLogger() { AddVectoredExceptionHandler(1, Handler); }

}  // namespace scpatch::crash
