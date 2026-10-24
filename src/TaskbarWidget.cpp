#include "TaskbarWidget.h"

#include <commctrl.h>
#include <uiautomation.h>

#include <algorithm>
#include <cmath>
#include <climits>
#include <ctime>

#include "Canvas.h"

namespace {

const wchar_t kWidgetClass[] = L"ClaudeUsageMonitor.TaskbarWidget";

const COLORREF kWarnDark = RGB(255, 179, 64), kCriticalDark = RGB(255, 107, 107);
const COLORREF kWarnLight = RGB(184, 110, 0), kCriticalLight = RGB(208, 32, 32);

bool IsLightTaskbar() {
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
           value == 1;
}

bool IsLightColor(COLORREF c) {
    return GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114 > 128000;
}

struct EnumCtx {
    HWND taskbar;
    HWND self;
    DWORD explorer_pid;
    RECT rects[16];
    int count;
};

// Collects other apps' windows living in the taskbar (e.g. TrafficMonitor).
BOOL CALLBACK CollectForeign(HWND h, LPARAM lp) {
    EnumCtx* ctx = reinterpret_cast<EnumCtx*>(lp);
    if (h == ctx->self || GetParent(h) != ctx->taskbar || !IsWindowVisible(h)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == ctx->explorer_pid || pid == GetCurrentProcessId()) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r) || r.right <= r.left) return TRUE;
    MapWindowPoints(HWND_DESKTOP, ctx->taskbar, reinterpret_cast<POINT*>(&r), 2);
    if (ctx->count < 16) ctx->rects[ctx->count++] = r;
    return TRUE;
}

// The mouse's own buttons as the hardware reports them (raw input): some mice and their software
// hand a right-click to the taskbar as a left one, so the click messages can't be trusted for which.
ULONGLONG g_raw_left_at = 0, g_raw_right_at = 0;

// Where each taskbar's own app buttons end (screen x), read through UI Automation, which sees
// Windows 11's buttons whether they sit at the left or in the middle and however many there are.
// A background thread reads every taskbar asked about every 1.5 s, so the UI never waits on it.
const CLSID kClsidCUIAutomation = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
const IID kIidIUIAutomation = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
SRWLOCK g_buttons_lock = SRWLOCK_INIT;
std::vector<std::pair<HWND, int>> g_buttons_end;  // taskbar -> screen x where its buttons end (-1 unknown)

int ReadButtonsEnd(IUIAutomation* uia, HWND taskbar) {
    IUIAutomationElement* root = nullptr;
    if (FAILED(uia->ElementFromHandle(taskbar, &root)) || !root) return -1;
    int end = -1;
    for (const wchar_t* cls : {L"Taskbar.TaskListButtonAutomationPeer", L"ToggleButton"}) {
        VARIANT name;
        VariantInit(&name);
        name.vt = VT_BSTR;
        name.bstrVal = SysAllocString(cls);
        IUIAutomationCondition* cond = nullptr;
        IUIAutomationElementArray* found = nullptr;
        if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ClassNamePropertyId, name, &cond)) && cond &&
            SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &found)) && found) {
            int count = 0;
            found->get_Length(&count);
            for (int i = 0; i < count; ++i) {
                IUIAutomationElement* e = nullptr;
                RECT r = {};
                if (SUCCEEDED(found->GetElement(i, &e)) && e && SUCCEEDED(e->get_CurrentBoundingRectangle(&r)) &&
                    r.right > r.left)
                    end = std::max(end, static_cast<int>(r.right));
                if (e) e->Release();
            }
        }
        if (found) found->Release();
        if (cond) cond->Release();
        VariantClear(&name);
    }
    root->Release();
    return end;
}

DWORD WINAPI ButtonsThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    if (FAILED(CoCreateInstance(kClsidCUIAutomation, nullptr, CLSCTX_INPROC_SERVER, kIidIUIAutomation,
                                reinterpret_cast<void**>(&uia))) || !uia)
        return 0;  // no UI Automation: the estimate from the Start button stays
    for (;;) {
        AcquireSRWLockShared(&g_buttons_lock);
        std::vector<HWND> bars;
        for (const auto& b : g_buttons_end) bars.push_back(b.first);
        ReleaseSRWLockShared(&g_buttons_lock);
        for (HWND bar : bars) {
            const int end = IsWindow(bar) ? ReadButtonsEnd(uia, bar) : -1;
            AcquireSRWLockExclusive(&g_buttons_lock);
            for (auto& b : g_buttons_end) if (b.first == bar) b.second = end;
            ReleaseSRWLockExclusive(&g_buttons_lock);
        }
        Sleep(1500);
    }
}

// The last reading for this taskbar (and from now on it's read); -1 until there is one.
int ButtonsEnd(HWND taskbar) {
    static bool started = false;
    if (!started) {
        started = true;
        HANDLE t = CreateThread(nullptr, 0, &ButtonsThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    AcquireSRWLockExclusive(&g_buttons_lock);
    int end = -2;
    for (const auto& b : g_buttons_end) if (b.first == taskbar) end = b.second;
    if (end == -2) {
        g_buttons_end.emplace_back(taskbar, -1);
        end = -1;
    }
    ReleaseSRWLockExclusive(&g_buttons_lock);
    return end;
}

// The other monitors' taskbars, left to right.
BOOL CALLBACK CollectSecondary(HWND h, LPARAM lp) {
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
    return TRUE;
}

std::vector<HWND> SecondaryTaskbars() {
    std::vector<HWND> list;
    EnumWindows(&CollectSecondary, reinterpret_cast<LPARAM>(&list));
    std::sort(list.begin(), list.end(), [](HWND a, HWND b) {
        RECT ra = {}, rb = {};
        GetWindowRect(a, &ra);
        GetWindowRect(b, &rb);
        return ra.left < rb.left;
    });
    return list;
}

HWND FindTaskbar(int index) {
    if (index <= 0) return FindWindowW(L"Shell_TrayWnd", nullptr);
    std::vector<HWND> others = SecondaryTaskbars();
    return index - 1 < static_cast<int>(others.size()) ? others[index - 1] : nullptr;
}

}  // namespace

int TaskbarWidget::TaskbarCount() {
    return (FindWindowW(L"Shell_TrayWnd", nullptr) ? 1 : 0) + static_cast<int>(SecondaryTaskbars().size());
}

void TaskbarWidget::Init(HINSTANCE instance, HWND owner) {
    instance_ = instance;
    owner_ = owner;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &TaskbarWidget::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWidgetClass;
    RegisterClassExW(&wc);
}

void TaskbarWidget::Destroy() {
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);  // also destroys the owned tooltip
    hwnd_ = nullptr;
    tooltip_ = nullptr;
    render_key_.clear();
    placed_ = RECT{};
    anim_ms_ = 0;  // its timer went with the window
    check_spinning_ = false;  // so did the ring's
}

