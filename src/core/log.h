#pragma once

#include <string>

namespace scpatch::log {

void Open(const std::wstring& path);
void Info(const char* format, ...);
void Warn(const char* format, ...);
void Error(const char* format, ...);

}  // namespace scpatch::log
