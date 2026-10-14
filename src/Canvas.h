// Small premultiplied-ARGB drawing surface for layered windows, menu bitmaps
// and icons. Text is rendered with grayscale antialiasing and its coverage
// becomes the alpha channel, so it looks right on any background.
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>

class Canvas {
public:
    Canvas() = default;
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    bool Create(int width, int height);
    int width() const { return w_; }
    int height() const { return h_; }
    HDC dc() const { return dc_; }  // output bitmap selected; for UpdateLayeredWindow

    void Clear(COLORREF color, unsigned alpha);
    void Fill(RECT r, COLORREF color, unsigned alpha);
    void FillRounded(RECT r, int radius, COLORREF color, unsigned alpha);
    void Text(const std::wstring& s, RECT r, UINT format, HFONT font, COLORREF color, unsigned alpha);

    HBITMAP DetachBitmap();  // premultiplied 32bpp DIB, caller owns
    HICON ToIcon();          // straight-alpha icon, caller owns

private:
    void Release();

    int w_ = 0, h_ = 0;
    HDC dc_ = nullptr, mask_dc_ = nullptr;
    HBITMAP bmp_ = nullptr, mask_bmp_ = nullptr;
    HGDIOBJ old_ = nullptr, old_mask_ = nullptr;
    uint32_t* px_ = nullptr;
    uint32_t* mask_ = nullptr;
};

HFONT CreateUiFont(const std::wstring& name, int size_pt, bool bold, int dpi);
int TextWidth(HFONT font, const std::wstring& s);
int LineHeight(HFONT font);

// Segoe Fluent Icons (Windows 11) or Segoe MDL2 Assets (Windows 10) glyphs.
namespace glyph {
const wchar_t kRefresh = 0xE72C;
const wchar_t kDisplay = 0xE7F4;
const wchar_t kClose = 0xE711;
const wchar_t kSettings = 0xE713;
const wchar_t kExit = 0xF3B1;
const wchar_t kWindow = 0xE8A7;
const wchar_t kTaskbar = 0xE7F4;
}  // namespace glyph

HBITMAP CreateGlyphBitmap(wchar_t glyph, int size, COLORREF color);
HICON CreateGlyphIcon(wchar_t glyph, int size, COLORREF color);
