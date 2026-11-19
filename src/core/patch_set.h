#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A group of code/data patches that can be switched on and off at runtime as a unit, e.g. constants that must
// hold one value in the menus and another during a match. Every site's original bytes are verified once
// before anything is written; if any site does not match, the whole set refuses to apply.
namespace scpatch::patch {

class PatchSet {
 public:
  explicit PatchSet(const char* name) : name_(name) {}

  void Add(uintptr_t address, std::vector<uint8_t> original, std::vector<uint8_t> replacement, const char* what);

  // Helpers for the most common shapes: a 32-bit or 16-bit immediate/displacement at `address`.
  void AddDword(uintptr_t address, uint32_t original, uint32_t replacement, const char* what);
  void AddWord(uintptr_t address, uint16_t original, uint16_t replacement, const char* what);

  // Checks every site against its original bytes. Logs each mismatch.
  bool Verify();
  // Writes the replacement (enabled) or original (disabled) bytes. No-op unless Verify() succeeded.
  void Enable(bool enabled);
  bool Enabled() const { return enabled_; }
  size_t Size() const { return sites_.size(); }

 private:
  struct Site {
    uintptr_t address;
    std::vector<uint8_t> original;
    std::vector<uint8_t> replacement;
    std::string what;
  };
  std::string name_;
  std::vector<Site> sites_;
  bool verified_ = false;
  bool enabled_ = false;
};

}  // namespace scpatch::patch
