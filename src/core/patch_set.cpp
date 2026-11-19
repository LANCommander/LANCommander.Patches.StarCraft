#include "core/patch_set.h"

#include <cstring>

#include "core/log.h"
#include "core/patch.h"

namespace scpatch::patch {

void PatchSet::Add(uintptr_t address, std::vector<uint8_t> original, std::vector<uint8_t> replacement,
                   const char* what) {
  sites_.push_back(Site{address, std::move(original), std::move(replacement), what});
}

void PatchSet::AddDword(uintptr_t address, uint32_t original, uint32_t replacement, const char* what) {
  std::vector<uint8_t> o(4), r(4);
  std::memcpy(o.data(), &original, 4);
  std::memcpy(r.data(), &replacement, 4);
  Add(address, std::move(o), std::move(r), what);
}

void PatchSet::AddWord(uintptr_t address, uint16_t original, uint16_t replacement, const char* what) {
  std::vector<uint8_t> o(2), r(2);
  std::memcpy(o.data(), &original, 2);
  std::memcpy(r.data(), &replacement, 2);
  Add(address, std::move(o), std::move(r), what);
}

bool PatchSet::Verify() {
  bool ok = true;
  for (const Site& s : sites_) {
    if (s.original.size() != s.replacement.size()) {
      log::Error("%s: '%s' has mismatched lengths", name_.c_str(), s.what.c_str());
      ok = false;
    } else if (std::memcmp(reinterpret_cast<const void*>(s.address), s.original.data(), s.original.size()) != 0) {
      log::Error("%s: '%s' at %08X does not match the expected original bytes", name_.c_str(), s.what.c_str(),
                 static_cast<unsigned>(s.address));
      ok = false;
    }
  }
  verified_ = ok;
  log::Info("%s: %zu site(s) %s", name_.c_str(), sites_.size(), ok ? "verified" : "FAILED verification");
  return ok;
}

void PatchSet::Enable(bool enabled) {
  if (!verified_ || enabled == enabled_) return;
  for (const Site& s : sites_) {
    const std::vector<uint8_t>& bytes = enabled ? s.replacement : s.original;
    WriteMemory(s.address, bytes.data(), bytes.size());
  }
  enabled_ = enabled;
}

}  // namespace scpatch::patch
