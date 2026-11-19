#include "core/patch.h"

#include <cstring>
#include <vector>

#include "core/log.h"

namespace scpatch::patch {

bool WriteMemory(uintptr_t address, const void* data, size_t size) {
  void* target = reinterpret_cast<void*>(address);
  DWORD oldProtect;
  if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
  std::memcpy(target, data, size);
  VirtualProtect(target, size, oldProtect, &oldProtect);
  FlushInstructionCache(GetCurrentProcess(), target, size);
  return true;
}

bool Bytes(uintptr_t address, std::initializer_list<uint8_t> expected, std::initializer_list<uint8_t> replacement,
           const char* what) {
  if (expected.size() != replacement.size()) {
    log::Error("patch '%s': expected/replacement length mismatch", what);
    return false;
  }
  if (std::memcmp(reinterpret_cast<void*>(address), expected.begin(), expected.size()) != 0) {
    log::Error("patch '%s': unexpected bytes at %08X; not applied", what, static_cast<unsigned>(address));
    return false;
  }
  return WriteMemory(address, replacement.begin(), replacement.size());
}

bool WriteJump(uintptr_t address, void* target) {
  uint8_t code[5] = {0xE9};
  int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (address + 5));
  std::memcpy(code + 1, &rel, sizeof(rel));
  return WriteMemory(address, code, sizeof(code));
}

bool RedirectCall(uintptr_t address, uintptr_t expectedTarget, void* newTarget, const char* what) {
  auto code = reinterpret_cast<const uint8_t*>(address);
  int32_t rel;
  std::memcpy(&rel, code + 1, sizeof(rel));
  if (code[0] != 0xE8 || address + 5 + rel != expectedTarget) {
    log::Error("patch '%s': no call to %08X at %08X; not applied", what, static_cast<unsigned>(expectedTarget),
               static_cast<unsigned>(address));
    return false;
  }
  rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(newTarget) - (address + 5));
  return WriteMemory(address + 1, &rel, sizeof(rel));
}

bool HookImport(HMODULE module, const char* dll, const char* function, void* replacement, void** original,
                WORD ordinal) {
  auto base = reinterpret_cast<uint8_t*>(module);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (dir.VirtualAddress == 0) return false;

  for (auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
    if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) != 0) continue;
    // OriginalFirstThunk keeps the names; FirstThunk is the IAT that holds resolved pointers.
    auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                                                       : desc->FirstThunk));
    auto iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++iat) {
      bool match;
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
        match = function == nullptr && IMAGE_ORDINAL(names->u1.Ordinal) == ordinal;
      } else {
        auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
        match = function != nullptr && std::strcmp(reinterpret_cast<const char*>(byName->Name), function) == 0;
      }
      if (!match) continue;
      if (original) *original = reinterpret_cast<void*>(iat->u1.Function);
      uintptr_t value = reinterpret_cast<uintptr_t>(replacement);
      return WriteMemory(reinterpret_cast<uintptr_t>(&iat->u1.Function), &value, sizeof(value));
    }
  }
  return false;
}

}  // namespace scpatch::patch
