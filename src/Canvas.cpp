#include "Canvas.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// Premultiplied "source over" of a solid color with coverage a (0-255).
inline void Blend(uint32_t& dst, COLORREF c, unsigned a) {
    if (a == 0) return;
    unsigned inv = 255 - a;
    unsigned sr = GetRValue(c) * a / 255, sg = GetGValue(c) * a / 255, sb = GetBValue(c) * a / 255;
    unsigned da = dst >> 24, dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    dst = ((a + da * inv / 255) << 24) | ((sr + dr * inv / 255) << 16) | ((sg + dg * inv / 255) << 8) |
          (sb + db * inv / 255);
}

RECT Clip(RECT r, int w, int h) {
    r.left = std::max<LONG>(r.left, 0);
    r.top = std::max<LONG>(r.top, 0);
    r.right = std::min<LONG>(r.right, w);
    r.bottom = std::min<LONG>(r.bottom, h);
    return r;
}

HDC MeasureDC() {
    static HDC dc = CreateCompatibleDC(nullptr);
    return dc;
}

const wchar_t* IconFontName() {
    static const wchar_t* name = [] {
        LOGFONTW lf = {};
        lf.lfCharSet = DEFAULT_CHARSET;
        lstrcpynW(lf.lfFaceName, L"Segoe Fluent Icons", LF_FACESIZE);
        bool found = false;
        EnumFontFamiliesExW(
            MeasureDC(), &lf,
            [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) -> int {
                *reinterpret_cast<bool*>(lp) = true;
                return 0;
            },
            reinterpret_cast<LPARAM>(&found), 0);
        return found ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }();
    return name;
}

}  // namespace

Canvas::~Canvas() { Release(); }

void Canvas::Release() {
    if (mask_dc_) {
        SelectObject(mask_dc_, old_mask_);
        DeleteDC(mask_dc_);
    }
    if (dc_) {
        if (bmp_) SelectObject(dc_, old_);
        DeleteDC(dc_);
    }
    if (mask_bmp_) DeleteObject(mask_bmp_);
    if (bmp_) DeleteObject(bmp_);
    dc_ = mask_dc_ = nullptr;
    bmp_ = mask_bmp_ = nullptr;
    px_ = mask_ = nullptr;
    w_ = h_ = 0;
}

bool Canvas::Create(int width, int height) {
    Release();
    if (width <= 0 || height <= 0) return false;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    void* mask_bits = nullptr;
    bmp_ = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    mask_bmp_ = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &mask_bits, nullptr, 0);
    if (!bmp_ || !mask_bmp_) {
        Release();
        return false;
    }
    w_ = width;
    h_ = height;
    px_ = static_cast<uint32_t*>(bits);
    mask_ = static_cast<uint32_t*>(mask_bits);
    dc_ = CreateCompatibleDC(nullptr);
    mask_dc_ = CreateCompatibleDC(nullptr);
    old_ = SelectObject(dc_, bmp_);
    old_mask_ = SelectObject(mask_dc_, mask_bmp_);
    SetBkMode(mask_dc_, TRANSPARENT);
    SetTextColor(mask_dc_, RGB(255, 255, 255));
    std::fill(px_, px_ + static_cast<size_t>(w_) * h_, 0u);
    std::fill(mask_, mask_ + static_cast<size_t>(w_) * h_, 0u);
    return true;
}

void Canvas::Clear(COLORREF color, unsigned alpha) {
    uint32_t v = (alpha << 24) | ((GetRValue(color) * alpha / 255) << 16) | ((GetGValue(color) * alpha / 255) << 8) |
                 (GetBValue(color) * alpha / 255);
    std::fill(px_, px_ + static_cast<size_t>(w_) * h_, v);
}

void Canvas::Fill(RECT r, COLORREF color, unsigned alpha) {
    r = Clip(r, w_, h_);
    for (int y = r.top; y < r.bottom; ++y)
        for (int x = r.left; x < r.right; ++x) Blend(px_[y * w_ + x], color, alpha);
}

