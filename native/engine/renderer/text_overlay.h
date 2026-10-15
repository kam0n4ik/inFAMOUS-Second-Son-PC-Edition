// SecondSonPC native runtime - screen text and panels (HUD, debug view).
//
// The glyph atlas is rasterised once at start-up with the system font renderer (GDI, gdi32.dll) for
// code points 32..255; text is UTF-8 (other code points are drawn as '?', a few punctuation marks are
// mapped). Quads are submitted to Renderer::SetOverlay in pixels.
#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "core/types.h"
#include "renderer/renderer.h"

namespace sspc {

constexpr u32 Rgba(u8 r, u8 g, u8 b, u8 a = 255) { return u32{r} | u32{g} << 8 | u32{b} << 16 | u32{a} << 24; }

class TextOverlay {
public:
    bool Init(Renderer& renderer, int pixel_height = 22);
    bool Ready() const { return atlas_ != kWhiteTexture; }

    void Begin(float screen_width, float screen_height);
    void Rect(float x, float y, float w, float h, u32 rgba);
    // Returns the advance width in pixels.
    float Text(float x, float y, std::string_view utf8, u32 rgba, float scale = 1.0f);
    float Measure(std::string_view utf8, float scale = 1.0f) const;
    float LineHeight(float scale = 1.0f) const { return line_height_ * scale; }
    float ScreenWidth() const { return screen_w_; }
    float ScreenHeight() const { return screen_h_; }
    void Submit(Renderer& renderer);

private:
    struct Glyph {
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        float width = 0, advance = 0;
    };
    void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 rgba);
    static u32 NextCodepoint(std::string_view s, size_t& i);
    const Glyph& GlyphFor(u32 cp) const;

    TextureHandle atlas_ = kWhiteTexture;
    std::array<Glyph, 256> glyphs_{};
    float white_u_ = 0, white_v_ = 0;
    float line_height_ = 22;
    float cell_height_ = 22;
    float screen_w_ = 1, screen_h_ = 1;
    std::vector<OverlayVertex> verts_;
};

}  // namespace sspc