bool TaskbarWidget::Attach() {
    Destroy();
    taskbar_ = FindTaskbar(target_);
    if (!taskbar_) return false;
    shown_tip_.clear();
    hover_col_ = -1;
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kWidgetClass, L"Claude Usage",
                            WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, taskbar_, nullptr, instance_, this);
    if (!hwnd_) return false;
    if (target_ == 0) {  // the main readout listens to the mouse hardware for every one of them
        RAWINPUTDEVICE rid = {0x01, 0x02, RIDEV_INPUTSINK, hwnd_};
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
    }

    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, instance_,
                               nullptr);
    if (tooltip_) {
        TOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hwnd_;
        ti.uId = reinterpret_cast<UINT_PTR>(hwnd_);
        ti.lpszText = const_cast<wchar_t*>(tooltip_text_.c_str());
        SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 600);
        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
        // Only after the pointer has rested a good while: it's often parked over the taskbar.
        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_INITIAL, 3000);
        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_RESHOW, 3000);
        SendMessageW(tooltip_, TTM_ACTIVATE, tooltip_enabled_, 0);
        tips_active_ = tooltip_enabled_;
    }
    return true;
}

void TaskbarWidget::SetTooltip(const std::wstring& text, bool enabled) {
    if (tooltip_ && enabled != tooltip_enabled_ && !folding_) {
        SendMessageW(tooltip_, TTM_ACTIVATE, enabled, 0);
        tips_active_ = enabled;
    }
    tooltip_enabled_ = enabled;
    tooltip_text_ = text;
    ShowTooltipText();
}

void TaskbarWidget::SetColumns(const std::vector<std::wstring>& tips, const std::vector<bool>& clickable) {
    col_tips_ = tips;
    col_clickable_ = clickable;
    ShowTooltipText();
}

int TaskbarWidget::ColumnAt(int x) const {
    for (size_t i = 0; i < col_spans_.size(); ++i)
        if (x >= col_spans_[i].first && x < col_spans_[i].second) return static_cast<int>(i);
    return -1;
}

// The hovered column's own short tip in the multi-account view, else the readout's tooltip.
void TaskbarWidget::ShowTooltipText() {
    const std::wstring& text = (hover_col_ >= 0 && hover_col_ < static_cast<int>(col_tips_.size()))
                                   ? col_tips_[hover_col_]
                                   : tooltip_text_;
    if (text == shown_tip_ || !tooltip_ || !hwnd_) return;
    shown_tip_ = text;
    TOOLINFOW ti = {};
    ti.cbSize = sizeof(ti);
    ti.hwnd = hwnd_;
    ti.uId = reinterpret_cast<UINT_PTR>(hwnd_);
    ti.lpszText = const_cast<wchar_t*>(shown_tip_.c_str());
    SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
}
// Where the taskbar's own buttons end. Centred (Windows 11): as far right of the middle as the Start
// button is left of it. 0 when they sit at the left (their end can't be measured then).
int TaskbarWidget::IconsRight(int dpi) const {
    const int end = ButtonsEnd(taskbar_);  // the buttons themselves, when UI Automation has read them
    if (end > 0) {
        POINT p = {end, 0};
        ScreenToClient(taskbar_, &p);
        return p.x + MulDiv(8, dpi, 96);
    }
    HWND start = FindWindowExW(taskbar_, nullptr, L"Start", nullptr);
    RECT client = {}, r = {};
    GetClientRect(taskbar_, &client);
    if (!start || !GetWindowRect(start, &r)) return 0;
    MapWindowPoints(HWND_DESKTOP, taskbar_, reinterpret_cast<POINT*>(&r), 2);
    const int mid = client.right / 2;
    return r.left > client.right / 8 && r.left < mid ? 2 * mid - r.left + MulDiv(8, dpi, 96) : 0;
}

int TaskbarWidget::RightEdge(int dpi) const {
    RECT client;
    GetClientRect(taskbar_, &client);
    // Fallback when the tray can't be found: on another monitor's taskbar (which has no tray), just
    // clear of its clock; on the main one, about the tray's width.
    const bool main_bar = FindWindowExW(taskbar_, nullptr, L"TrayNotifyWnd", nullptr) != nullptr;
    int right = client.right - MulDiv(main_bar ? 280 : 92, dpi, 96);

    HWND notify = FindWindowExW(taskbar_, nullptr, L"TrayNotifyWnd", nullptr);
    RECT rn;
    if (notify && GetWindowRect(notify, &rn) && rn.right > rn.left) {
        MapWindowPoints(HWND_DESKTOP, taskbar_, reinterpret_cast<POINT*>(&rn), 2);
        if (rn.left > 0) right = rn.left;
    }

    // Step left past other apps' taskbar widgets that sit against the tray.
    EnumCtx ctx = {};
    ctx.taskbar = taskbar_;
    ctx.self = hwnd_;
    GetWindowThreadProcessId(taskbar_, &ctx.explorer_pid);
    EnumChildWindows(taskbar_, &CollectForeign, reinterpret_cast<LPARAM>(&ctx));
    int slack = MulDiv(6, dpi, 96);
    for (bool moved = true; moved;) {
        moved = false;
        for (int i = 0; i < ctx.count; ++i) {
            const RECT& r = ctx.rects[i];
            if (r.left < right && r.right >= right - slack) {
                right = r.left;
                moved = true;
            }
        }
    }
    return right - MulDiv(2, dpi, 96);
}

