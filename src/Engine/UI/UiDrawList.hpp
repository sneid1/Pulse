#pragma once

#include "Engine/SceneFrame.hpp"
#include "Engine/UI/Font.hpp"
#include "Engine/UI/Color.hpp"

#include <string_view>
#include <vector>

namespace pulse {

// Builds a screen-space HUD as a UiVertex triangle list (rects, lines, triangles,
// glyph runs). The game fills one of these each frame and points SceneFrame::ui at
// vertices(). Coordinates are pixels with origin at the top-left.
class UiDrawList {
public:
    explicit UiDrawList(const Font& font) : font_(&font) {}

    void reset() { verts_.clear(); }

    void rect(float x, float y, float w, float h, uint32_t color);
    void line(float x0, float y0, float x1, float y1, float width, uint32_t color);
    void triangle(float ax, float ay, float bx, float by, float cx, float cy, uint32_t color);
    // Fill a convex polygon (n >= 3 hull points, xy = x0,y0,x1,y1,...) with a 1px AA
    // feather on the OUTER perimeter only - so chamfered panels, the title shard, and
    // diamonds get smooth diagonal silhouettes without seaming their internal fan edges.
    void convexFill(const float* xy, int n, uint32_t color);
    void text(float x, float y, std::string_view s, uint32_t color, float scale = 1.0f);
    void textRight(float x, float y, std::string_view s, uint32_t color, float scale = 1.0f);
    void textCentered(float cx, float y, std::string_view s, uint32_t color, float scale = 1.0f);
    // Letter-spaced text (manual tracking) for titles + emphasis on a bitmap font.
    void textTracked(float x, float y, std::string_view s, uint32_t color, float scale, float tracking);

    // DISPLAY face (Chakra Petch, proportional) - headlines / names / big numbers. Per-glyph advance.
    void textD(float x, float y, std::string_view s, uint32_t color, float scale = 1.0f);
    void textDRight(float x, float y, std::string_view s, uint32_t color, float scale = 1.0f);
    void textDCentered(float cx, float y, std::string_view s, uint32_t color, float scale = 1.0f);
    float textDWidth(std::string_view s, float scale = 1.0f) const;
    float dLineHeight(float scale = 1.0f) const { return font_->dCellHeight() * scale; }

    // Composite helpers (a small reusable kit so HUD/menus share one visual language).
    void rectOutline(float x, float y, float w, float h, uint32_t color, float t = 1.0f);
    void panel(float x, float y, float w, float h, uint32_t fill, uint32_t border, float t = 2.0f);
    void bar(float x, float y, float w, float h, float frac, uint32_t track, uint32_t fill);
    // Vertical gradient fill (top to bottom over `bands` lerped strips) for panel depth.
    void gradientV(float x, float y, float w, float h, uint32_t top, uint32_t bottom, int bands = 10);

    float textWidth(std::string_view s, float scale = 1.0f) const { return s.size() * font_->cellWidth() * scale; }
    float textTrackedWidth(std::string_view s, float scale, float tracking) const {
        return s.empty() ? 0.0f : (s.size() * font_->cellWidth() * scale + (s.size() - 1) * tracking);
    }
    float lineHeight(float scale = 1.0f) const { return font_->cellHeight() * scale; }

    const std::vector<UiVertex>& vertices() const { return verts_; }

private:
    void vert(float x, float y, float u, float v, uint32_t c) { verts_.push_back({ x, y, u, v, c }); }
    void solidTri(float ax, float ay, float bx, float by, float cx, float cy, uint32_t c);
    // A 1px alpha-feather quad along an edge (inner = full colour, outer = alpha 0) for
    // analytic anti-aliasing of diagonal line edges.
    void featherEdge(float ix0, float iy0, float ix1, float iy1, float nx, float ny, uint32_t color);
    void solidQuad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c);

    const Font* font_;
    std::vector<UiVertex> verts_;
};

} // namespace pulse
