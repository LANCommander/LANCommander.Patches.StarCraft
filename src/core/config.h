#pragma once

#include <string>

// Local (per-machine) options from scpatch.ini in the game folder. Options that change gameplay must not
// live here; they are game rules chosen by the lobby host so every client agrees.
namespace scpatch::config {

void Load(const std::wstring& path);
bool GetBool(const wchar_t* section, const wchar_t* key, bool fallback);
int GetInt(const wchar_t* section, const wchar_t* key, int fallback);
std::wstring GetString(const wchar_t* section, const wchar_t* key, const wchar_t* fallback);

}  // namespace scpatch::config
