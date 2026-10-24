#include "MainWindow.h"

#include <commctrl.h>

#include <algorithm>

#include "Canvas.h"

namespace {

const wchar_t kMainClass[] = L"ClaudeUsageMonitor.MainWindow";
const COLORREF kWarn = RGB(255, 179, 64), kCritical = RGB(255, 107, 107);
const COLORREF kWarnOnLight = RGB(184, 110, 0), kCriticalOnLight = RGB(208, 32, 32);

bool IsLightColor(COLORREF c) {
    return GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114 > 128000;
}

}  // namespace

void MainWindow::Init(HINSTANCE instance, HWND owner) {
    instance_ = instance;
    owner_ = owner;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &MainWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);
}

void MainWindow::Open(const Settings& settings) {
    if (hwnd_) return;
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kMainClass, L"Claude Usage",
                            WS_POPUP, settings.main_x, settings.main_y, 1, 1, nullptr, nullptr, instance_, this);
    if (!hwnd_) return;
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
        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_INITIAL, 3000);  // only after a long rest
        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_RESHOW, 3000);
    }
    render_key_.clear();
}

void MainWindow::Close() {
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    tooltip_ = nullptr;
}

void MainWindow::SetTooltip(const std::wstring& text, bool enabled) {
    if (tooltip_ && enabled != tooltip_enabled_) SendMessageW(tooltip_, TTM_ACTIVATE, enabled, 0);
    tooltip_enabled_ = enabled;
    if (text == tooltip_text_) return;
    tooltip_text_ = text;
    if (!tooltip_) return;
    TOOLINFOW ti = {};
    ti.cbSize = sizeof(ti);
    ti.hwnd = hwnd_;
    ti.uId = reinterpret_cast<UINT_PTR>(hwnd_);
    ti.lpszText = const_cast<wchar_t*>(tooltip_text_.c_str());
    SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
}

void MainWindow::ApplyWindowStyle(const Settings& settings) {
    locked_ = settings.main_locked;
    allow_offscreen_ = settings.main_offscreen;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    LONG_PTR want = settings.main_click_through ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
    if (want != ex) SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, want);
    bool topmost = (ex & WS_EX_TOPMOST) != 0;
    if (topmost != settings.main_topmost)
        SetWindowPos(hwnd_, settings.main_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void MainWindow::ClampToScreen(RECT& r) const {
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    const RECT& wa = mi.rcWork;
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = std::max<LONG>(wa.left, std::min<LONG>(r.left, wa.right - w));
    int y = std::max<LONG>(wa.top, std::min<LONG>(r.top, wa.bottom - h));
    r = RECT{x, y, x + w, y + h};
}

void MainWindow::Update(const std::vector<DisplayItem>& items, const Settings& settings) {
    if (!hwnd_) return;
    ApplyWindowStyle(settings);

    const WindowLook& look = settings.main;
    int dpi = static_cast<int>(GetDpiForWindow(hwnd_));
    if (dpi <= 0) dpi = 96;
    auto scale = [dpi](int px) { return MulDiv(px, dpi, 96); };

    std::wstring font_key = look.font_name + L"|" + std::to_wstring(look.font_size) + (look.font_bold ? L"b" : L"") +
                            L"|" + std::to_wstring(dpi);
    if (!font_ || font_key != font_key_) {
        if (font_) DeleteObject(font_);
        if (small_font_) DeleteObject(small_font_);
        font_ = CreateUiFont(look.font_name, look.font_size, look.font_bold, dpi);
        small_font_ = CreateUiFont(look.font_name, std::max(6, look.font_size - 1), false, dpi);
        font_key_ = font_key;
    }

    const bool light = IsLightColor(look.background_color);
    const COLORREF warn = light ? kWarnOnLight : kWarn, critical = light ? kCriticalOnLight : kCritical;
    const int pad = scale(12), gap = scale(16), item_gap = scale(10);
    const int line_h = LineHeight(font_), small_h = LineHeight(small_font_);
    const bool second_line = look.show_bars || look.show_reset;
    const int bar_h = scale(4);

    if (std::any_of(items.begin(), items.end(), [](const DisplayItem& it) { return it.header || it.compact; })) {
        SIZE size;
        if (RenderAccounts(items, settings, dpi, size)) Place(size.cx, size.cy, settings);
        return;
    }

    // Measure.
    int content_w = scale(150);
    for (const DisplayItem& it : items) {
        content_w = std::max(content_w, TextWidth(font_, it.label) + gap + TextWidth(font_, it.value));
        if (look.show_reset) content_w = std::max(content_w, scale(60) + gap + TextWidth(small_font_, it.reset));
    }
    const int item_h = line_h + (second_line ? scale(3) + std::max(small_h, bar_h) : 0);
    int n = std::max<int>(1, static_cast<int>(items.size()));
    const int width = pad * 2 + content_w;
    const int height = pad * 2 + n * item_h + (n - 1) * item_gap;

    std::wstring key = std::to_wstring(width) + L"x" + std::to_wstring(height) + font_key_ +
                       std::to_wstring(look.label_color) + L"," + std::to_wstring(look.value_color) + L"," +
                       std::to_wstring(look.bar_color) + L"," + std::to_wstring(look.background_color) + L"," +
                       std::to_wstring(settings.main_opacity) + (look.show_reset ? L"r" : L"") +
                       (look.show_bars ? L"b" : L"") + (look.bar_quarters ? L"q" : L"");
    for (const DisplayItem& it : items) {
        key += L"|" + it.label + L"|" + it.value + L"|" + it.reset + L"|" + std::to_wstring(it.severity) + L"|" +
               std::to_wstring(static_cast<int>(it.fraction * 1000));
    }

    if (key != render_key_) {
        Canvas canvas;
        if (!canvas.Create(width, height)) return;
        unsigned alpha = static_cast<unsigned>(settings.main_opacity * 255 / 100);
        canvas.FillRounded(RECT{0, 0, width, height}, scale(8), look.background_color, alpha);

        int y = pad;
        if (items.empty()) {
            canvas.Text(L"No items selected", RECT{pad, y, width - pad, y + line_h}, DT_LEFT, font_,
                        look.label_color, 255);
        }
        for (const DisplayItem& it : items) {
            COLORREF vcolor = it.severity == 2 ? critical : (it.severity == 1 ? warn : look.value_color);
            canvas.Text(it.label, RECT{pad, y, width - pad, y + line_h}, DT_LEFT, font_, look.label_color, 255);
            canvas.Text(it.value, RECT{pad, y, width - pad, y + line_h}, DT_RIGHT, font_, vcolor, 255);
            if (second_line) {
                int y2 = y + line_h + scale(3);
                int row2_h = std::max(small_h, bar_h);
                int reset_w = look.show_reset ? TextWidth(small_font_, it.reset.empty() ? L"--" : it.reset) : 0;
                if (look.show_reset)
                    canvas.Text(it.reset.empty() ? L"--" : it.reset, RECT{width - pad - reset_w, y2, width - pad, y2 + row2_h},
                                DT_RIGHT, small_font_, look.label_color, 170);
                if (look.show_bars) {
                    int bar_right = width - pad - (look.show_reset ? reset_w + scale(8) : 0);
                    int by = y2 + (row2_h - bar_h) / 2;
                    RECT track = {pad, by, bar_right, by + bar_h};
                    canvas.Bar(track, it.fraction, bar_h / 2, look.label_color, 50, it.severity ? vcolor : look.bar_color,
                               255, look.bar_quarters, scale(2));
                }
            }
            y += item_h + item_gap;
        }

        POINT src = {0, 0};
        SIZE size = {width, height};
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, canvas.dc(), &src, 0, &bf, ULW_ALPHA);
        render_key_ = key;
    }

    Place(width, height, settings);
}

