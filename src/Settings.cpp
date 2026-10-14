#include "Settings.h"

#include <cwchar>

const wchar_t* const kItemNames[kItemCount] = {L"Session (5h)", L"Weekly (7d)", L"Weekly Sonnet", L"Weekly Opus"};

namespace {

const wchar_t kGeneral[] = L"general";
const wchar_t kTaskbar[] = L"taskbar";
const wchar_t kMain[] = L"main_window";
const wchar_t kLegacy[] = L"settings";  // v1.0 single-section file

const wchar_t* const kItemKeys[kItemCount] = {L"session", L"weekly", L"sonnet", L"opus"};

std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L"\\/") + 1);
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

class Ini {
public:
    explicit Ini(std::wstring path) : path_(std::move(path)) {}

    bool Has(const wchar_t* section, const wchar_t* key) const {
        wchar_t buf[8] = {};
        GetPrivateProfileStringW(section, key, L"\x1", buf, 8, path_.c_str());
        return buf[0] != L'\x1';
    }
    int Int(const wchar_t* section, const wchar_t* key, int def) const {
        return static_cast<int>(GetPrivateProfileIntW(section, key, def, path_.c_str()));
    }
    bool Bool(const wchar_t* section, const wchar_t* key, bool def) const { return Int(section, key, def) != 0; }
    std::wstring Str(const wchar_t* section, const wchar_t* key, const std::wstring& def) const {
        if (!Has(section, key)) return def;
        wchar_t buf[1024] = {};
        GetPrivateProfileStringW(section, key, def.c_str(), buf, 1024, path_.c_str());
        return buf;
    }
    COLORREF Color(const wchar_t* section, const wchar_t* key, COLORREF def) const {
        std::wstring s = Str(section, key, L"");
        unsigned r, g, b;
        if (s.size() == 7 && s[0] == L'#' && swscanf(s.c_str() + 1, L"%2x%2x%2x", &r, &g, &b) == 3) return RGB(r, g, b);
        return def;
    }

    void Put(const wchar_t* section, const wchar_t* key, const std::wstring& value) const {
        WritePrivateProfileStringW(section, key, value.c_str(), path_.c_str());
    }
    void Put(const wchar_t* section, const wchar_t* key, int value) const { Put(section, key, std::to_wstring(value)); }
    void PutBool(const wchar_t* section, const wchar_t* key, bool value) const { Put(section, key, value ? 1 : 0); }
    void PutColor(const wchar_t* section, const wchar_t* key, COLORREF c) const {
        wchar_t buf[8];
        swprintf(buf, 8, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
        Put(section, key, std::wstring(buf));
    }
    void DeleteSection(const wchar_t* section) const {
        WritePrivateProfileStringW(section, nullptr, nullptr, path_.c_str());
    }

private:
    std::wstring path_;
};

// UTF-16 with BOM so the Private Profile APIs keep non-ASCII labels intact.
void CreateEmptyIni(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const wchar_t bom = 0xFEFF;
    WriteFile(h, &bom, sizeof(bom), &written, nullptr);
    CloseHandle(h);
}

void ReadLook(const Ini& ini, const wchar_t* sec, WindowLook& look) {
    look.font_name = ini.Str(sec, L"font_name", look.font_name);
    if (look.font_name.empty()) look.font_name = L"Segoe UI";
    look.font_size = Clamp(ini.Int(sec, L"font_size", look.font_size), 6, 28);
    look.font_bold = ini.Bool(sec, L"font_bold", look.font_bold);
    look.label_color = ini.Color(sec, L"label_color", look.label_color);
    look.value_color = ini.Color(sec, L"value_color", look.value_color);
    look.bar_color = ini.Color(sec, L"bar_color", look.bar_color);
    look.background_color = ini.Color(sec, L"background_color", look.background_color);
    look.show_reset = ini.Bool(sec, L"show_reset", look.show_reset);
    look.show_bars = ini.Bool(sec, L"show_bars", look.show_bars);
    look.percent_space = ini.Bool(sec, L"percent_space", look.percent_space);
    look.hide_percent = ini.Bool(sec, L"hide_percent", look.hide_percent);
    look.tooltip = ini.Bool(sec, L"tooltip", look.tooltip);
    for (int i = 0; i < kItemCount; ++i) {
        look.items[i].show = ini.Bool(sec, (std::wstring(L"show_") + kItemKeys[i]).c_str(), look.items[i].show);
        look.items[i].label = ini.Str(sec, (std::wstring(L"label_") + kItemKeys[i]).c_str(), look.items[i].label);
    }
}

void WriteLook(const Ini& ini, const wchar_t* sec, const WindowLook& look) {
    ini.Put(sec, L"font_name", look.font_name);
    ini.Put(sec, L"font_size", look.font_size);
    ini.PutBool(sec, L"font_bold", look.font_bold);
    ini.PutColor(sec, L"label_color", look.label_color);
    ini.PutColor(sec, L"value_color", look.value_color);
    ini.PutColor(sec, L"bar_color", look.bar_color);
    ini.PutColor(sec, L"background_color", look.background_color);
    ini.PutBool(sec, L"show_reset", look.show_reset);
    ini.PutBool(sec, L"show_bars", look.show_bars);
    ini.PutBool(sec, L"percent_space", look.percent_space);
    ini.PutBool(sec, L"hide_percent", look.hide_percent);
    ini.PutBool(sec, L"tooltip", look.tooltip);
    for (int i = 0; i < kItemCount; ++i) {
        ini.PutBool(sec, (std::wstring(L"show_") + kItemKeys[i]).c_str(), look.items[i].show);
        ini.Put(sec, (std::wstring(L"label_") + kItemKeys[i]).c_str(), look.items[i].label);
    }
}

}  // namespace

