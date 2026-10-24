// Files that live next to settings.ini (%APPDATA%\ClaudeUsageMonitor by default).
#pragma once
#include <string>

std::wstring AppDataFile(const wchar_t* name);

// Appends "<local time>  <line>" to app.log (kept under 64 KB). Thread-safe enough
// for our use: every write opens, appends and closes the file.
void AppendAppLog(const std::wstring& line);
