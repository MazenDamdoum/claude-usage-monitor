// Small premultiplied-ARGB drawing surface for layered windows, menu bitmaps
// and icons. Text is rendered with grayscale antialiasing and its coverage
// becomes the alpha channel, so it looks right on any background.
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// A small antialiased shape: coverage 0-255 per pixel, drawn in any colour.
struct Sprite {
    int w = 0, h = 0;
    std::vector<uint8_t> cover;
    double ink_w = 0, ink_h = 0;  // the size of its ink, in pixels
};

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
    // A usage bar: a faint track and the filled part (rounded pieces when radius > 0). `quarters`
    // cuts it into four with `gap`-pixel breaks, so 25, 50, 75 and 100% read at a glance.
    // `green_from` (0-1): the part of the track from there to the end is green (a friend's freed reserve).
    void Bar(RECT track, double fraction, int radius, COLORREF track_color, unsigned track_alpha, COLORREF fill_color,
             unsigned fill_alpha, bool quarters, int gap, double green_from = -1);
    void Text(const std::wstring& s, RECT r, UINT format, HFONT font, COLORREF color, unsigned alpha);
    void Draw(const Sprite& s, int x, int y, COLORREF color, unsigned alpha);  // top-left at (x, y)
    // The sprite turned by `angle` radians (clockwise) about its middle, placed at (cx, cy).
    void DrawRotated(const Sprite& s, double cx, double cy, double angle, COLORREF color, unsigned alpha);
    // An arc of a ring with round ends: centre (cx, cy), `radius` to the middle of its stroke,
    // `thickness` wide; from `start` (a fraction of a turn, clockwise from 12 o'clock) for `sweep`
    // of a turn (1 = the whole ring).
    void Arc(double cx, double cy, double radius, double thickness, double start, double sweep, COLORREF color,
             unsigned alpha);
    // `src` laid over this canvas, faded to `alpha`, its column `src_x` at this canvas's left edge.
    void Compose(const Canvas& src, int src_x, unsigned alpha);

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
// `ch` in `font_name`, `pixel_height` tall, in a `box` x `box` sprite with the middle of its ink at the
// sprite's exact centre (to a quarter pixel). It is drawn at four times the size, its ink found from the
// pixels themselves, then shrunk: glyph metrics are whole pixels and differ glyph to glyph, so a run of
// different glyphs drawn at one spot from them wobbles; these don't.
Sprite CenteredGlyph(const std::wstring& font_name, int pixel_height, wchar_t ch, int box);

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
