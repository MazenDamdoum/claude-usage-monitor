#include "OptionsDialog.h"

#include <commctrl.h>
#include <commdlg.h>

#include "Canvas.h"
#include "UsageClient.h"
#include "resource.h"

namespace options {
namespace {

HWND g_sheet = nullptr;

// Shared custom colors for every color picker: the app palette first.
COLORREF g_custom_colors[16] = {
    RGB(217, 119, 87), RGB(255, 255, 255), RGB(190, 190, 190), RGB(128, 128, 128),
    RGB(64, 64, 64),   RGB(24, 24, 24),    RGB(0, 0, 0),       RGB(255, 179, 64),
    RGB(255, 107, 107), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
    RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
};

struct SheetState {
    HINSTANCE instance = nullptr;
    Settings working;
    bool startup = false;
    const ApplyFn* apply = nullptr;
};

struct PageCtx {
    SheetState* state = nullptr;
    Page page = kPageGeneral;
    bool ready = false;  // ignore change notifications during WM_INITDIALOG
};

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
    if (ctx && ctx->ready) PropSheet_Changed(GetParent(page), page);
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
}

void InitLookPage(HWND dlg, PageCtx* ctx) {
    const Settings& s = ctx->state->working;
    const WindowLook& look = LookOf(ctx);
    SetDlgItemTextW(dlg, IDC_FONT_NAME, (look.font_name + (look.font_bold ? L", Bold" : L"")).c_str());
    SetupSpin(dlg, IDC_FONT_SPIN, 6, 28, look.font_size);
    SetCheck(dlg, IDC_SHOW_RESET, look.show_reset);
    SetCheck(dlg, IDC_SHOW_BARS, look.show_bars);
    SetCheck(dlg, IDC_SPACE, look.percent_space);
    SetCheck(dlg, IDC_HIDE_PERCENT, look.hide_percent);
    SetCheck(dlg, IDC_TOOLTIP, look.tooltip);
    if (ctx->page == kPageTaskbar) {
        SetCheck(dlg, IDC_TRANSPARENT, s.taskbar_transparent);
        SetCheck(dlg, IDC_AUTO_COLORS, s.taskbar_auto_colors);
        SetCheck(dlg, IDC_RIGHT_ALIGN, s.taskbar_right_align);
        SetCheck(dlg, IDC_HORIZONTAL, s.taskbar_horizontal);
        SetupSpin(dlg, IDC_OFFSET_SPIN, -2000, 2000, s.taskbar_offset_x);
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
    look.percent_space = GetCheck(dlg, IDC_SPACE);
    look.hide_percent = GetCheck(dlg, IDC_HIDE_PERCENT);
    look.tooltip = GetCheck(dlg, IDC_TOOLTIP);
    if (ctx->page == kPageTaskbar) {
        s.taskbar_transparent = GetCheck(dlg, IDC_TRANSPARENT);
        s.taskbar_auto_colors = GetCheck(dlg, IDC_AUTO_COLORS);
        s.taskbar_right_align = GetCheck(dlg, IDC_RIGHT_ALIGN);
        s.taskbar_horizontal = GetCheck(dlg, IDC_HORIZONTAL);
        s.taskbar_offset_x = GetSpin(dlg, IDC_OFFSET_SPIN, -2000, 2000);
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
    SetupSpin(dlg, IDC_POLL_SPIN, 60, 3600, s.poll_seconds);
    SetDlgItemTextW(dlg, IDC_CRED, s.credentials_path.c_str());
    SendDlgItemMessageW(dlg, IDC_CRED, EM_SETCUEBANNER, TRUE,
                        reinterpret_cast<LPARAM>(claude_usage::DefaultCredentialsPath().c_str()));
    SetupSpin(dlg, IDC_WARN_SPIN, 1, 100, s.warn_percent);
    SetupSpin(dlg, IDC_CRIT_SPIN, 1, 100, s.critical_percent);
    SetCheck(dlg, IDC_NOTIFY, s.notify);
    SetCheck(dlg, IDC_TRAY, s.show_tray_icon);
    EnableWindow(GetDlgItem(dlg, IDC_NOTIFY), s.show_tray_icon);
}

void ApplyGeneralPage(HWND dlg, PageCtx* ctx) {
    Settings& s = ctx->state->working;
    ctx->state->startup = GetCheck(dlg, IDC_STARTUP);
    s.poll_seconds = GetSpin(dlg, IDC_POLL_SPIN, 60, 3600);
    s.credentials_path = GetText(dlg, IDC_CRED);
    s.warn_percent = GetSpin(dlg, IDC_WARN_SPIN, 1, 100);
    s.critical_percent = GetSpin(dlg, IDC_CRIT_SPIN, 1, 100);
    if (s.critical_percent < s.warn_percent) s.critical_percent = s.warn_percent;
    s.notify = GetCheck(dlg, IDC_NOTIFY);
    s.show_tray_icon = GetCheck(dlg, IDC_TRAY);
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
        ctx = reinterpret_cast<PageCtx*>(reinterpret_cast<PROPSHEETPAGEW*>(lp)->lParam);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
        if (ctx->page == kPageGeneral)
            InitGeneralPage(dlg, ctx);
        else
            InitLookPage(dlg, ctx);
        ctx->ready = true;
        return TRUE;
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
    case WM_NOTIFY:
        if (ctx && reinterpret_cast<NMHDR*>(lp)->code == PSN_APPLY) {
            if (ctx->page == kPageGeneral)
                ApplyGeneralPage(dlg, ctx);
            else
                ApplyLookPage(dlg, ctx);
            if (ctx->state->apply && *ctx->state->apply) (*ctx->state->apply)(ctx->state->working, ctx->state->startup);
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

HIMAGELIST g_tab_images = nullptr;

// The sheet's own tab image list drops alpha, so install a 32-bit one.
int CALLBACK SheetCallback(HWND sheet, UINT msg, LPARAM) {
    if (msg != PSCB_INITIALIZED) return 0;
    g_sheet = sheet;
    const int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(sheet));
    const wchar_t glyphs[3] = {glyph::kWindow, glyph::kTaskbar, glyph::kSettings};
    g_tab_images = ImageList_Create(size, size, ILC_COLOR32, 3, 0);
    for (wchar_t g : glyphs) {
        HICON icon = CreateGlyphIcon(g, size, RGB(64, 64, 64));
        ImageList_AddIcon(g_tab_images, icon);
        DestroyIcon(icon);
    }
    HWND tab = PropSheet_GetTabControl(sheet);
    TabCtrl_SetImageList(tab, g_tab_images);
    for (int i = 0; i < 3; ++i) {
        TCITEMW item = {};
        item.mask = TCIF_IMAGE;
        item.iImage = i;
        SendMessageW(tab, TCM_SETITEMW, i, reinterpret_cast<LPARAM>(&item));
    }
    return 0;
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
    if (g_sheet && IsWindow(g_sheet)) {
        PropSheet_SetCurSel(g_sheet, nullptr, start_page);
        SetForegroundWindow(g_sheet);
        return;
    }

    SheetState state;
    state.instance = instance;
    state.working = current;
    state.startup = start_with_windows;
    state.apply = &apply;

    PageCtx ctx[3] = {{&state, kPageMain}, {&state, kPageTaskbar}, {&state, kPageGeneral}};
    const int dialogs[3] = {IDD_PAGE_MAIN, IDD_PAGE_TASKBAR, IDD_PAGE_GENERAL};
    PROPSHEETPAGEW pages[3] = {};
    for (int i = 0; i < 3; ++i) {
        pages[i].dwSize = sizeof(PROPSHEETPAGEW);
        pages[i].hInstance = instance;
        pages[i].pszTemplate = MAKEINTRESOURCEW(dialogs[i]);
        pages[i].pfnDlgProc = &PageProc;
        pages[i].lParam = reinterpret_cast<LPARAM>(&ctx[i]);
    }

    PROPSHEETHEADERW header = {};
    header.dwSize = sizeof(header);
    header.dwFlags = PSH_PROPSHEETPAGE | PSH_NOCONTEXTHELP | PSH_USEHICON | PSH_USECALLBACK;
    header.hwndParent = owner;
    header.hInstance = instance;
    header.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    header.pszCaption = L"Option Settings";
    header.nPages = 3;
    header.nStartPage = start_page;
    header.ppsp = pages;
    header.pfnCallback = &SheetCallback;
    PropertySheetW(&header);
    g_sheet = nullptr;
    if (g_tab_images) ImageList_Destroy(g_tab_images);
    g_tab_images = nullptr;
}

}  // namespace options
