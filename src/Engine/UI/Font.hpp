#pragma once

#include "Engine/RHI/Device.hpp"
#include "Engine/RHI/Heaps.hpp"
#include "Engine/RHI/Uploader.hpp"

#include <array>

namespace pulse {

// Two font faces baked into ONE atlas (so the UI renderer still binds a single SRV):
//   MONO    - JetBrains Mono (monospace), the telemetry/label face. One advance for all glyphs.
//   DISPLAY - Chakra Petch (proportional, bold), the headline/name/number face. Per-glyph advances.
// Both faces are loaded from assets/fonts/*.ttf at bake() via a private AddFontResourceEx, baked
// with GDI, supersampled, and reported at LOGICAL metrics so existing layout math is unchanged.
// The atlas alpha (R channel) is glyph coverage, blended by ui.hlsl; a white texel backs solid
// HUD primitives.
class Font {
public:
    struct Glyph { float u0 = 0, v0 = 0, u1 = 0, v1 = 0; };

    void bake(rhi::Heaps& heaps, rhi::Uploader& uploader, int pixelHeight = 18);

    uint32_t atlasSrv() const { return atlasSrv_; }

    // --- MONO (JetBrains Mono) -------------------------------------------
    const Glyph& glyph(char c) const {
        const int i = static_cast<unsigned char>(c);
        return (i >= 32 && i <= 126) ? glyphs_[i - 32] : glyphs_[0];
    }
    float cellWidth() const { return cellW_; }     // mono advance in logical px (scale 1)
    float cellHeight() const { return cellH_; }

    // --- DISPLAY (Chakra Petch, proportional) ----------------------------
    const Glyph& dGlyph(char c) const {
        const int i = static_cast<unsigned char>(c);
        return (i >= 32 && i <= 126) ? dGlyphs_[i - 32] : dGlyphs_[0];
    }
    // Logical advance (pen step) for a display glyph at scale 1.
    float dAdvance(char c) const {
        const int i = static_cast<unsigned char>(c);
        return (i >= 32 && i <= 126) ? dAdv_[i - 32] : dAdv_[0];
    }
    // The drawn cell box for a display glyph (its ink fits inside this; advance is smaller).
    float dCellWidth() const { return dCellW_; }
    float dCellHeight() const { return dCellH_; }

    float whiteU() const { return whiteU_; }
    float whiteV() const { return whiteV_; }

private:
    rhi::Texture            atlas_;
    uint32_t                atlasSrv_ = 0;
    std::array<Glyph, 95>   glyphs_{};   // mono cell rects
    std::array<Glyph, 95>   dGlyphs_{};  // display cell rects
    std::array<float, 95>   dAdv_{};     // display per-glyph logical advance
    float cellW_ = 0, cellH_ = 0;        // mono logical metrics
    float dCellW_ = 0, dCellH_ = 0;      // display logical cell box
    float whiteU_ = 0, whiteV_ = 0;
};

} // namespace pulse