void Canvas::FillRounded(RECT r, int radius, COLORREF color, unsigned alpha) {
    RECT c = Clip(r, w_, h_);
    double rad = radius;
    for (int y = c.top; y < c.bottom; ++y) {
        for (int x = c.left; x < c.right; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double cx = std::min(std::max(px, r.left + rad), r.right - rad);
            double cy = std::min(std::max(py, r.top + rad), r.bottom - rad);
            double d = std::hypot(px - cx, py - cy);
            double cover = std::min(1.0, std::max(0.0, rad + 0.5 - d));
            if (rad <= 0) cover = 1.0;
            Blend(px_[y * w_ + x], color, static_cast<unsigned>(alpha * cover + 0.5));
        }
    }
}

void Canvas::Text(const std::wstring& s, RECT r, UINT format, HFONT font, COLORREF color, unsigned alpha) {
    if (s.empty()) return;
    r = Clip(r, w_, h_);
    if (r.right <= r.left || r.bottom <= r.top) return;
    HGDIOBJ old_font = SelectObject(mask_dc_, font);
    RECT draw = r;
    DrawTextW(mask_dc_, s.c_str(), static_cast<int>(s.size()), &draw,
              format | DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
    GdiFlush();
    SelectObject(mask_dc_, old_font);
    for (int y = r.top; y < r.bottom; ++y) {
        for (int x = r.left; x < r.right; ++x) {
            uint32_t& m = mask_[y * w_ + x];
            if (!m) continue;
            unsigned cover = std::max({m & 0xFF, (m >> 8) & 0xFF, (m >> 16) & 0xFF});
            m = 0;
            Blend(px_[y * w_ + x], color, cover * alpha / 255);
        }
    }
}

HBITMAP Canvas::DetachBitmap() {
    if (!bmp_) return nullptr;
    SelectObject(dc_, old_);
    HBITMAP b = bmp_;
    bmp_ = nullptr;
    Release();
    return b;
}

HICON Canvas::ToIcon() {
    if (!bmp_) return nullptr;
    // Icons want straight (non-premultiplied) alpha.
    for (size_t i = 0, n = static_cast<size_t>(w_) * h_; i < n; ++i) {
        uint32_t p = px_[i];
        unsigned a = p >> 24;
        if (a == 0 || a == 255) continue;
        unsigned r = ((p >> 16) & 0xFF) * 255 / a, g = ((p >> 8) & 0xFF) * 255 / a, b = (p & 0xFF) * 255 / a;
        px_[i] = (a << 24) | (std::min(r, 255u) << 16) | (std::min(g, 255u) << 8) | std::min(b, 255u);
    }
    std::vector<uint8_t> mask_bits(static_cast<size_t>((w_ + 15) / 16 * 2) * h_, 0);
    HBITMAP mask = CreateBitmap(w_, h_, 1, 1, mask_bits.data());
    SelectObject(dc_, old_);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = bmp_;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    SelectObject(dc_, bmp_);
    return icon;
}

HFONT CreateUiFont(const std::wstring& name, int size_pt, bool bold, int dpi) {
    LOGFONTW lf = {};
    lf.lfHeight = -MulDiv(size_pt, dpi, 72);
    lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;  // grayscale AA: coverage doubles as alpha
    lstrcpynW(lf.lfFaceName, name.c_str(), LF_FACESIZE);
    return CreateFontIndirectW(&lf);
}

int TextWidth(HFONT font, const std::wstring& s) {
    HDC dc = MeasureDC();
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

int LineHeight(HFONT font) {
    HDC dc = MeasureDC();
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    return tm.tmHeight;
}

namespace {
bool DrawGlyph(Canvas& canvas, wchar_t glyph, int size, COLORREF color) {
    if (!canvas.Create(size, size)) return false;
    LOGFONTW lf = {};
    lf.lfHeight = -size;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lstrcpynW(lf.lfFaceName, IconFontName(), LF_FACESIZE);
    HFONT font = CreateFontIndirectW(&lf);
    canvas.Text(std::wstring(1, glyph), RECT{0, 0, size, size}, DT_CENTER, font, color, 255);
    DeleteObject(font);
    return true;
}
}  // namespace

HBITMAP CreateGlyphBitmap(wchar_t glyph, int size, COLORREF color) {
    Canvas canvas;
    if (!DrawGlyph(canvas, glyph, size, color)) return nullptr;
    return canvas.DetachBitmap();
}

HICON CreateGlyphIcon(wchar_t glyph, int size, COLORREF color) {
    Canvas canvas;
    if (!DrawGlyph(canvas, glyph, size, color)) return nullptr;
    return canvas.ToIcon();
}