namespace {
const UINT_PTR kFlashTimer = 7;
const UINT_PTR kFoldTimer = 9;
const UINT_PTR kCheckTimer = 10;  // the ring turning while a usage check is due
const double kFoldMs = 480.0;  // collapsing to the blossom, or opening back out

double EaseInOut(double t) { return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2; }
const double kFlashMs = 1400.0;

// Claude Code's "working" spinner: these glyphs forward, then back again, in Claude orange.
const UINT_PTR kAnimTimer = 8;
const UINT kAnimFastMs = 15, kAnimIdleMs = 100;  // ~60 Hz while tokens flow (Windows' timer limit), 10 Hz idle
const wchar_t* const kSpinFrames[] = {L"·", L"✢", L"✳", L"✶", L"✻", L"✽",
                                      L"✽", L"✻", L"✶", L"✳", L"✢", L"·"};
const int kSpinCount = sizeof(kSpinFrames) / sizeof(kSpinFrames[0]);
const int kStillFrame = 4;  // the blossom, standing still
const COLORREF kClaudeOrange = RGB(217, 119, 87);

// Where `ch`'s ink sits in a line of `font`: its middle, down from the line's top and across from
// where it is drawn, and its width. Fonts box their glyphs differently, so lining up ink, not boxes,
// is what makes the spinner sit centred on the text and evenly between its neighbours.
void InkOf(HFONT font, wchar_t ch, int& mid_y, int& mid_x, int& width, int* ink_top = nullptr) {
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    GLYPHMETRICS gm = {};
    const MAT2 identity = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    mid_y = tm.tmHeight / 2;
    mid_x = width = TextWidth(font, std::wstring(1, ch));
    mid_x /= 2;
    if (GetGlyphOutlineW(dc, ch, GGO_METRICS, &gm, 0, nullptr, &identity) != GDI_ERROR) {
        mid_y = tm.tmAscent - gm.gmptGlyphOrigin.y + static_cast<int>(gm.gmBlackBoxY) / 2;
        mid_x = gm.gmptGlyphOrigin.x + static_cast<int>(gm.gmBlackBoxX) / 2;
        width = static_cast<int>(gm.gmBlackBoxX);
        if (ink_top) *ink_top = tm.tmAscent - gm.gmptGlyphOrigin.y;
    }
    SelectObject(dc, old);
    DeleteDC(dc);
}

// Milliseconds per frame: about Claude Code's own pace at a normal ~100 tok/s, quicker as the
// speed climbs (on a log scale, fastest from 5,000 tok/s), still below 1 tok/s.

UINT SpinInterval(double rate) {
    if (rate < 1) return 0;
    double t = std::min(1.0, std::log10(rate) / std::log10(5000.0));
    return static_cast<UINT>(230 - 190 * t);
}
}  // namespace

void TaskbarWidget::Flash(int column, COLORREF color) {
    if (!hwnd_) return;
    flash_col_ = column;
    flash_color_ = color;
    flash_start_ = GetTickCount64();
    SetTimer(hwnd_, kFlashTimer, 16, nullptr);
}

void TaskbarWidget::SetTokenSource(std::function<TokenFlow()> source) {
    token_source_ = std::move(source);
}

void TaskbarWidget::SetCheckSource(std::function<CheckTimes()> source) {
    check_source_ = std::move(source);
}

void TaskbarWidget::Animate() {
    ULONGLONG now = GetTickCount64();
    double dt = std::min(0.25, (now - anim_last_) / 1000.0);
    anim_last_ = now;
    TokenFlow flow = token_source_ ? token_source_() : TokenFlow();
    double target = std::max(0.0, flow.rate);
    shown_rate_ += (target - shown_rate_) * (1 - std::exp(-dt / 0.15));
    if (target < 0.5 && shown_rate_ < 0.5) shown_rate_ = 0;
    // The pile: a new stretch starts from 0; otherwise climb toward it, never back down (a reply
    // that lands short of its estimate just holds the number until the real count passes it).
    if (flow.pile != shown_pile_) {
        if (shown_pile_ != -1 || flow.working) shown_count_ = 0;
        shown_pile_ = flow.pile;
    }
    const bool climbing = flow.working && flow.count > shown_count_ + 0.5;
    if (climbing) shown_count_ += (flow.count - shown_count_) * (1 - std::exp(-dt / 0.35));
    UINT interval = SpinInterval(shown_rate_);
    if (interval) spin_phase_ = std::fmod(spin_phase_ + dt * 1000.0 / interval, static_cast<double>(kSpinCount));

    UINT want = (target >= 1 || shown_rate_ >= 1 || climbing) ? kAnimFastMs : kAnimIdleMs;
    if (want != anim_ms_) {
        SetTimer(hwnd_, kAnimTimer, want, nullptr);
        anim_ms_ = want;
    }
    // The speed: "117 tok/s", "4.3k tok/s". The pile: every token up to 99,999 (so it is seen
    // climbing), then 123k and 1.2M.
    wchar_t buf[64];
    token_count_up_ = flow.count_up;
    tokens_left_shown_ = flow.left >= 0;
    const long long n = static_cast<long long>(shown_count_);
    if (!flow.count_up) {
        // 0-999 tok/s, then 1.0k tok/s and on (tokens/s is an option).
        const wchar_t* unit = last_settings_.taskbar_long_unit ? L"tokens/s" : L"tok/s";
        if (shown_rate_ < 999.5) swprintf(buf, 32, L"%.0f %ls", shown_rate_, unit);
        else if (shown_rate_ < 99950) swprintf(buf, 32, L"%.1fk %ls", shown_rate_ / 1000, unit);
        else swprintf(buf, 32, L"%.0fk %ls", shown_rate_ / 1000, unit);
    } else if (n < 1000) {
        swprintf(buf, 32, L"%lld tokens", n);
    } else if (n < 100000) {
        swprintf(buf, 32, L"%lld,%03lld tokens", n / 1000, n % 1000);
    } else if (n < 1000000) {
        swprintf(buf, 32, L"%lldk tokens", n / 1000);
    } else {
        swprintf(buf, 32, L"%lld.%lldM tokens", n / 1000000, n / 100000 % 10);
    }
    const std::wstring speed_only = buf;  // without "· tok left" (a small screen drops that first)
    if (flow.left >= 0) {  // " · 842.3k tok left": the 5-hour window, going down as Claude works
        wchar_t left[32];
        if (flow.left < 1000) swprintf(left, 32, L" · %.0f tok left", flow.left);
        else if (flow.left < 1e6) swprintf(left, 32, L" · %.1fk tok left", flow.left / 1000);
        else swprintf(left, 32, L" · %.2fM tok left", flow.left / 1e6);
        wcsncat(buf, left, 63 - wcslen(buf));
    }
    int frame = interval ? static_cast<int>(spin_phase_) % kSpinCount : -1;
    // The tokens column follows the text on screen: a few pixels a tick toward its width (never a jump).
    bool widening = false;
    if (tokens_w_goal_ > 0) {
        if (shown_tokens_w_ <= 0) shown_tokens_w_ = tokens_w_goal_;
        const double gap = tokens_w_goal_ - shown_tokens_w_;
        if (std::fabs(gap) > 0.5) {
            shown_tokens_w_ += gap * (1 - std::exp(-dt / 0.18));
            if (std::fabs(tokens_w_goal_ - shown_tokens_w_) < 0.5) shown_tokens_w_ = tokens_w_goal_;
            widening = true;
        }
    }
    if (widening && anim_ms_ != kAnimFastMs) {  // smooth while the column is moving, even when idle
        SetTimer(hwnd_, kAnimTimer, kAnimFastMs, nullptr);
        anim_ms_ = kAnimFastMs;
    }
    if (buf != token_text_ || frame != token_frame_ || widening) {  // repaint only when something visible moved
        token_text_ = buf;
        token_speed_text_ = speed_only;
        token_frame_ = frame;
        if (have_last_) Update(last_items_, last_settings_);
    }
}

