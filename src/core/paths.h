#pragma once

#include <string>

namespace scpatch::paths {

void Initialize();

// Folder containing StarCraft.exe, without a trailing backslash.
const std::wstring& GameDir();
// Where per-user data lives: %USERPROFILE%\Saved Games\Blizzard Entertainment\StarCraft by default
// (resolved through the Saved Games known folder, so a relocated Saved Games folder is honoured).
const std::wstring& UserDataRoot();

std::wstring ConfigFile();
std::wstring LogFile();

// Creates `path` and any missing parents.
bool EnsureDirectory(const std::wstring& path);

}  // namespace scpatch::paths
