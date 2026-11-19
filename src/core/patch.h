#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>

// Code patching helpers. Anything that modifies game code verifies the original bytes first and refuses to
// patch on mismatch, so a wrong address or an unexpected game build fails loudly instead of corrupting code.
namespace scpatch::patch {

bool WriteMemory(uintptr_t address, const void* data, size_t size);

// Replaces `expected` at `address` with `replacement` (same length).
bool Bytes(uintptr_t address, std::initializer_list<uint8_t> expected, std::initializer_list<uint8_t> replacement,
           const char* what);

// Writes an unconditional `jmp rel32` at `address` (no verification; for hooks we construct ourselves).
bool WriteJump(uintptr_t address, void* target);

// Retargets an existing `call rel32` (E8) at `address`, verifying the current target. Returns false on mismatch.
bool RedirectCall(uintptr_t address, uintptr_t expectedTarget, void* newTarget, const char* what);

// Replaces an import address table entry of `module` for `dll`!`function`. Stores the previous pointer in
// `original` (if non-null). Imports by ordinal are matched when `function` is null and `ordinal` is set.
bool HookImport(HMODULE module, const char* dll, const char* function, void* replacement, void** original,
                WORD ordinal = 0);

}  // namespace scpatch::patch
