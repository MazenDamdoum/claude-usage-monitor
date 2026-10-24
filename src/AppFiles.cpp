#include "AppFiles.h"

#include <windows.h>

#include <cstdio>

#include "Settings.h"
#include "UsageClient.h"

std::wstring AppDataFile(const wchar_t* name) {
    std::wstring path = Settings::Path();
    return path.substr(0, path.find_last_of(L"\\/") + 1) + name;
}

void AppendAppLog(const std::wstring& line) {
    std::wstring path = AppDataFile(L"app.log");
    WIN32_FILE_ATTRIBUTE_DATA info;
    const DWORD limit = claude_usage::TestApiPort() ? 8u << 20 : 64u * 1024;  // tests keep the whole run
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && (info.nFileSizeHigh || info.nFileSizeLow > limit))
        DeleteFileW(path.c_str());

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04d-%02d-%02d %02d:%02d:%02d  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    std::string utf8 = claude_usage::WideToUtf8(stamp + line) + "\r\n";
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
}