WindowLook Settings::DefaultTaskbarLook() {
    WindowLook look;
    look.items[kItemSession] = {true, L"5h:"};
    look.items[kItemWeekly] = {true, L"7d:"};
    look.items[kItemSonnet] = {false, L"Son:"};
    look.items[kItemOpus] = {false, L"Opus:"};
    return look;
}

WindowLook Settings::DefaultMainLook() {
    WindowLook look;
    look.font_size = 10;
    look.label_color = RGB(190, 190, 190);
    look.value_color = RGB(255, 255, 255);
    look.background_color = RGB(24, 24, 24);
    look.show_bars = true;
    for (int i = 0; i < kItemCount; ++i) look.items[i] = {i <= kItemWeekly, kItemNames[i]};
    return look;
}

Settings Settings::Defaults() {
    Settings s;
    s.taskbar = DefaultTaskbarLook();
    s.main = DefaultMainLook();
    return s;
}

std::wstring Settings::Path() {
    std::wstring portable = ExeDir() + L"settings.ini";
    if (GetFileAttributesW(portable.c_str()) != INVALID_FILE_ATTRIBUTES) return portable;

    wchar_t appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return portable;
    std::wstring dir = std::wstring(appdata) + L"\\ClaudeUsageMonitor";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

Settings Settings::Load() {
    Settings s = Defaults();
    std::wstring path = Path();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateEmptyIni(path);
        s.Save();
        return s;
    }
    Ini ini(path);

    // v1.0 files kept everything in [settings]; use those values as defaults.
    s.poll_seconds = ini.Int(kLegacy, L"poll_seconds", s.poll_seconds);
    s.credentials_path = ini.Str(kLegacy, L"credentials_path", s.credentials_path);
    s.show_tray_icon = ini.Bool(kLegacy, L"show_tray_icon", s.show_tray_icon);
    s.warn_percent = ini.Int(kLegacy, L"warn_percent", s.warn_percent);
    s.critical_percent = ini.Int(kLegacy, L"critical_percent", s.critical_percent);
    s.taskbar.show_reset = ini.Bool(kLegacy, L"show_reset", s.taskbar.show_reset);
    s.taskbar.show_bars = ini.Bool(kLegacy, L"show_bars", s.taskbar.show_bars);
    s.taskbar.percent_space = ini.Bool(kLegacy, L"percent_space", s.taskbar.percent_space);
    s.taskbar.font_name = ini.Str(kLegacy, L"font_name", s.taskbar.font_name);
    s.taskbar.font_size = ini.Int(kLegacy, L"font_size", s.taskbar.font_size);
    s.taskbar_offset_x = ini.Int(kLegacy, L"offset_x", s.taskbar_offset_x);
    s.taskbar.items[kItemSession].label = ini.Str(kLegacy, L"label_session", s.taskbar.items[kItemSession].label);
    s.taskbar.items[kItemWeekly].label = ini.Str(kLegacy, L"label_weekly", s.taskbar.items[kItemWeekly].label);

    s.poll_seconds = Clamp(ini.Int(kGeneral, L"poll_seconds", s.poll_seconds), 60, 3600);
    s.credentials_path = ini.Str(kGeneral, L"credentials_path", s.credentials_path);
    s.warn_percent = Clamp(ini.Int(kGeneral, L"warn_percent", s.warn_percent), 1, 100);
    s.critical_percent = Clamp(ini.Int(kGeneral, L"critical_percent", s.critical_percent), 1, 100);
    s.notify = ini.Bool(kGeneral, L"notify", s.notify);
    s.show_tray_icon = ini.Bool(kGeneral, L"show_tray_icon", s.show_tray_icon);

    s.taskbar_visible = ini.Bool(kTaskbar, L"visible", s.taskbar_visible);
    ReadLook(ini, kTaskbar, s.taskbar);
    s.taskbar_transparent = ini.Bool(kTaskbar, L"transparent", s.taskbar_transparent);
    s.taskbar_auto_colors = ini.Bool(kTaskbar, L"auto_colors", s.taskbar_auto_colors);
    s.taskbar_right_align = ini.Bool(kTaskbar, L"right_align", s.taskbar_right_align);
    s.taskbar_horizontal = ini.Bool(kTaskbar, L"horizontal", s.taskbar_horizontal);
    s.taskbar_offset_x = Clamp(ini.Int(kTaskbar, L"offset_x", s.taskbar_offset_x), -4000, 4000);

    s.main_visible = ini.Bool(kMain, L"visible", s.main_visible);
    ReadLook(ini, kMain, s.main);
    s.main_opacity = Clamp(ini.Int(kMain, L"opacity", s.main_opacity), 20, 100);
    s.main_topmost = ini.Bool(kMain, L"topmost", s.main_topmost);
    s.main_click_through = ini.Bool(kMain, L"click_through", s.main_click_through);
    s.main_locked = ini.Bool(kMain, L"locked", s.main_locked);
    s.main_offscreen = ini.Bool(kMain, L"offscreen", s.main_offscreen);
    s.main_has_position = ini.Has(kMain, L"x") && ini.Has(kMain, L"y");
    s.main_x = ini.Int(kMain, L"x", 0);
    s.main_y = ini.Int(kMain, L"y", 0);
    return s;
}