void MainWindow::Place(int width, int height, const Settings& settings) {
    int dpi = static_cast<int>(GetDpiForWindow(hwnd_));
    auto scale = [dpi](int px) { return MulDiv(px, dpi > 0 ? dpi : 96, 96); };
    RECT wr;
    GetWindowRect(hwnd_, &wr);
    bool resized = (wr.right - wr.left) != width || (wr.bottom - wr.top) != height;
    if (!IsWindowVisible(hwnd_) || resized) {
        RECT r = {wr.left, wr.top, wr.left + width, wr.top + height};
        if (!settings.main_has_position && !IsWindowVisible(hwnd_)) {
            // First run: bottom-right corner of the primary work area.
            RECT wa;
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            r = RECT{wa.right - width - scale(24), wa.bottom - height - scale(24), wa.right - scale(24),
                     wa.bottom - scale(24)};
        }
        if (!settings.main_offscreen) ClampToScreen(r);
        SetWindowPos(hwnd_, nullptr, r.left, r.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->hwnd_ == hwnd) return self->Handle(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN:
        if (!locked_) {
            ReleaseCapture();
            SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);  // system drag loop
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        PostMessageW(owner_, kMsgDoubleClick, 0, 0);
        return 0;
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:  // also the keyboard's menu key, and a pen or finger held down
        PostMessageW(owner_, kMsgContextMenu, 0, 0);
        return 0;
    case WM_MOVING:
        if (!allow_offscreen_) ClampToScreen(*reinterpret_cast<RECT*>(lp));
        return TRUE;
    case WM_EXITSIZEMOVE: {
        RECT r;
        GetWindowRect(hwnd_, &r);
        PostMessageW(owner_, kMsgMoved, 0, MAKELPARAM(static_cast<short>(r.left), static_cast<short>(r.top)));
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
        render_key_.clear();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(hwnd_, nullptr);
        return 0;
    case WM_NCDESTROY:
        hwnd_ = nullptr;
        tooltip_ = nullptr;
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// cswap-watch style: an account header line, then one compact line per limit.
bool MainWindow::RenderAccounts(const std::vector<DisplayItem>& items, const Settings& settings, int dpi, SIZE& size) {
    const WindowLook& look = settings.main;
    auto scale = [dpi](int px) { return MulDiv(px, dpi, 96); };
    const bool light = IsLightColor(look.background_color);
    const COLORREF warn = light ? kWarnOnLight : kWarn, critical = light ? kCriticalOnLight : kCritical;
    const int pad = scale(14), gap = scale(10), indent = scale(18), bar_w = scale(150), bar_h = scale(4);
    const int line_h = LineHeight(font_);
    const int header_gap = scale(4), row_gap = scale(3), account_gap = scale(12);

    int label_col = 0, value_col = TextWidth(font_, L"100 %"), reset_col = 0, header_w = 0;
    int height = pad * 2;
    bool first = true;
    for (const DisplayItem& it : items) {
        if (it.header) {
            header_w = std::max(header_w, TextWidth(font_, it.label) + gap + TextWidth(small_font_, it.sublabel) +
                                              gap * 2 + TextWidth(small_font_, it.value));
            height += (first ? 0 : account_gap) + line_h + header_gap;
            first = false;
        } else {
            label_col = std::max(label_col, TextWidth(small_font_, it.label));
            value_col = std::max(value_col, TextWidth(font_, it.value));
            if (look.show_reset && !it.reset.empty()) reset_col = std::max(reset_col, TextWidth(small_font_, L"resets " + it.reset));
            height += line_h + row_gap;
        }
    }
    const int row_w = indent + label_col + gap + (look.show_bars ? bar_w + gap : 0) + value_col +
                      (reset_col ? gap + reset_col : 0);
    const int width = pad * 2 + std::max({row_w, header_w, scale(220)});
    size = SIZE{width, height};

    std::wstring key = L"acc" + std::to_wstring(width) + L"x" + std::to_wstring(height) + font_key_ +
                       std::to_wstring(look.label_color) + L"," + std::to_wstring(look.value_color) + L"," +
                       std::to_wstring(look.bar_color) + L"," + std::to_wstring(look.background_color) + L"," +
                       std::to_wstring(settings.main_opacity) + (look.show_reset ? L"r" : L"") + (look.show_bars ? L"b" : L"") + (look.bar_quarters ? L"q" : L"");
    for (const DisplayItem& it : items) {
        key += L"|" + it.label + L"|" + it.sublabel + L"|" + it.value + L"|" + it.reset + L"|" +
               std::to_wstring(it.severity) + (it.accent ? L"a" : L"") + (it.dim ? L"d" : L"") +
               std::to_wstring(static_cast<int>(it.fraction * 1000));
    }
    if (key == render_key_) return true;

    Canvas canvas;
    if (!canvas.Create(width, height)) return false;
    canvas.FillRounded(RECT{0, 0, width, height}, scale(8), look.background_color,
                       static_cast<unsigned>(settings.main_opacity * 255 / 100));

    int y = pad;
    first = true;
    for (const DisplayItem& it : items) {
        const unsigned alpha = it.dim ? 200 : 255;
        if (it.header) {
            if (!first) y += account_gap;
            first = false;
            int lw = TextWidth(font_, it.label);
            canvas.Text(it.label, RECT{pad, y, pad + lw, y + line_h}, DT_LEFT, font_, look.value_color, alpha);
            if (!it.sublabel.empty())
                canvas.Text(it.sublabel, RECT{pad + lw + gap, y, width - pad, y + line_h}, DT_LEFT, small_font_,
                            look.label_color, 200);
            canvas.Text(it.value, RECT{pad, y, width - pad, y + line_h}, DT_RIGHT, small_font_,
                        it.accent ? look.bar_color : look.label_color, it.accent ? 255 : 170);
            y += line_h + header_gap;
            continue;
        }
        const COLORREF vcolor = it.severity == 2 ? critical : (it.severity == 1 ? warn : look.value_color);
        int x = pad + indent;
        canvas.Text(it.label, RECT{x, y, x + label_col, y + line_h}, DT_LEFT, small_font_, look.label_color, alpha);
        x += label_col + gap;
        if (look.show_bars) {
            int by = y + (line_h - bar_h) / 2;
            RECT track = {x, by, x + bar_w, by + bar_h};
            canvas.Bar(track, it.fraction, bar_h / 2, look.label_color, 45, it.severity ? vcolor : look.bar_color, alpha,
                       look.bar_quarters, scale(2));
            x += bar_w + gap;
        }
        canvas.Text(it.value, RECT{x, y, x + value_col, y + line_h}, DT_RIGHT, font_, vcolor, alpha);
        x += value_col + gap;
        if (reset_col && !it.reset.empty())
            canvas.Text(L"resets " + it.reset, RECT{x, y, x + reset_col, y + line_h}, DT_LEFT, small_font_,
                        look.label_color, 170);
        y += line_h + row_gap;
    }

    POINT src = {0, 0};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, canvas.dc(), &src, 0, &bf, ULW_ALPHA);
    render_key_ = key;
    return true;
}
