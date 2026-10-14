#include "TaskbarWidget.h"

#include <commctrl.h>

#include <algorithm>

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

}  // namespace

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
}

bool TaskbarWidget::Attach() {
    Destroy();
    taskbar_ = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar_) return false;
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kWidgetClass, L"Claude Usage",
                            WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, taskbar_, nullptr, instance_, this);
    if (!hwnd_) return false;

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
        SendMessageW(tooltip_, TTM_ACTIVATE, tooltip_enabled_, 0);
    }
    return true;
}

void TaskbarWidget::SetTooltip(const std::wstring& text, bool enabled) {
    if (tooltip_ && enabled != tooltip_enabled_) SendMessageW(tooltip_, TTM_ACTIVATE, enabled, 0);
    tooltip_enabled_ = enabled;
    if (text == tooltip_text_) return;
    tooltip_text_ = text;
    if (!tooltip_ || !hwnd_) return;
    TOOLINFOW ti = {};
    ti.cbSize = sizeof(ti);
    ti.hwnd = hwnd_;
    ti.uId = reinterpret_cast<UINT_PTR>(hwnd_);
    ti.lpszText = const_cast<wchar_t*>(tooltip_text_.c_str());
    SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
}

int TaskbarWidget::RightEdge(int dpi) const {
    RECT client;
    GetClientRect(taskbar_, &client);
    int right = client.right - MulDiv(280, dpi, 96);  // fallback when the tray can't be found

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

void TaskbarWidget::Update(const std::vector<DisplayItem>& items, const Settings& settings) {
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
        font_ = CreateUiFont(look.font_name, look.font_size, look.font_bold, dpi);
        font_key_ = font_key;
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
    const int pad = scale(3), gap = scale(4), reset_gap = scale(6), col_gap = scale(10);
    const int line_h = LineHeight(font_);
    const int bar_h = look.show_bars ? scale(2) : 0, bar_gap = look.show_bars ? scale(1) : 0;
    const int row_gap = look.show_bars ? scale(1) : scale(2);
    const int row_h = line_h + bar_gap + bar_h;
    const std::wstring sample = look.hide_percent ? L"100" : (look.percent_space ? L"100 %" : L"100%");
    const int sample_reset_w = TextWidth(font_, L"23h 59m");

    struct Column { int label_w = 0, value_w = 0, reset_w = 0, x = 0; };
    std::vector<Column> columns(cols);
    int width = pad;
    for (int c = 0; c < cols; ++c) {
        Column& col = columns[c];
        col.value_w = TextWidth(font_, sample);
        for (int r = 0; r < rows && c * rows + r < n; ++r) {
            const DisplayItem& it = items[c * rows + r];
            col.label_w = std::max(col.label_w, TextWidth(font_, it.label));
            col.value_w = std::max(col.value_w, TextWidth(font_, it.value));
            if (look.show_reset) col.reset_w = std::max({col.reset_w, sample_reset_w, TextWidth(font_, it.reset)});
        }
        col.x = width + (c ? col_gap : 0);
        width = col.x + col.label_w + (col.label_w ? gap : 0) + col.value_w + (look.show_reset ? reset_gap + col.reset_w : 0);
    }
    width += pad;

    RECT client;
    GetClientRect(taskbar_, &client);
    const int height = client.bottom - client.top;
    if (height <= 0 || width <= 0) return;

    // Repaint only when something visible changed.
    std::wstring key = std::to_wstring(width) + L"x" + std::to_wstring(height) + font_key_ +
                       std::to_wstring(label) + L"," + std::to_wstring(value) + L"," +
                       std::to_wstring(look.bar_color) + L"," + std::to_wstring(look.background_color) +
                       (settings.taskbar_transparent ? L"T" : L"") + (settings.taskbar_right_align ? L"R" : L"") +
                       (look.show_reset ? L"r" : L"") + (look.show_bars ? L"b" : L"") + (light_bg ? L"L" : L"D");
    for (const DisplayItem& it : items) {
        key += L"|" + it.label + L"|" + it.value + L"|" + it.reset + L"|" + std::to_wstring(it.severity) + L"|" +
               std::to_wstring(static_cast<int>(it.fraction * 1000));
    }

    if (key != render_key_) {
        Canvas canvas;
        if (!canvas.Create(width, height)) return;
        // Transparent mode keeps alpha 1 so the whole widget still gets hover/clicks.
        if (settings.taskbar_transparent)
            canvas.Clear(RGB(0, 0, 0), 1);
        else
            canvas.Clear(look.background_color, 255);

        const int top = (height - (rows * row_h + (rows - 1) * row_gap)) / 2;
        for (int i = 0; i < n; ++i) {
            const DisplayItem& it = items[i];
            const Column& col = columns[i / rows];
            int y = top + (i % rows) * (row_h + row_gap);
            int vx = col.x + col.label_w + (col.label_w ? gap : 0);
            COLORREF vcolor = it.severity == 2 ? critical : (it.severity == 1 ? warn : value);

            canvas.Text(it.label, RECT{col.x, y, col.x + col.label_w, y + line_h}, DT_LEFT, font_, label, 255);
            canvas.Text(it.value, RECT{vx, y, vx + col.value_w, y + line_h},
                        settings.taskbar_right_align ? DT_RIGHT : DT_LEFT, font_, vcolor, 255);
            if (look.show_reset) {
                int rx = vx + col.value_w + reset_gap;
                canvas.Text(it.reset, RECT{rx, y, rx + col.reset_w, y + line_h}, DT_LEFT, font_, label, 150);
            }
            if (look.show_bars) {
                int by = y + line_h + bar_gap;
                RECT track = {col.x, by, vx + col.value_w, by + bar_h};
                canvas.Fill(track, label, 45);
                if (it.fraction > 0) {
                    RECT bar = track;
                    bar.right = bar.left + static_cast<LONG>((track.right - track.left) * it.fraction + 0.5f);
                    canvas.Fill(bar, it.severity ? vcolor : look.bar_color, 255);
                }
            }
        }

        POINT src = {0, 0};
        SIZE size = {width, height};
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, canvas.dc(), &src, 0, &bf, ULW_ALPHA);
        render_key_ = key;
    }

    int x = RightEdge(dpi) - width + scale(settings.taskbar_offset_x);
    RECT want = {x, 0, x + width, height};
    if (!EqualRect(&want, &placed_) || !IsWindowVisible(hwnd_)) {
        SetWindowPos(hwnd_, HWND_TOP, x, 0, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
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
    case WM_RBUTTONUP:
        if (self) PostMessageW(self->owner_, kMsgContextMenu, 0, 0);
        return 0;
    case WM_LBUTTONDBLCLK:
        if (self) PostMessageW(self->owner_, kMsgDoubleClick, 0, 0);
        return 0;
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
