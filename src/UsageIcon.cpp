#include "UsageIcon.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

HICON CreateUsageIcon(int size, int percent, int severity) {
    const double kPi = 3.14159265358979323846;
    const double c = size / 2.0;
    const double r_out = size / 2.0 - 0.5;
    const double r_in = r_out * 0.52;
    const double fill = percent < 0 ? 0.0 : (percent > 100 ? 1.0 : percent / 100.0);

    struct Rgb { double r, g, b; };
    const Rgb track = {140, 140, 140};
    const Rgb arc = severity == 2 ? Rgb{230, 72, 72} : (severity == 1 ? Rgb{245, 170, 40} : Rgb{217, 119, 87});
    const double track_alpha = 0.55;

    std::vector<uint32_t> pixels(static_cast<size_t>(size) * size);
    const int ss = 4;  // 4x4 supersampling
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    double px = x + (sx + 0.5) / ss - c;
                    double py = y + (sy + 0.5) / ss - c;
                    double d = std::sqrt(px * px + py * py);
                    if (d > r_out || d < r_in) continue;
                    double angle = std::atan2(px, -py);  // 0 at 12 o'clock, clockwise
                    if (angle < 0) angle += 2 * kPi;
                    bool on_arc = angle / (2 * kPi) < fill;
                    const Rgb& col = on_arc ? arc : track;
                    double sa = on_arc ? 1.0 : track_alpha;
                    r += col.r * sa;
                    g += col.g * sa;
                    b += col.b * sa;
                    a += sa;
                }
            }
            double n = ss * ss;
            uint32_t A = static_cast<uint32_t>(a / n * 255 + 0.5);
            uint32_t R = a > 0 ? static_cast<uint32_t>(r / a + 0.5) : 0;
            uint32_t G = a > 0 ? static_cast<uint32_t>(g / a + 0.5) : 0;
            uint32_t B = a > 0 ? static_cast<uint32_t>(b / a + 0.5) : 0;
            pixels[static_cast<size_t>(y) * size + x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }

    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color) return nullptr;
    memcpy(bits, pixels.data(), pixels.size() * sizeof(uint32_t));

    std::vector<uint8_t> mask_bits(static_cast<size_t>((size + 15) / 16 * 2) * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, mask_bits.data());

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}