void Settings::Save() const {
    std::wstring path = Path();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) CreateEmptyIni(path);
    Ini ini(path);
    ini.DeleteSection(kLegacy);

    ini.Put(kGeneral, L"poll_seconds", poll_seconds);
    ini.Put(kGeneral, L"credentials_path", credentials_path);
    ini.Put(kGeneral, L"warn_percent", warn_percent);
    ini.Put(kGeneral, L"critical_percent", critical_percent);
    ini.PutBool(kGeneral, L"notify", notify);
    ini.PutBool(kGeneral, L"show_tray_icon", show_tray_icon);

    ini.PutBool(kTaskbar, L"visible", taskbar_visible);
    WriteLook(ini, kTaskbar, taskbar);
    ini.PutBool(kTaskbar, L"transparent", taskbar_transparent);
    ini.PutBool(kTaskbar, L"auto_colors", taskbar_auto_colors);
    ini.PutBool(kTaskbar, L"right_align", taskbar_right_align);
    ini.PutBool(kTaskbar, L"horizontal", taskbar_horizontal);
    ini.Put(kTaskbar, L"offset_x", taskbar_offset_x);

    ini.PutBool(kMain, L"visible", main_visible);
    WriteLook(ini, kMain, main);
    ini.Put(kMain, L"opacity", main_opacity);
    ini.PutBool(kMain, L"topmost", main_topmost);
    ini.PutBool(kMain, L"click_through", main_click_through);
    ini.PutBool(kMain, L"locked", main_locked);
    ini.PutBool(kMain, L"offscreen", main_offscreen);
    if (main_has_position) {
        ini.Put(kMain, L"x", main_x);
        ini.Put(kMain, L"y", main_y);
    }
}
