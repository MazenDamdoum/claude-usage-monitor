#include "OptionsDialog.h"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>

#include "AppFiles.h"
#include "Canvas.h"
#include "UsageClient.h"
#include "resource.h"

namespace options {
namespace {

const wchar_t kScrollClass[] = L"ClaudeUsageMonitor.OptionsScroll";

// Shared custom colors for every color picker: the app palette first.
COLORREF g_custom_colors[16] = {
    RGB(217, 119, 87), RGB(255, 255, 255), RGB(190, 190, 190), RGB(128, 128, 128),
    RGB(64, 64, 64),   RGB(24, 24, 24),    RGB(0, 0, 0),       RGB(255, 179, 64),
    RGB(255, 107, 107), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
    RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
};

struct SheetState;

struct PageCtx {
    SheetState* state = nullptr;
    Page page = kPageGeneral;
    bool ready = false;  // ignore change notifications during WM_INITDIALOG
    HWND hwnd = nullptr;
};

// The Options window: every page in one scrolling view (taskbar, main window, general).
struct SheetState {
    HINSTANCE instance = nullptr;
    Settings working;
    bool startup = false;
    const ApplyFn* apply = nullptr;
    Page start_page = kPageTaskbar;
    HWND host = nullptr;
    HWND scroll = nullptr;
    PageCtx pages[3];
    int section_y[3] = {};  // where each page's heading sits, by Page
    int content_h = 0;
    int pos = 0;            // scrolled this far down
    HFONT header_font = nullptr;
    bool laid_out = false;  // follow the keyboard focus only once the pages are in place
};

SheetState* g_state = nullptr;  // the open Options window, if any

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
void SetCheck(HWND dlg, int id, bool v) { CheckDlgButton(dlg, id, v ? BST_CHECKED : BST_UNCHECKED); }
bool GetCheck(HWND dlg, int id) { return IsDlgButtonChecked(dlg, id) == BST_CHECKED; }

void SetupSpin(HWND dlg, int spin_id, int lo, int hi, int value) {
    SendDlgItemMessageW(dlg, spin_id, UDM_SETRANGE32, static_cast<WPARAM>(lo), static_cast<LPARAM>(hi));
    SendDlgItemMessageW(dlg, spin_id, UDM_SETPOS32, 0, value);
}

int GetSpin(HWND dlg, int spin_id, int lo, int hi) {
    BOOL error = FALSE;
    int v = static_cast<int>(SendDlgItemMessageW(dlg, spin_id, UDM_GETPOS32, 0, reinterpret_cast<LPARAM>(&error)));
    return Clamp(v, lo, hi);
}

std::wstring GetText(HWND dlg, int id) {
    wchar_t buf[1024] = {};
    GetDlgItemTextW(dlg, id, buf, 1024);
    return buf;
}

void Changed(HWND page) {
    auto* ctx = reinterpret_cast<PageCtx*>(GetWindowLongPtrW(page, DWLP_USER));
    if (ctx && ctx->ready && ctx->state->host) EnableWindow(GetDlgItem(ctx->state->host, IDC_OPT_APPLY), TRUE);
}

bool PickColor(HWND owner, COLORREF& color) {
    CHOOSECOLORW cc = {};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = owner;
    cc.rgbResult = color;
    cc.lpCustColors = g_custom_colors;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR;
    if (!ChooseColorW(&cc)) return false;
    color = cc.rgbResult;
    return true;
}

bool PickFont(HWND owner, WindowLook& look) {
    LOGFONTW lf = {};
    HDC dc = GetDC(owner);
    lf.lfHeight = -MulDiv(look.font_size, GetDeviceCaps(dc, LOGPIXELSY), 72);
    ReleaseDC(owner, dc);
    lf.lfWeight = look.font_bold ? FW_BOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, look.font_name.c_str(), LF_FACESIZE);
    CHOOSEFONTW cf = {};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = owner;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS | CF_NOSCRIPTSEL | CF_LIMITSIZE;
    cf.nSizeMin = 6;
    cf.nSizeMax = 28;
    if (!ChooseFontW(&cf)) return false;
    look.font_name = lf.lfFaceName;
    look.font_size = Clamp(cf.iPointSize / 10, 6, 28);
    look.font_bold = lf.lfWeight >= FW_SEMIBOLD;
    return true;
}

void DrawSwatch(const DRAWITEMSTRUCT* di, COLORREF color) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    bool disabled = (di->itemState & ODS_DISABLED) != 0;
    FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));
    InflateRect(&r, -1, -1);
    HBRUSH fill = CreateSolidBrush(disabled ? GetSysColor(COLOR_BTNFACE) : color);
    FillRect(dc, &r, fill);
    DeleteObject(fill);
    HBRUSH frame = CreateSolidBrush(GetSysColor(disabled ? COLOR_GRAYTEXT : COLOR_BTNSHADOW));
    FrameRect(dc, &r, frame);
    DeleteObject(frame);
    if (di->itemState & ODS_FOCUS) {
        RECT f = di->rcItem;
        DrawFocusRect(dc, &f);
    }
}

