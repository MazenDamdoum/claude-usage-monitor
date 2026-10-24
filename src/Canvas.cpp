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

void Canvas::Bar(RECT track, double fraction, int radius, COLORREF track_color, unsigned track_alpha,
                 COLORREF fill_color, unsigned fill_alpha, bool quarters, int gap, double green_from) {
    const int w = track.right - track.left;
    if (w <= 0) return;
    const int pieces = quarters && w >= 16 ? 4 : 1;
    fraction = std::min(1.0, std::max(0.0, fraction));
    const int fill_to = track.left + static_cast<int>(w * fraction + 0.5);
    for (int i = 0; i < pieces; ++i) {
        RECT seg = track;
        seg.left = track.left + w * i / pieces;
        seg.right = track.left + w * (i + 1) / pieces - (i + 1 < pieces ? gap : 0);
        if (seg.right <= seg.left) continue;
        int r = std::min<int>(radius, (seg.right - seg.left) / 2);
        if (r > 0) FillRounded(seg, r, track_color, track_alpha);
        else Fill(seg, track_color, track_alpha);
        if (green_from >= 0) {  // the freed reserve: green from there (or what's used, if more) to the end
            RECT g = seg;
            g.left = std::max<LONG>(seg.left, track.left + static_cast<LONG>(w * std::max(green_from, fraction) + 0.5));
            if (g.right > g.left) {
                if (r > 0) FillRounded(g, std::min<int>(r, (g.right - g.left) / 2), RGB(52, 199, 89), 235);
                else Fill(g, RGB(52, 199, 89), 235);
            }
        }
        if (fraction <= 0 || fill_to <= seg.left) continue;
        RECT part = seg;
        part.right = std::min<LONG>(seg.right, fill_to);
        if (r > 0) {
            part.right = std::max<LONG>(part.right, std::min<LONG>(seg.right, part.left + 2 * r));  // a dot, not a sliver
            FillRounded(part, r, fill_color, fill_alpha);
        } else {
            Fill(part, fill_color, fill_alpha);
        }
    }
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

void Canvas::Arc(double cx, double cy, double radius, double thickness, double start, double sweep, COLORREF color,
                 unsigned alpha) {
    if (sweep <= 0) return;
    const double two_pi = 6.283185307179586, half = thickness / 2;
    const bool whole = sweep >= 1;
    const double a0 = start * two_pi, span = std::min(sweep, 1.0) * two_pi;
    const double e0x = cx + radius * std::sin(a0), e0y = cy - radius * std::cos(a0);
    const double e1x = cx + radius * std::sin(a0 + span), e1y = cy - radius * std::cos(a0 + span);
    auto inside = [&](double x, double y) {
        const double dx = x - cx, dy = y - cy;
        if (std::fabs(std::hypot(dx, dy) - radius) <= half) {
            if (whole) return true;
            double a = std::atan2(dx, -dy);  // clockwise from 12 o'clock
            if (std::fmod(a - a0 + 3 * two_pi, two_pi) <= span) return true;
        }
        return !whole && (std::hypot(x - e0x, y - e0y) <= half || std::hypot(x - e1x, y - e1y) <= half);
    };
    const int reach = static_cast<int>(std::ceil(radius + half)) + 1;
    for (int y = std::max(0, static_cast<int>(cy) - reach); y < std::min(h_, static_cast<int>(cy) + reach + 1); ++y) {
        for (int x = std::max(0, static_cast<int>(cx) - reach); x < std::min(w_, static_cast<int>(cx) + reach + 1); ++x) {
            int hits = 0;  // 4 x 4 samples a pixel: smooth edges
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) hits += inside(x + (sx + 0.5) / 4, y + (sy + 0.5) / 4);
            if (hits) Blend(px_[y * w_ + x], color, alpha * hits / 16);
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

void Canvas::Draw(const Sprite& s, int x, int y, COLORREF color, unsigned alpha) {
    for (int sy = 0; sy < s.h; ++sy) {
        int py = y + sy;
        if (py < 0 || py >= h_) continue;
        for (int sx = 0; sx < s.w; ++sx) {
            int px = x + sx;
            unsigned cover = s.cover[sy * s.w + sx];
            if (px < 0 || px >= w_ || !cover) continue;
            Blend(px_[py * w_ + px], color, cover * alpha / 255);
        }
    }
}

void Canvas::DrawRotated(const Sprite& s, double cx, double cy, double angle, COLORREF color, unsigned alpha) {
    const double c = std::cos(angle), sn = std::sin(angle), half = s.w / 2.0;
    const int reach = static_cast<int>(std::ceil(half * 1.4143)) + 1;
    auto cover = [&](int x, int y) -> double {
        return x < 0 || y < 0 || x >= s.w || y >= s.h ? 0.0 : s.cover[y * s.w + x];
    };
    for (int y = static_cast<int>(std::floor(cy)) - reach; y <= static_cast<int>(std::ceil(cy)) + reach; ++y) {
        if (y < 0 || y >= h_) continue;
        for (int x = static_cast<int>(std::floor(cx)) - reach; x <= static_cast<int>(std::ceil(cx)) + reach; ++x) {
            if (x < 0 || x >= w_) continue;
            const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            const double sx = c * dx + sn * dy + half - 0.5, sy = -sn * dx + c * dy + half - 0.5;  // turned back
            const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
            const double fx = sx - x0, fy = sy - y0;
            const double v = cover(x0, y0) * (1 - fx) * (1 - fy) + cover(x0 + 1, y0) * fx * (1 - fy) +
                             cover(x0, y0 + 1) * (1 - fx) * fy + cover(x0 + 1, y0 + 1) * fx * fy;
            if (v >= 1) Blend(px_[y * w_ + x], color, static_cast<unsigned>(v * alpha / 255 + 0.5));
        }
    }
}

void Canvas::Compose(const Canvas& src, int src_x, unsigned alpha) {
    if (!alpha) return;
    for (int y = 0; y < std::min(h_, src.h_); ++y) {
        for (int x = 0; x < w_; ++x) {
            const int sx = x + src_x;
            if (sx < 0 || sx >= src.w_) continue;
            const uint32_t s = src.px_[y * src.w_ + sx];
            const unsigned sa = (s >> 24) * alpha / 255;
            if (!sa) continue;
            const unsigned sr = ((s >> 16) & 0xFF) * alpha / 255, sg = ((s >> 8) & 0xFF) * alpha / 255,
                           sb = (s & 0xFF) * alpha / 255;
            uint32_t& d = px_[y * w_ + x];
            const unsigned inv = 255 - sa;
            d = ((sa + (d >> 24) * inv / 255) << 24) | ((sr + ((d >> 16) & 0xFF) * inv / 255) << 16) |
                ((sg + ((d >> 8) & 0xFF) * inv / 255) << 8) | (sb + (d & 0xFF) * inv / 255);
        }
    }
}

Sprite CenteredGlyph(const std::wstring& font_name, int pixel_height, wchar_t ch, int box) {
    const int k = 4;  // supersampling
    Sprite out;
    out.w = out.h = box;
    out.cover.assign(static_cast<size_t>(box) * box, 0);
    HFONT font = CreateFontW(-pixel_height * k, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, font_name.c_str());
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old_font = SelectObject(dc, font);
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    SIZE size = {};
    GetTextExtentPoint32W(dc, &ch, 1, &size);
    const int margin = tm.tmHeight / 2;
    const int bw = size.cx + 2 * margin, bh = tm.tmHeight + 2 * margin;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = bw;
    bi.bmiHeader.biHeight = -bh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HGDIOBJ old_bmp = SelectObject(dc, bmp);
        std::fill_n(static_cast<uint32_t*>(bits), static_cast<size_t>(bw) * bh, 0u);
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, TRANSPARENT);
        TextOutW(dc, margin, margin, &ch, 1);
        GdiFlush();
        const uint32_t* p = static_cast<const uint32_t*>(bits);
        auto at = [&](int x, int y) -> unsigned {
            if (x < 0 || y < 0 || x >= bw || y >= bh) return 0;
            uint32_t v = p[y * bw + x];
            return std::max({v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF});
        };
        int minx = bw, maxx = -1, miny = bh, maxy = -1;
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x)
                if (at(x, y)) {
                    minx = std::min(minx, x);
                    maxx = std::max(maxx, x);
                    miny = std::min(miny, y);
                    maxy = std::max(maxy, y);
                }
        if (maxx >= 0) {
            out.ink_w = (maxx - minx + 1) / static_cast<double>(k);
            out.ink_h = (maxy - miny + 1) / static_cast<double>(k);
            // The ink's middle lands on the sprite's middle.
            const int ox = static_cast<int>(std::lround((minx + maxx + 1) / 2.0 - box * k / 2.0));
            const int oy = static_cast<int>(std::lround((miny + maxy + 1) / 2.0 - box * k / 2.0));
            for (int fy = 0; fy < box; ++fy)
                for (int fx = 0; fx < box; ++fx) {
                    unsigned sum = 0;
                    for (int j = 0; j < k; ++j)
                        for (int i = 0; i < k; ++i) sum += at(ox + fx * k + i, oy + fy * k + j);
                    out.cover[fy * box + fx] = static_cast<uint8_t>(sum / (k * k));
                }
        }
        SelectObject(dc, old_bmp);
        DeleteObject(bmp);
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    return out;
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
