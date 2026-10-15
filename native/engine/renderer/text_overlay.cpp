#include "renderer/text_overlay.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"

namespace sspc {
namespace {

constexpr int kAtlasW = 1024, kAtlasH = 512;

}  // namespace

bool TextOverlay::Init(Renderer& renderer, int pixel_height) {
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = kAtlasW;
    bi.bmiHeader.biHeight = -kAtlasH;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HFONT font = CreateFontW(-pixel_height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    if (!bmp || !font || !bits) {
        if (bmp) DeleteObject(bmp);
        if (font) DeleteObject(font);
        DeleteDC(dc);
        log::Warn("overlay", "GDI font atlas could not be created: no HUD text");
        return false;
    }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    HGDIOBJ old_font = SelectObject(dc, font);
    std::memset(bits, 0, static_cast<size_t>(kAtlasW) * kAtlasH * 4);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    cell_height_ = static_cast<float>(tm.tmHeight);
    line_height_ = static_cast<float>(tm.tmHeight + tm.tmExternalLeading + 2);
    // White block for rectangles at the top-left.
    auto* px = static_cast<u32*>(bits);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) px[y * kAtlasW + x] = 0x00ffffffu;
    }
    white_u_ = 2.0f / kAtlasW;
    white_v_ = 2.0f / kAtlasH;
    int cx = 8, cy = 0;
    const int row_h = tm.tmHeight + 2;
    for (u32 cp = 32; cp < 256; ++cp) {
        if (cp >= 127 && cp < 160) continue;
        const wchar_t ch = static_cast<wchar_t>(cp);
        SIZE sz{};
        GetTextExtentPoint32W(dc, &ch, 1, &sz);
        const int w = sz.cx + 2;
        if (cx + w >= kAtlasW) {
            cx = 0;
            cy += row_h;
        }
        if (cy + row_h >= kAtlasH) break;
        TextOutW(dc, cx + 1, cy, &ch, 1);
        Glyph& g = glyphs_[cp];
        g.u0 = static_cast<float>(cx) / kAtlasW;
        g.v0 = static_cast<float>(cy) / kAtlasH;
        g.u1 = static_cast<float>(cx + w) / kAtlasW;
        g.v1 = static_cast<float>(cy + tm.tmHeight) / kAtlasH;
        g.width = static_cast<float>(w);
        g.advance = static_cast<float>(sz.cx);
        cx += w + 2;
    }
    GdiFlush();
    // Grey-scale anti-aliasing: coverage in the colour channels -> alpha of a white atlas.
    std::vector<u8> rgba(static_cast<size_t>(kAtlasW) * kAtlasH * 4);
    for (size_t i = 0; i < static_cast<size_t>(kAtlasW) * kAtlasH; ++i) {
        const u32 p = px[i];
        const u8 cov = static_cast<u8>(std::max({p & 0xff, (p >> 8) & 0xff, (p >> 16) & 0xff}));
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = (i % kAtlasW < 4 && i / kAtlasW < 4) ? 255 : cov;
    }
    SelectObject(dc, old_font);
    SelectObject(dc, old_bmp);
    DeleteObject(font);
    DeleteObject(bmp);
    DeleteDC(dc);
    atlas_ = renderer.UploadRgba(kAtlasW, kAtlasH, rgba, /*srgb=*/false, "font-atlas");
    return atlas_ != kWhiteTexture;
}

void TextOverlay::Begin(float screen_width, float screen_height) {
    screen_w_ = std::max(1.0f, screen_width);
    screen_h_ = std::max(1.0f, screen_height);
    verts_.clear();
}

void TextOverlay::Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 rgba) {
    const OverlayVertex a{x0, y0, u0, v0, rgba}, b{x1, y0, u1, v0, rgba}, c{x1, y1, u1, v1, rgba}, d{x0, y1, u0, v1, rgba};
    verts_.insert(verts_.end(), {a, b, c, c, d, a});
}

void TextOverlay::Rect(float x, float y, float w, float h, u32 rgba) {
    Quad(x, y, x + w, y + h, white_u_, white_v_, white_u_, white_v_, rgba);
}

u32 TextOverlay::NextCodepoint(std::string_view s, size_t& i) {
    const u8 c = static_cast<u8>(s[i]);
    auto cont = [&](size_t k) { return i + k < s.size() ? static_cast<u8>(s[i + k]) & 0x3f : 0u; };
    if (c < 0x80) {
        i += 1;
        return c;
    }
    if ((c & 0xe0) == 0xc0) {
        const u32 cp = (c & 0x1fu) << 6 | cont(1);
        i += 2;
        return cp;
    }
    if ((c & 0xf0) == 0xe0) {
        const u32 cp = (c & 0x0fu) << 12 | cont(1) << 6 | cont(2);
        i += 3;
        return cp;
    }
    i += (c & 0xf8) == 0xf0 ? 4 : 1;
    return '?';
}

const TextOverlay::Glyph& TextOverlay::GlyphFor(u32 cp) const {
    if (cp < 32) cp = ' ';  // line breaks inside a text: the caller wraps lines itself
    switch (cp) {
        case 0x2018:
        case 0x2019: cp = '\''; break;
        case 0x201c:
        case 0x201d: cp = '"'; break;
        case 0x2013:
        case 0x2014: cp = '-'; break;
        default: break;
    }
    if (cp < 256 && glyphs_[cp].advance > 0) return glyphs_[cp];
    return glyphs_['?'];
}

float TextOverlay::Measure(std::string_view s, float scale) const {
    float w = 0;
    for (size_t i = 0; i < s.size();) {
        const u32 cp = NextCodepoint(s, i);
        if (cp == 0x2026) {
            w += GlyphFor('.').advance * 3 * scale;
            continue;
        }
        w += GlyphFor(cp).advance * scale;
    }
    return w;
}

float TextOverlay::Text(float x, float y, std::string_view s, u32 rgba, float scale) {
    const float x_start = x;
    auto draw = [&](u32 cp) {
        const Glyph& g = GlyphFor(cp);
        if (cp > ' ') Quad(x, y, x + g.width * scale, y + cell_height_ * scale, g.u0, g.v0, g.u1, g.v1, rgba);
        x += g.advance * scale;
    };
    for (size_t i = 0; i < s.size();) {
        const u32 cp = NextCodepoint(s, i);
        if (cp == 0x2026) {
            draw('.');
            draw('.');
            draw('.');
        } else {
            draw(cp);
        }
    }
    return x - x_start;
}

void TextOverlay::Submit(Renderer& renderer) {
    if (atlas_ == kWhiteTexture) return;
    renderer.SetOverlay(atlas_, verts_);
}

}  // namespace sspc