// ---- Display items dialog ---------------------------------------------------

struct ItemsArgs {
    const wchar_t* title;
    ItemConfig* items;
    const WindowLook* defaults;
};

INT_PTR CALLBACK ItemsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* args = reinterpret_cast<ItemsArgs*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        args = reinterpret_cast<ItemsArgs*>(lp);
        SetWindowTextW(dlg, args->title);
        for (int i = 0; i < kItemCount; ++i) {
            SetCheck(dlg, IDC_DI_SHOW0 + i, args->items[i].show);
            SetDlgItemTextW(dlg, IDC_DI_LABEL0 + i, args->items[i].label.c_str());
        }
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_DI_DEFAULTS:
            for (int i = 0; i < kItemCount; ++i) {
                SetCheck(dlg, IDC_DI_SHOW0 + i, args->defaults->items[i].show);
                SetDlgItemTextW(dlg, IDC_DI_LABEL0 + i, args->defaults->items[i].label.c_str());
            }
            return TRUE;
        case IDOK: {
            bool any = false;
            for (int i = 0; i < kItemCount; ++i) any = any || GetCheck(dlg, IDC_DI_SHOW0 + i);
            if (!any) {
                MessageBoxW(dlg, L"Choose at least one item to display.", L"Display Items", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            for (int i = 0; i < kItemCount; ++i) {
                args->items[i].show = GetCheck(dlg, IDC_DI_SHOW0 + i);
                args->items[i].label = GetText(dlg, IDC_DI_LABEL0 + i);
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

// ---- Main window / taskbar window pages ---------------------------------------

WindowLook& LookOf(PageCtx* ctx) {
    return ctx->page == kPageMain ? ctx->state->working.main : ctx->state->working.taskbar;
}

COLORREF* SwatchColor(PageCtx* ctx, int id) {
    WindowLook& look = LookOf(ctx);
    switch (id) {
    case IDC_LABEL_COLOR: return &look.label_color;
    case IDC_VALUE_COLOR: return &look.value_color;
    case IDC_BAR_COLOR: return &look.bar_color;
    case IDC_BG_COLOR: return &look.background_color;
    default: return nullptr;
    }
}

void UpdateLookEnables(HWND dlg, PageCtx* ctx) {
    if (ctx->page != kPageTaskbar) return;
    bool auto_colors = GetCheck(dlg, IDC_AUTO_COLORS);
    EnableWindow(GetDlgItem(dlg, IDC_LABEL_COLOR), !auto_colors);
    EnableWindow(GetDlgItem(dlg, IDC_VALUE_COLOR), !auto_colors);
    EnableWindow(GetDlgItem(dlg, IDC_BG_COLOR), !GetCheck(dlg, IDC_TRANSPARENT));
    bool tokens = GetCheck(dlg, IDC_TOKENS_SHOW);
    EnableWindow(GetDlgItem(dlg, IDC_TOKENS_PILE), tokens);
    EnableWindow(GetDlgItem(dlg, IDC_BLOSSOM), tokens);
    EnableWindow(GetDlgItem(dlg, IDC_LONG_UNIT), tokens && !GetCheck(dlg, IDC_TOKENS_PILE));
    EnableWindow(GetDlgItem(dlg, IDC_BAR_QUARTERS), GetCheck(dlg, IDC_SHOW_BARS));
}

void InitLookPage(HWND dlg, PageCtx* ctx) {
    const Settings& s = ctx->state->working;
    const WindowLook& look = LookOf(ctx);
    SetDlgItemTextW(dlg, IDC_FONT_NAME, (look.font_name + (look.font_bold ? L", Bold" : L"")).c_str());
    SetupSpin(dlg, IDC_FONT_SPIN, 6, 28, look.font_size);
    SetCheck(dlg, IDC_SHOW_RESET, look.show_reset);
    SetCheck(dlg, IDC_SHOW_BARS, look.show_bars);
    SetCheck(dlg, IDC_BAR_QUARTERS, look.bar_quarters);
    SetCheck(dlg, IDC_SPACE, look.percent_space);
    SetCheck(dlg, IDC_HIDE_PERCENT, look.hide_percent);
    SetCheck(dlg, IDC_TOOLTIP, look.tooltip);
    if (ctx->page == kPageTaskbar) {
        SetCheck(dlg, IDC_TRANSPARENT, s.taskbar_transparent);
        SetCheck(dlg, IDC_AUTO_COLORS, s.taskbar_auto_colors);
        SetCheck(dlg, IDC_RIGHT_ALIGN, s.taskbar_right_align);
        SetCheck(dlg, IDC_HORIZONTAL, s.taskbar_horizontal);
        SetupSpin(dlg, IDC_OFFSET_SPIN, -2000, 2000, s.taskbar_offset_x);
        SetCheck(dlg, IDC_TOKENS_SHOW, s.taskbar_tokens);
        SetCheck(dlg, IDC_TOKENS_PILE, s.taskbar_token_pile);
        SetCheck(dlg, IDC_BLOSSOM, s.taskbar_blossom);
        SetCheck(dlg, IDC_SWITCH_GLOW, s.taskbar_switch_glow);
        SetCheck(dlg, IDC_ACTIVE_FIRST, s.taskbar_active_first);
        SetCheck(dlg, IDC_DIM_STANDBY, s.taskbar_dim_standby);
        SetCheck(dlg, IDC_LONG_UNIT, s.taskbar_long_unit);
        SetCheck(dlg, IDC_CHECK_TIMER, s.taskbar_check_timer);
        SetCheck(dlg, IDC_FIT, s.taskbar_fit);
    } else {
        SetupSpin(dlg, IDC_OPACITY_SPIN, 20, 100, s.main_opacity);
        SetCheck(dlg, IDC_TOPMOST, s.main_topmost);
        SetCheck(dlg, IDC_CLICK_THROUGH, s.main_click_through);
        SetCheck(dlg, IDC_LOCK, s.main_locked);
        SetCheck(dlg, IDC_OFFSCREEN, s.main_offscreen);
    }
    UpdateLookEnables(dlg, ctx);
}

void ApplyLookPage(HWND dlg, PageCtx* ctx) {
    Settings& s = ctx->state->working;
    WindowLook& look = LookOf(ctx);
    look.font_size = GetSpin(dlg, IDC_FONT_SPIN, 6, 28);
    look.show_reset = GetCheck(dlg, IDC_SHOW_RESET);
    look.show_bars = GetCheck(dlg, IDC_SHOW_BARS);
    look.bar_quarters = GetCheck(dlg, IDC_BAR_QUARTERS);
    look.percent_space = GetCheck(dlg, IDC_SPACE);
    look.hide_percent = GetCheck(dlg, IDC_HIDE_PERCENT);
    look.tooltip = GetCheck(dlg, IDC_TOOLTIP);
    if (ctx->page == kPageTaskbar) {
        s.taskbar_transparent = GetCheck(dlg, IDC_TRANSPARENT);
        s.taskbar_auto_colors = GetCheck(dlg, IDC_AUTO_COLORS);
        s.taskbar_right_align = GetCheck(dlg, IDC_RIGHT_ALIGN);
        s.taskbar_horizontal = GetCheck(dlg, IDC_HORIZONTAL);
        s.taskbar_offset_x = GetSpin(dlg, IDC_OFFSET_SPIN, -2000, 2000);
        s.taskbar_tokens = GetCheck(dlg, IDC_TOKENS_SHOW);
        s.taskbar_token_pile = GetCheck(dlg, IDC_TOKENS_PILE);
        s.taskbar_blossom = GetCheck(dlg, IDC_BLOSSOM);
        s.taskbar_switch_glow = GetCheck(dlg, IDC_SWITCH_GLOW);
        s.taskbar_active_first = GetCheck(dlg, IDC_ACTIVE_FIRST);
        s.taskbar_dim_standby = GetCheck(dlg, IDC_DIM_STANDBY);
        s.taskbar_long_unit = GetCheck(dlg, IDC_LONG_UNIT);
        s.taskbar_check_timer = GetCheck(dlg, IDC_CHECK_TIMER);
        s.taskbar_fit = GetCheck(dlg, IDC_FIT);
    } else {
        s.main_opacity = GetSpin(dlg, IDC_OPACITY_SPIN, 20, 100);
        s.main_topmost = GetCheck(dlg, IDC_TOPMOST);
        s.main_click_through = GetCheck(dlg, IDC_CLICK_THROUGH);
        s.main_locked = GetCheck(dlg, IDC_LOCK);
        s.main_offscreen = GetCheck(dlg, IDC_OFFSCREEN);
    }
}

void LookCommand(HWND dlg, PageCtx* ctx, int id, int code) {
    WindowLook& look = LookOf(ctx);
    switch (id) {
    case IDC_FONT_BTN:
        look.font_size = GetSpin(dlg, IDC_FONT_SPIN, 6, 28);
        if (PickFont(dlg, look)) {
            SetDlgItemTextW(dlg, IDC_FONT_NAME, (look.font_name + (look.font_bold ? L", Bold" : L"")).c_str());
            SendDlgItemMessageW(dlg, IDC_FONT_SPIN, UDM_SETPOS32, 0, look.font_size);
            Changed(dlg);
        }
        break;
    case IDC_LABEL_COLOR:
    case IDC_VALUE_COLOR:
    case IDC_BAR_COLOR:
    case IDC_BG_COLOR:
        if (PickColor(dlg, *SwatchColor(ctx, id))) {
            InvalidateRect(GetDlgItem(dlg, id), nullptr, TRUE);
            Changed(dlg);
        }
        break;
    case IDC_PRESET: {
        WindowLook def = ctx->page == kPageMain ? Settings::DefaultMainLook() : Settings::DefaultTaskbarLook();
        look.label_color = def.label_color;
        look.value_color = def.value_color;
        look.bar_color = def.bar_color;
        look.background_color = def.background_color;
        if (ctx->page == kPageTaskbar) {
            SetCheck(dlg, IDC_AUTO_COLORS, true);
            SetCheck(dlg, IDC_TRANSPARENT, true);
        } else {
            SendDlgItemMessageW(dlg, IDC_OPACITY_SPIN, UDM_SETPOS32, 0, Settings().main_opacity);
        }
        UpdateLookEnables(dlg, ctx);
        InvalidateRect(dlg, nullptr, TRUE);
        Changed(dlg);
        break;
    }
    case IDC_ITEMS_BTN: {
        WindowLook def = ctx->page == kPageMain ? Settings::DefaultMainLook() : Settings::DefaultTaskbarLook();
        const wchar_t* title = ctx->page == kPageMain ? L"Main Window Display Items" : L"Taskbar Window Display Items";
        if (EditDisplayItems(ctx->state->instance, dlg, title, look.items, def)) Changed(dlg);
        break;
    }
    case IDC_FONT_SIZE:
    case IDC_OFFSET:
    case IDC_OPACITY:
        if (code == EN_CHANGE) Changed(dlg);
        break;
    default:
        if (code == BN_CLICKED) {
            UpdateLookEnables(dlg, ctx);
            Changed(dlg);
        }
        break;
    }
}

// ---- General page -------------------------------------------------------------

void InitGeneralPage(HWND dlg, PageCtx* ctx) {
    const Settings& s = ctx->state->working;
    SetCheck(dlg, IDC_STARTUP, ctx->state->startup);
    SetCheck(dlg, IDC_LIVE, s.live);
    SetupSpin(dlg, IDC_POLL_SPIN, 60, 3600, s.poll_seconds);
    SetDlgItemTextW(dlg, IDC_CRED, s.credentials_path.c_str());
    SendDlgItemMessageW(dlg, IDC_CRED, EM_SETCUEBANNER, TRUE,
                        reinterpret_cast<LPARAM>(claude_usage::DefaultCredentialsPath().c_str()));
    SetupSpin(dlg, IDC_WARN_SPIN, 1, 100, s.warn_percent);
    SetupSpin(dlg, IDC_CRIT_SPIN, 1, 100, s.critical_percent);
    SetCheck(dlg, IDC_NOTIFY, s.notify);
    SetCheck(dlg, IDC_TRAY, s.show_tray_icon);
    SetCheck(dlg, IDC_PUSH, s.phone_notifications);
    SetCheck(dlg, IDC_ACCOUNTS, s.accounts_enabled);
    SetCheck(dlg, IDC_AUTO_SWITCH, s.auto_switch);
    SetupSpin(dlg, IDC_AUTO_PCT_SPIN, 50, 100, s.auto_switch_percent);
    EnableWindow(GetDlgItem(dlg, IDC_NOTIFY), s.show_tray_icon);
}

void ApplyGeneralPage(HWND dlg, PageCtx* ctx) {
    Settings& s = ctx->state->working;
    ctx->state->startup = GetCheck(dlg, IDC_STARTUP);
    s.live = GetCheck(dlg, IDC_LIVE);
    s.poll_seconds = GetSpin(dlg, IDC_POLL_SPIN, 60, 3600);
    s.credentials_path = GetText(dlg, IDC_CRED);
    s.warn_percent = GetSpin(dlg, IDC_WARN_SPIN, 1, 100);
    s.critical_percent = GetSpin(dlg, IDC_CRIT_SPIN, 1, 100);
    if (s.critical_percent < s.warn_percent) s.critical_percent = s.warn_percent;
    s.notify = GetCheck(dlg, IDC_NOTIFY);
    s.show_tray_icon = GetCheck(dlg, IDC_TRAY);
    s.phone_notifications = GetCheck(dlg, IDC_PUSH);
    s.accounts_enabled = GetCheck(dlg, IDC_ACCOUNTS);
    s.auto_switch = GetCheck(dlg, IDC_AUTO_SWITCH);
    s.auto_switch_percent = GetSpin(dlg, IDC_AUTO_PCT_SPIN, 50, 100);
}

void GeneralCommand(HWND dlg, int id, int code) {
    switch (id) {
    case IDC_CRED_BROWSE: {
        wchar_t file[MAX_PATH] = {};
        GetDlgItemTextW(dlg, IDC_CRED, file, MAX_PATH);
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = dlg;
        ofn.lpstrFilter = L"Claude Code credentials (*.json)\0*.json\0All files (*.*)\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = L"Choose Claude Code login file";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_FORCESHOWHIDDEN;
        if (GetOpenFileNameW(&ofn)) {
            SetDlgItemTextW(dlg, IDC_CRED, file);
            Changed(dlg);
        }
        break;
    }
    case IDC_TRAY:
        EnableWindow(GetDlgItem(dlg, IDC_NOTIFY), GetCheck(dlg, IDC_TRAY));
        Changed(dlg);
        break;
    case IDC_AUTO_PCT:
    case IDC_POLL:
    case IDC_CRED:
    case IDC_WARN:
    case IDC_CRIT:
        if (code == EN_CHANGE) Changed(dlg);
        break;
    default:
        if (code == BN_CLICKED) Changed(dlg);
        break;
    }
}

// ---- Page procedure -------------------------------------------------------------

INT_PTR CALLBACK PageProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<PageCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        ctx = reinterpret_cast<PageCtx*>(lp);
        ctx->hwnd = dlg;
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
        if (ctx->page == kPageGeneral)
            InitGeneralPage(dlg, ctx);
        else
            InitLookPage(dlg, ctx);
        ctx->ready = true;
        return FALSE;  // the Options window sets the focus, once every page is in place
    }
    case WM_DRAWITEM: {
        auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        COLORREF* c = ctx ? SwatchColor(ctx, static_cast<int>(di->CtlID)) : nullptr;
        if (c) {
            DrawSwatch(di, *c);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (!ctx || !ctx->ready) break;
        if (ctx->page == kPageGeneral)
            GeneralCommand(dlg, LOWORD(wp), HIWORD(wp));
        else
            LookCommand(dlg, ctx, LOWORD(wp), HIWORD(wp));
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

// ---- The scrolling Options window ----------------------------------------------------

void ApplyAll(SheetState* st) {
    for (PageCtx& c : st->pages) {
        if (!c.hwnd) continue;
        if (c.page == kPageGeneral)
            ApplyGeneralPage(c.hwnd, &c);
        else
            ApplyLookPage(c.hwnd, &c);
    }
    if (st->apply && *st->apply) (*st->apply)(st->working, st->startup);
    EnableWindow(GetDlgItem(st->host, IDC_OPT_APPLY), FALSE);
}

void UpdateScrollBar(SheetState* st) {
    RECT view;
    GetClientRect(st->scroll, &view);
    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMax = std::max(0, st->content_h - 1);
    si.nPage = static_cast<UINT>(view.bottom);
    si.nPos = st->pos;
    SetScrollInfo(st->scroll, SB_VERT, &si, TRUE);
}

void ScrollTo(SheetState* st, int pos) {
    RECT view;
    GetClientRect(st->scroll, &view);
    pos = Clamp(pos, 0, std::max(0, st->content_h - static_cast<int>(view.bottom)));
    int dy = st->pos - pos;
    if (!dy) return;
    st->pos = pos;
    ScrollWindowEx(st->scroll, 0, dy, nullptr, nullptr, nullptr, nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    UpdateScrollBar(st);
}

LRESULT CALLBACK ScrollProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SheetState* st = g_state;
    if (st && st->scroll == hwnd) {
        const int dpi = static_cast<int>(GetDpiForWindow(hwnd));
        RECT view;
        GetClientRect(hwnd, &view);
        switch (msg) {
        case WM_VSCROLL: {
            int pos = st->pos;
            const int line = MulDiv(24, dpi, 96);
            switch (LOWORD(wp)) {
            case SB_LINEUP: pos -= line; break;
            case SB_LINEDOWN: pos += line; break;
            case SB_PAGEUP: pos -= view.bottom - line; break;
            case SB_PAGEDOWN: pos += view.bottom - line; break;
            case SB_TOP: pos = 0; break;
            case SB_BOTTOM: pos = st->content_h; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: {
                SCROLLINFO si = {};
                si.cbSize = sizeof(si);
                si.fMask = SIF_TRACKPOS;
                GetScrollInfo(hwnd, SB_VERT, &si);
                pos = si.nTrackPos;
                break;
            }
            default: break;
            }
            ScrollTo(st, pos);
            return 0;
        }
        case WM_MOUSEWHEEL:  // also arrives here from the controls under the mouse
            ScrollTo(st, st->pos - MulDiv(GET_WHEEL_DELTA_WPARAM(wp), MulDiv(72, dpi, 96), WHEEL_DELTA));
            return 0;
        default:
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Keeps the control that just got the keyboard focus (Tab) in view.
void ScrollIntoView(SheetState* st, HWND focus) {
    if (!focus || !IsChild(st->scroll, focus)) return;
    RECT r, view;
    GetWindowRect(focus, &r);
    MapWindowPoints(HWND_DESKTOP, st->scroll, reinterpret_cast<POINT*>(&r), 2);
    GetClientRect(st->scroll, &view);
    const int margin = MulDiv(12, static_cast<int>(GetDpiForWindow(st->scroll)), 96);
    if (r.top < margin) ScrollTo(st, st->pos + r.top - margin);
    else if (r.bottom > view.bottom - margin) ScrollTo(st, st->pos + r.bottom - view.bottom + margin);
}

INT_PTR CALLBACK HostProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    SheetState* st = g_state;
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<SheetState*>(lp);
        g_state = st;
        st->host = dlg;
        st->scroll = GetDlgItem(dlg, IDC_OPT_SCROLL);
        HICON icon = LoadIconW(st->instance, MAKEINTRESOURCEW(IDI_APP));
        SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        const int dpi = static_cast<int>(GetDpiForWindow(dlg));
        auto scale = [dpi](int px) { return MulDiv(px, dpi, 96); };
        st->header_font = CreateUiFont(L"Segoe UI Semibold", 11, false, dpi);
        RECT view;
        GetClientRect(st->scroll, &view);

        // Taskbar first (what most people change), then the main window, then general.
        const Page order[3] = {kPageTaskbar, kPageMain, kPageGeneral};
        const int templates[3] = {IDD_PAGE_TASKBAR, IDD_PAGE_MAIN, IDD_PAGE_GENERAL};
        int y = scale(8);
        const int header_h = scale(24);
        for (int i = 0; i < 3; ++i) {
            PageCtx& c = st->pages[i];
            c.state = st;
            c.page = order[i];
            HWND page = CreateDialogParamW(st->instance, MAKEINTRESOURCEW(templates[i]), st->scroll, &PageProc,
                                           reinterpret_cast<LPARAM>(&c));
            if (!page) continue;
            wchar_t title[128] = {};
            GetWindowTextW(page, title, 128);  // the page's caption becomes its heading
            RECT client;
            GetClientRect(page, &client);
            SetWindowLongPtrW(page, GWL_STYLE, (GetWindowLongPtrW(page, GWL_STYLE) & ~WS_CAPTION) | WS_CHILD);
            SetWindowLongPtrW(page, GWL_EXSTYLE, GetWindowLongPtrW(page, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);
            const int x = std::max(0, static_cast<int>(view.right - client.right) / 2);
            st->section_y[c.page] = y - scale(8);
            HWND header = CreateWindowExW(0, L"STATIC", title, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                          x + scale(10), y, client.right - scale(10), header_h, st->scroll, nullptr,
                                          st->instance, nullptr);
            SendMessageW(header, WM_SETFONT, reinterpret_cast<WPARAM>(st->header_font), TRUE);
            y += header_h;
            SetWindowPos(page, nullptr, x, y, client.right, client.bottom,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            y += client.bottom + scale(10);
        }
        st->content_h = y;
        UpdateScrollBar(st);
        ScrollTo(st, st->section_y[st->start_page]);
        st->laid_out = true;
        return TRUE;
    }
    case WM_COMMAND:
        if (!st) break;
        switch (LOWORD(wp)) {
        case IDOK:
            ApplyAll(st);
            EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        case IDC_OPT_APPLY:
            ApplyAll(st);
            return TRUE;
        default:
            break;
        }
        break;
    case WM_DESTROY:
        if (st && st->header_font) DeleteObject(st->header_font);
        g_state = nullptr;
        break;
    default:
        break;
    }
    return FALSE;
}

// Tab moves the focus without telling anyone; follow it so the focused control stays in view.
void CALLBACK FocusFollow(HWINEVENTHOOK, DWORD, HWND hwnd, LONG id, LONG, DWORD, DWORD) {
    if (g_state && g_state->laid_out && id == OBJID_CLIENT) ScrollIntoView(g_state, hwnd);
}

}  // namespace

bool EditDisplayItems(HINSTANCE instance, HWND owner, const wchar_t* title, ItemConfig items[kItemCount],
                      const WindowLook& defaults) {
    ItemsArgs args = {title, items, &defaults};
    return DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_DISPLAY_ITEMS), owner, &ItemsProc,
                           reinterpret_cast<LPARAM>(&args)) == IDOK;
}

void Show(HINSTANCE instance, HWND owner, const Settings& current, bool start_with_windows, Page start_page,
          const ApplyFn& apply) {
    if (g_state && g_state->host && IsWindow(g_state->host)) {  // already open: go to that section
        ScrollTo(g_state, g_state->section_y[start_page]);
        SetForegroundWindow(g_state->host);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &ScrollProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_3DFACE);
        wc.lpszClassName = kScrollClass;
        registered = RegisterClassExW(&wc) != 0;
    }

    SheetState state;
    state.instance = instance;
    state.working = current;
    state.startup = start_with_windows;
    state.apply = &apply;
    state.start_page = start_page;
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, &FocusFollow,
                                         GetCurrentProcessId(), GetCurrentThreadId(), WINEVENT_OUTOFCONTEXT);
    if (DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_OPTIONS), owner, &HostProc, reinterpret_cast<LPARAM>(&state)) == -1)
        AppendAppLog(L"options: couldn't open the window (error " + std::to_wstring(GetLastError()) + L")");
    if (hook) UnhookWinEvent(hook);
    g_state = nullptr;
}

}  // namespace options