void TaskbarWidget::Update(const std::vector<DisplayItem>& items, const Settings& settings) {
    if (&items != &last_items_) {
        last_items_ = items;
        last_settings_ = settings;
        have_last_ = true;
    }
    if (!hwnd_ || !IsWindow(hwnd_) || !taskbar_ || !IsWindow(taskbar_) || GetParent(hwnd_) != taskbar_) {
        if (!Attach()) return;
    }
    if (items.empty()) {
        ShowWindow(hwnd_, SW_HIDE);
        return;
    }

    const WindowLook& look = settings.taskbar;
    int dpi = static_cast<int>(GetDpiForWindow(taskbar_));
    if (dpi <= 0) dpi = 96;
    auto scale = [dpi](int px) { return MulDiv(px, dpi, 96); };

    std::wstring font_key = look.font_name + L"|" + std::to_wstring(look.font_size) + (look.font_bold ? L"b" : L"") +
                            L"|" + std::to_wstring(dpi);
    if (!font_ || font_key != font_key_) {
        if (font_) DeleteObject(font_);
        if (small_font_) DeleteObject(small_font_);
        font_ = CreateUiFont(look.font_name, look.font_size, look.font_bold, dpi);
        small_font_ = CreateUiFont(look.font_name, std::max(6, look.font_size - 2), look.font_bold, dpi);
        // The spinner's frames, each pre-drawn with its ink dead centre, so cycling them in one
        // spot doesn't shake (the way a terminal's fixed cell keeps Claude Code's spinner steady).
        const int spin_px = MulDiv(static_cast<int>(std::lround(look.font_size * 1.25)), dpi, 72);
        spin_box_ = (spin_px + 5) & ~1;  // even, so its centre falls on a pixel edge
        spin_sprites_.clear();
        double ink_w = 0;
        for (const wchar_t* frame : kSpinFrames) {
            spin_sprites_.push_back(CenteredGlyph(L"Segoe UI Symbol", spin_px, frame[0], spin_box_));
            ink_w = std::max(ink_w, spin_sprites_.back().ink_w);
        }
        spin_w_ = static_cast<int>(std::ceil(ink_w));
        if ((spin_w_ - spin_box_) % 2) ++spin_w_;  // same parity as the box: the centres line up exactly
        int x = 0, w = 0;
        InkOf(font_, L'0', digit_ink_, x, w);
        InkOf(small_font_, L'0', small_digit_ink_, x, w);
        int y = 0;
        InkOf(font_, L'h', y, x, w, &letter_top_);
        font_key_ = font_key;
    }

    // The token speed animates itself while its column is shown.
    bool has_tokens = std::any_of(items.begin(), items.end(), [](const DisplayItem& it) { return it.span; });
    if (has_tokens && token_source_ && !anim_ms_) {
        anim_last_ = GetTickCount64();
        SetTimer(hwnd_, kAnimTimer, kAnimIdleMs, nullptr);
        anim_ms_ = kAnimIdleMs;
    } else if (!has_tokens && anim_ms_) {
        KillTimer(hwnd_, kAnimTimer);
        anim_ms_ = 0;
    }

    // No hover tips while folding or folded: the window moves under the pointer, and a tip shown
    // then would stay stuck on screen. Back on (if enabled) once it's fully open.
    const bool tips_on = tooltip_enabled_ && !folding_ && !settings.taskbar_collapsed;
    if (tooltip_ && tips_on != tips_active_) {
        if (!tips_on) SendMessageW(tooltip_, TTM_POP, 0, 0);
        SendMessageW(tooltip_, TTM_ACTIVATE, tips_on, 0);
        tips_active_ = tips_on;
    }
    blossom_hit_ = RECT{};
    // A click on the blossom flips the setting; the fold between the two then plays out over half a
    // second: the blossom rolls along to the right as everything folds in behind it, or rolls back
    // out to the left as it all opens up again.
    const double fold_target = settings.taskbar_collapsed ? 1.0 : 0.0;
    if (fold_target_ < 0) {
        fold_pos_ = fold_target_ = fold_target;  // starting up: no animation
    } else if (fold_target != fold_target_) {
        fold_from_ = fold_pos_;
        fold_target_ = fold_target;
        fold_start_ = GetTickCount64();
        folding_ = true;
        SetTimer(hwnd_, kFoldTimer, 15, nullptr);
    }
    // Colors.
    bool light_taskbar = IsLightTaskbar();
    COLORREF label = look.label_color, value = look.value_color;
    if (settings.taskbar_auto_colors) label = value = light_taskbar ? RGB(0, 0, 0) : RGB(255, 255, 255);
    bool light_bg = settings.taskbar_transparent ? light_taskbar : IsLightColor(look.background_color);
    COLORREF warn = light_bg ? kWarnLight : kWarnDark, critical = light_bg ? kCriticalLight : kCriticalDark;

    // Layout: TrafficMonitor-style columns of two rows, or a single row.
    const int n = static_cast<int>(items.size());
    const int rows = (settings.taskbar_horizontal || n == 1) ? 1 : 2;
    const int cols = (n + rows - 1) / rows;
    // One even gap between every group: the token speed, each account, and the tray.
    const int pad = scale(2), gap = scale(2), reset_gap = scale(3), col_gap = scale(6);  // compact, same detail
    const int line_h = LineHeight(font_);
    const int bar_h = look.show_bars ? scale(2) : 0, bar_gap = look.show_bars ? scale(1) : 0;
    const int row_gap = look.show_bars ? scale(1) : scale(2);
    const int row_h = line_h + bar_gap + bar_h;
    const std::wstring sample = look.hide_percent ? L"100" : (look.percent_space ? L"100 %" : L"100%");
    // The longest reset times that occur: 5-hour "4h 59m", weekly "6d 23h" (a weekly reset under a day
    // away, "23h 59m", widens its column for that last day).
    const int sample_reset_w = std::max(TextWidth(font_, L"4h 59m"), TextWidth(font_, L"6d 23h"));

    // The ring at the far right: the time to the next usage check inside it ("1:42"), the ring
    // emptying toward it; "now" and turning once it's due, until the check is done.
    std::wstring check_text;
    double check_left = 0;  // the share of the wait still to go: the ring's length
    bool check_due = false;
    const CheckTimes check = settings.taskbar_check_timer && check_source_ ? check_source_() : CheckTimes();
    if (check.next > 0) {
        const long long now_s = static_cast<long long>(time(nullptr));
        if (check.next != check_next_) {
            check_next_ = check.next;
            check_since_ = check.last > 0 && check.last <= now_s && now_s - check.last < 60 ? check.last : now_s;
        }
        const long long left_s = check.next - now_s;
        if (left_s <= 0) {
            check_due = true;
            check_text = L"now";
        } else {
            wchar_t buf[16];
            if (left_s < 600) swprintf(buf, 16, L"%lld:%02lld", left_s / 60, left_s % 60);
            else swprintf(buf, 16, L"%lldm", (left_s + 59) / 60);
            check_text = buf;
            check_left = std::min(1.0, static_cast<double>(left_s) / std::max<long long>(1, check.next - check_since_));
        }
    }
    const int ring_text_w = std::max(TextWidth(small_font_, L"0:00"), TextWidth(small_font_, L"now"));
    const int ring_d = check_text.empty() ? 0 : ring_text_w + scale(10);

    struct Column { int label_w = 0, value_w = 0, reset_w = 0, x = 0; };
    std::vector<Column> columns(cols);
    int width = pad;
    // Every column keeps a fixed width (wide enough for "100%" and the longest reset time), so nothing
    // on the taskbar shifts when numbers or the account in use change.
    const bool blossom = settings.taskbar_blossom;
    const int spin_gap = scale(4);  // the blossom to the number: tight, as the number never changes width
    // Room on this taskbar: from the end of its own buttons to the tray. On a small screen the readout
    // drops detail until it fits, least useful first: 1 "· tok left", 2 the reset times, 3 the speed's
    // text (the blossom stays), 4 everything but the blossom (folded). Coming back up needs a little
    // spare room, so it doesn't flick between two levels as digits change.
    // Full (the option off): everything, always, even where it runs over the taskbar's buttons.
    const int room = settings.taskbar_fit ? RightEdge(dpi) - IconsRight(dpi) : INT_MAX / 2;
    std::wstring token_shown = token_text_;
    auto measure = [&](int level) {
        token_shown = level >= 3 ? std::wstring() : level >= 1 ? token_speed_text_ : token_text_;
        // The tokens column is as wide as the text now shown (plus the spinner), rounded up to a small
        // step so it settles instead of twitching with every digit, and eased (Animate) so it changes
        // width subtly.
        const int step = scale(6);
        const int shown_text_w = (token_shown.empty() ? 0 : TextWidth(font_, token_shown.c_str()) + (blossom ? spin_gap : 0)) +
                                 (blossom ? spin_w_ : 0);
        const int goal = ((shown_text_w + step - 1) / step) * step;
        const int sample_tokens_w = level == fit_level_ && shown_tokens_w_ > 0 ? static_cast<int>(std::lround(shown_tokens_w_)) : goal;
        width = pad;
        for (int c = 0; c < cols; ++c) {
            Column& col = columns[c];
            col = Column();
            col.value_w = TextWidth(font_, sample);
            bool plain = false;
            for (int r = 0; r < rows && c * rows + r < n; ++r) {
                const DisplayItem& it = items[c * rows + r];
                plain = plain || it.plain;
                col.label_w = std::max(col.label_w, TextWidth(font_, it.label));
                col.value_w = std::max(col.value_w, TextWidth(font_, it.value));
                if (look.show_reset && level < 2 && !it.plain)
                    col.reset_w = std::max({col.reset_w, sample_reset_w, TextWidth(font_, it.reset)});
            }
            if (plain) col.value_w = std::max(col.value_w, sample_tokens_w);
            col.x = width + (c ? col_gap : 0);
            width = col.x + col.label_w + (col.label_w ? gap : 0) + col.value_w + (col.reset_w ? reset_gap + col.reset_w : 0);
        }
        return width + (ring_d ? col_gap + ring_d : 0);
    };
    int level = 0;
    for (; level < 4; ++level) {
        const int spare = level < fit_level_ ? scale(16) : 0;  // stepping back up: a little to spare
        if (measure(level) + spare <= room) break;
    }
    if (level != fit_level_) {
        fit_level_ = level;
        shown_tokens_w_ = 0;  // the token column starts at its new width
        if (level < 4) measure(level);
    }
    if (level < 4) tokens_w_goal_ = [&] {
        const int step = scale(6);
        const int w = (token_shown.empty() ? 0 : TextWidth(font_, token_shown.c_str()) + (blossom ? spin_gap : 0)) + (blossom ? spin_w_ : 0);
        return ((w + step - 1) / step) * step;
    }();
    // Nothing after the last column: the tray's arrow brings its own margin, about one gap.
    col_spans_.clear();
    for (int c = 0; c < cols; ++c) {  // clickable area of each column, gaps split between neighbours
        int left = c == 0 ? 0 : columns[c].x - col_gap / 2;
        int right = c + 1 < cols ? columns[c + 1].x - col_gap / 2 : width;
        col_spans_.emplace_back(left, right);
    }
    const int ring_x = width + col_gap;
    if (ring_d) width = ring_x + ring_d;
    if (check_due != check_spinning_) {  // turn while due, at about 30 frames a second
        check_spinning_ = check_due;
        if (check_due) SetTimer(hwnd_, kCheckTimer, 33, nullptr);
        else KillTimer(hwnd_, kCheckTimer);
    }

    // No room even with every detail dropped (a portrait monitor, a small laptop screen with many apps
    // open): just the blossom, as when folded by hand.
    const bool cramped = level >= 4;
    if ((settings.taskbar_collapsed || cramped) && !folding_) {
        check_hit_ = RECT{};
        if (check_spinning_) {  // no ring while collapsed
            KillTimer(hwnd_, kCheckTimer);
            check_spinning_ = false;
        }
        // Collapsed: only the blossom, at the far right by the tray; it still turns while tokens
        // flow. Clicking it brings everything back.
        RECT client;
        GetClientRect(taskbar_, &client);
        const int ch = client.bottom - client.top;
        const int cw = spin_box_ + scale(12);
        if (ch <= 0) return;
        col_spans_.clear();
        blossom_hit_ = RECT{0, 0, cw, ch};
        std::wstring key = L"collapsed|" + std::to_wstring(cw) + L"x" + std::to_wstring(ch) + font_key_ + L"|" +
                           std::to_wstring(token_frame_) + (settings.taskbar_transparent ? L"T" : L"") +
                           std::to_wstring(look.background_color);
        if (key != render_key_) {
            Canvas canvas;
            if (!canvas.Create(cw, ch)) return;
            if (settings.taskbar_transparent)
                canvas.Clear(RGB(0, 0, 0), 1);  // alpha 1: still takes the click
            else
                canvas.Clear(look.background_color, 255);
            const bool moving = token_frame_ >= 0;
            canvas.Draw(spin_sprites_[moving ? token_frame_ : kStillFrame], (cw - spin_box_) / 2,
                        (ch - spin_box_) / 2, kClaudeOrange, moving ? 255 : 120);
            POINT src = {0, 0};
            SIZE size = {cw, ch};
            BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, canvas.dc(), &src, 0, &bf, ULW_ALPHA);
            render_key_ = key;
        }
        int x = RightEdge(dpi) - cw + scale(settings.taskbar_offset_x);
        RECT want = {x, 0, x + cw, ch};
        if (!EqualRect(&want, &placed_) || !IsWindowVisible(hwnd_)) {
            SetWindowPos(hwnd_, HWND_TOP, x, 0, cw, ch, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            placed_ = want;
        }
        return;
    }

    RECT client;
    GetClientRect(taskbar_, &client);
    const int height = client.bottom - client.top;
    if (height <= 0 || width <= 0) return;
    check_hit_ = ring_d ? RECT{ring_x - col_gap / 2, 0, width, height} : RECT{};
    if (blossom && !folding_) {  // the blossom is a button: a click collapses the readout to it
        for (int i = 0; i < n; ++i) {
            if (!items[i].span) continue;
            const int bx = columns[i / rows].x;
            blossom_hit_ = RECT{std::max(0, bx - scale(4)), 0, bx + spin_w_ + scale(4), height};
            break;
        }
    }

    // Repaint only when something visible changed.
    std::wstring key = std::to_wstring(width) + L"x" + std::to_wstring(height) + font_key_ +
                       std::to_wstring(label) + L"," + std::to_wstring(value) + L"," +
                       std::to_wstring(look.bar_color) + L"," + std::to_wstring(look.background_color) +
                       (settings.taskbar_transparent ? L"T" : L"") + (settings.taskbar_right_align ? L"R" : L"") +
                       (settings.taskbar_blossom ? L"o" : L"") +
                       (look.show_reset ? L"r" : L"") + (look.show_bars ? L"b" : L"") + (look.bar_quarters ? L"q" : L"") + (light_bg ? L"L" : L"D");
    // Fade in then out (half a sine) over the confirmed column.
    unsigned flash_alpha = 0;
    if (flash_col_ >= 0 && flash_col_ < cols) {
        double t = static_cast<double>(GetTickCount64() - flash_start_) / kFlashMs;
        if (t < 1) flash_alpha = static_cast<unsigned>(140 * std::sin(t * 3.14159265358979));
    }
    key += L"|f" + std::to_wstring(flash_col_) + L":" + std::to_wstring(flash_alpha) + L"c" + std::to_wstring(flash_color_);
    if (has_tokens) key += L"|t" + token_shown + L"|" + std::to_wstring(token_frame_) + L"|L" + std::to_wstring(level);
    const double check_turn = check_due ? std::fmod(GetTickCount64() / 1000.0, 1.0) : 0;  // once a second
    if (ring_d) key += L"|c" + check_text + L"|" + std::to_wstring(static_cast<int>(check_left * 400)) + L"|" +
                       std::to_wstring(static_cast<int>(check_turn * 60));
    for (const DisplayItem& it : items) {
        key += L"|" + it.label + L"|" + it.value + L"|" + it.reset + L"|" + std::to_wstring(it.severity) +
               (it.accent ? L"a" : L"") + (it.dim ? L"d" : L"") + L"|" +
               std::to_wstring(static_cast<int>(it.fraction * 1000)) + L"g" + std::to_wstring(static_cast<int>(it.reserve_from * 100)) + L"c" + std::to_wstring(static_cast<int>(it.cap_at * 100));
    }

    int shown_width = width;  // narrower while folding
    if (folding_ || key != render_key_) {
        Canvas canvas;
        if (!canvas.Create(width, height)) return;
        double blossom_x = 3, blossom_y = (height - spin_box_) / 2.0;  // where the blossom sits when all is shown
        // Transparent mode keeps alpha 1 so the whole widget still gets hover/clicks.
        if (settings.taskbar_transparent)
            canvas.Clear(RGB(0, 0, 0), 1);
        else
            canvas.Clear(look.background_color, 255);

        // Centre the ink, not the line boxes: a line's box has empty room above its letters, so with
        // the bars along the bottom, the block sat a pixel below the tray icons' middle.
        const int block_h = rows * row_h + (rows - 1) * row_gap;
        const int box_top = (height - block_h) / 2;
        const int top = look.show_bars ? (height - letter_top_ - block_h) / 2 : box_top;
        for (int i = 0; i < n; ++i) {
            const DisplayItem& it = items[i];
            const Column& col = columns[i / rows];
            int y = top + (i % rows) * (row_h + row_gap);
            int vx = col.x + col.label_w + (col.label_w ? gap : 0);
            COLORREF vcolor = it.severity == 2 ? critical : (it.severity == 1 ? warn : value);

            if (it.span) {  // one value across both rows: "N tok/s", then the spinner, both centred
                int block = rows * row_h + (rows - 1) * row_gap;
                int right = col.x + col.label_w + (col.label_w ? gap : 0) + col.value_w;
                // The blossom's ink box sits at the column's left edge and the number starts just after
                // it, so the two stay tight together; the column is as wide as "9.9k tok/s" (real speeds stay under it), so the
                // accounts beside it never move either.
                int spin_x = col.x;
                int text_top = box_top + (block - line_h) / 2;  // one line: its own box is already centred
                RECT text = {spin_x + (blossom ? spin_w_ + spin_gap : 0), text_top, right, text_top + line_h};
                canvas.Text(token_shown, text, DT_LEFT, font_, value, 255);
                blossom_x = spin_x + (spin_w_ - spin_box_) / 2.0;
                blossom_y = text_top + digit_ink_ - spin_box_ / 2.0;
                if (!blossom || folding_) continue;  // while folding, the blossom is drawn rolling (below)
                // Still: a dimmed blossom. Moving: Claude Code's frames. Every frame's ink middle sits on
                // one fixed point, level with the middle of the digits.
                bool moving = token_frame_ >= 0;
                const Sprite& frame = spin_sprites_[moving ? token_frame_ : kStillFrame];
                canvas.Draw(frame, spin_x + (spin_w_ - spin_box_) / 2, text_top + digit_ink_ - spin_box_ / 2, kClaudeOrange,
                            moving ? 255 : 120);
                continue;
            }
            if (it.plain && it.value.empty() && it.label.empty()) continue;  // the empty half of a span
            const unsigned text_alpha = it.dim ? 185 : 255;  // standby accounts sit back a little
            COLORREF name = it.accent ? look.bar_color : label;
            if (flash_alpha && i / rows == flash_col_ && i % rows == 0) {
                // The name glows green and back: the switch to it is confirmed.
                double k = flash_alpha / 140.0;
                const COLORREF c = flash_color_;
                name = RGB(static_cast<int>(GetRValue(name) + (GetRValue(c) - GetRValue(name)) * k),
                           static_cast<int>(GetGValue(name) + (GetGValue(c) - GetGValue(name)) * k),
                           static_cast<int>(GetBValue(name) + (GetBValue(c) - GetBValue(name)) * k));
            }
            canvas.Text(it.label, RECT{col.x, y, col.x + col.label_w, y + line_h}, DT_LEFT, font_, name, text_alpha);
            canvas.Text(it.value, RECT{vx, y, vx + col.value_w, y + line_h},
                        settings.taskbar_right_align ? DT_RIGHT : DT_LEFT, font_, vcolor, text_alpha);
            if (col.reset_w) {
                int rx = vx + col.value_w + reset_gap;
                canvas.Text(it.reset, RECT{rx, y, rx + col.reset_w, y + line_h}, DT_LEFT, font_, label, 150);
            }
            if (look.show_bars && !it.plain) {
                int by = y + line_h + bar_gap;
                RECT track = {col.x, by, vx + col.value_w, by + bar_h};
                canvas.Bar(track, it.fraction, 0, label, 45, it.severity ? vcolor : look.bar_color, 255,
                           look.bar_quarters, std::max(1, scale(1)), it.reserve_from);
                if (it.cap_at >= 0) {  // the cap: a bright tick standing just proud of the bar
                    const LONG mx = track.left + static_cast<LONG>((track.right - track.left) * it.cap_at + 0.5);
                    const LONG half = std::max(1, scale(1));
                    canvas.Fill(RECT{mx - half, by - scale(1), mx + half, by + bar_h + scale(1)}, label, 255);
                }
            }
        }

        if (ring_d) {
            const double cx = ring_x + ring_d / 2.0, cy = height / 2.0;
            const double thick = std::max(1.5, scale(3) / 2.0), radius = ring_d / 2.0 - thick / 2 - 0.5;
            canvas.Arc(cx, cy, radius, thick, 0, 1, label, 45);  // the track, faint like the bars'
            // In the usage bars' colour.
            if (check_due) canvas.Arc(cx, cy, radius, thick, check_turn, 0.28, look.bar_color, 255);
            else canvas.Arc(cx, cy, radius, thick, 1 - check_left, check_left, look.bar_color, 255);
            const int text_top = static_cast<int>(cy) - small_digit_ink_;  // the digits' ink centred in the ring
            canvas.Text(check_text, RECT{ring_x, text_top, ring_x + ring_d, text_top + LineHeight(small_font_)}, DT_CENTER,
                        small_font_, value, check_due ? 200 : 255);
        }

        POINT src = {0, 0};
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        if (folding_) {
            // The window narrows from the left (it stays against the tray), the readout fades, and the
            // blossom rolls from its place by the token speed to the collapsed spot at the far right.
            const double e = EaseInOut(fold_pos_);
            const int collapsed_w = spin_box_ + scale(12);
            const int cut = static_cast<int>(std::lround((width - collapsed_w) * e));
            shown_width = width - cut;
            Canvas out;
            if (!out.Create(shown_width, height)) return;
            if (settings.taskbar_transparent)
                out.Clear(RGB(0, 0, 0), 1);
            else
                out.Clear(look.background_color, 255);
            out.Compose(canvas, cut, static_cast<unsigned>(255 * std::pow(1 - e, 1.6)));
            const double end_x = width - collapsed_w + (collapsed_w - spin_box_) / 2.0;
            const double end_y = (height - spin_box_) / 2.0;
            const double bx = blossom_x + (end_x - blossom_x) * e, by = blossom_y + (end_y - blossom_y) * e;
            const double roll = (bx - blossom_x) / (spin_box_ / 2.0);  // turned as far as it has rolled
            const bool moving = token_frame_ >= 0;
            out.DrawRotated(spin_sprites_[moving ? token_frame_ : kStillFrame], bx - cut + spin_box_ / 2.0,
                            by + spin_box_ / 2.0, roll, kClaudeOrange, moving || e > 0.02 ? 255 : 120);
            SIZE size = {shown_width, height};
            UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, out.dc(), &src, 0, &bf, ULW_ALPHA);
            render_key_.clear();  // the next still frame repaints in full
        } else {
            SIZE size = {width, height};
            UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, canvas.dc(), &src, 0, &bf, ULW_ALPHA);
            render_key_ = key;
        }
    }

    int x = RightEdge(dpi) - shown_width + scale(settings.taskbar_offset_x);
    RECT want = {x, 0, x + shown_width, height};
    if (!EqualRect(&want, &placed_) || !IsWindowVisible(hwnd_)) {
        SetWindowPos(hwnd_, HWND_TOP, x, 0, shown_width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        placed_ = want;
    } else if (GetWindow(hwnd_, GW_HWNDPREV)) {
        // Keep above the taskbar's own content when Explorer re-stacks it.
        SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

LRESULT CALLBACK TaskbarWidget::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<TaskbarWidget*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_TIMER:
        if (self && wp == kFoldTimer) {
            double t = (GetTickCount64() - self->fold_start_) / kFoldMs;
            if (t >= 1) {
                KillTimer(hwnd, kFoldTimer);
                self->folding_ = false;
                self->fold_pos_ = self->fold_target_;
            } else {
                self->fold_pos_ = self->fold_from_ + (self->fold_target_ - self->fold_from_) * t;
            }
            if (self->have_last_) self->Update(self->last_items_, self->last_settings_);
            return 0;
        }
        if (self && wp == kFlashTimer) {
            if (GetTickCount64() - self->flash_start_ > static_cast<ULONGLONG>(kFlashMs)) {
                KillTimer(hwnd, kFlashTimer);
                self->flash_col_ = -1;
            }
            if (self->have_last_) self->Update(self->last_items_, self->last_settings_);
        } else if (self && wp == kAnimTimer) {
            self->Animate();
        } else if (self && wp == kCheckTimer) {
            if (self->have_last_) self->Update(self->last_items_, self->last_settings_);
        }
        return 0;
    case WM_RBUTTONDOWN:
        return 0;
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:  // also the keyboard's menu key, and a pen or finger held down
        if (self) PostMessageW(self->owner_, kMsgContextMenu, 0, 0);
        return 0;
    case WM_LBUTTONDOWN:
        if (self) {
            self->left_down_ = true;
            self->left_down_at_ = GetTickCount64();
            self->down_was_right_ = (wp & (MK_CONTROL | MK_RBUTTON)) || (GetKeyState(VK_CONTROL) & 0x8000) ||
                                    (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
                                    (g_raw_right_at > g_raw_left_at && GetTickCount64() - g_raw_right_at < 3000);
        }
        return 0;
    case WM_INPUT: {
        RAWINPUT raw = {};
        UINT size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            raw.header.dwType == RIM_TYPEMOUSE) {
            const USHORT f = raw.data.mouse.usButtonFlags;
            if (f & RI_MOUSE_LEFT_BUTTON_DOWN) g_raw_left_at = GetTickCount64();
            if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) g_raw_right_at = GetTickCount64();
        }
        break;  // DefWindowProc cleans up
    }
    case WM_LBUTTONUP:
        // Only a left click that started here: never a stray release (another button, a drag in).
        if (self && self->left_down_) {
            self->left_down_ = false;
            // A right-click in disguise (remote-control apps and Macs send it as Ctrl + left click, or
            // with the right button's flag on, or the hardware said right): the menu, never a switch.
            const ULONGLONG t = GetTickCount64();
            const bool raw_right = g_raw_right_at > g_raw_left_at && t - g_raw_right_at < 3000;
            const bool ctrl = (wp & MK_CONTROL) || (GetKeyState(VK_CONTROL) & 0x8000);
            const bool rflag = (wp & MK_RBUTTON) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
            if (raw_right || ctrl || rflag || self->down_was_right_) {
                PostMessageW(self->owner_, kMsgContextMenu, 0, 0);
                return 0;
            }
            // A plain left click: the blossom folds or opens, the ring checks now, a name switches.
            POINT pt = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            if (PtInRect(&self->blossom_hit_, pt)) {
                if (self->tooltip_) SendMessageW(self->tooltip_, TTM_POP, 0, 0);  // no tip left behind
                PostMessageW(self->owner_, kMsgBlossomClick, 0, 0);
                return 0;
            }
            if (PtInRect(&self->check_hit_, pt)) {
                PostMessageW(self->owner_, kMsgCheckClick, 0, 0);
                return 0;
            }
            int col = self->ColumnAt(pt.x);
            if (col >= 0 && col < static_cast<int>(self->col_clickable_.size()) && self->col_clickable_[col])
                PostMessageW(self->owner_, kMsgColumnClick, static_cast<WPARAM>(col), 0);
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        // Clicks switch accounts in the multi-account view; double-click opens Options otherwise.
        if (self && self->col_tips_.empty()) PostMessageW(self->owner_, kMsgDoubleClick, 0, 0);
        return 0;
    case WM_MOUSEMOVE:
        if (self) {
            int col = self->col_tips_.empty() ? -1 : self->ColumnAt(static_cast<short>(LOWORD(lp)));
            if (col != self->hover_col_) {
                self->hover_col_ = col;
                self->ShowTooltipText();
            }
        }
        break;
    case WM_SETCURSOR:
        if (self && self->hover_col_ >= 0 && self->hover_col_ < static_cast<int>(self->col_clickable_.size()) &&
            self->col_clickable_[self->hover_col_]) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(hwnd, nullptr);  // content comes from UpdateLayeredWindow
        return 0;
    case WM_NCDESTROY:
        if (self && self->hwnd_ == hwnd) {
            self->hwnd_ = nullptr;
            self->tooltip_ = nullptr;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
