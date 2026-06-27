#include "Engine/UI/UiDrawList.hpp"

#include <cmath>

namespace pulse {

void UiDrawList::solidTri(float ax, float ay, float bx, float by, float cx, float cy, uint32_t c) {
    const float u = font_->whiteU(), v = font_->whiteV();
    vert(ax, ay, u, v, c);
    vert(bx, by, u, v, c);
    vert(cx, cy, u, v, c);
}

void UiDrawList::solidQuad(float x0, float y0, float x1, float y1,
                           float x2, float y2, float x3, float y3, uint32_t c) {
    solidTri(x0, y0, x1, y1, x2, y2, c);
    solidTri(x0, y0, x2, y2, x3, y3, c);
}

void UiDrawList::rect(float x, float y, float w, float h, uint32_t color) {
    solidQuad(x, y, x + w, y, x + w, y + h, x, y + h, color);
}

// Edge anti-aliasing: a ~1px feather quad from an inner edge (full colour) to an
// outer edge (same rgb, alpha 0). The rasterizer interpolates the vertex alpha and
// ui.hlsl multiplies it into coverage, so diagonal UI edges (chamfers, the pulse
// waveform, focus rings, chevrons) get a smooth 1px falloff instead of stair-steps.
namespace { constexpr float kAaFeather = 1.0f; }

void UiDrawList::featherEdge(float ix0, float iy0, float ix1, float iy1,
                             float nx, float ny, uint32_t color) {
    const float u = font_->whiteU(), v = font_->whiteV();
    const uint32_t clear = color & 0x00FFFFFFu;               // same rgb, alpha 0
    const float ox0 = ix0 + nx * kAaFeather, oy0 = iy0 + ny * kAaFeather;
    const float ox1 = ix1 + nx * kAaFeather, oy1 = iy1 + ny * kAaFeather;
    vert(ix0, iy0, u, v, color); vert(ix1, iy1, u, v, color); vert(ox1, oy1, u, v, clear);
    vert(ix0, iy0, u, v, color); vert(ox1, oy1, u, v, clear); vert(ox0, oy0, u, v, clear);
}

void UiDrawList::triangle(float ax, float ay, float bx, float by, float cx, float cy, uint32_t color) {
    // Plain fill (no per-edge feather): a lone triangle's caller may share edges with a
    // neighbour. Convex shapes that need smooth silhouettes use convexFill instead.
    solidTri(ax, ay, bx, by, cx, cy, color);
}

void UiDrawList::convexFill(const float* xy, int n, uint32_t color) {
    if (n < 3) return;
    const float u = font_->whiteU(), v = font_->whiteV();
    // fill as a triangle fan from vertex 0
    for (int i = 1; i + 1 < n; ++i) {
        vert(xy[0], xy[1], u, v, color);
        vert(xy[2 * i], xy[2 * i + 1], u, v, color);
        vert(xy[2 * i + 2], xy[2 * i + 3], u, v, color);
    }
    // centroid (to orient each edge feather outward)
    float gx = 0.0f, gy = 0.0f;
    for (int i = 0; i < n; ++i) { gx += xy[2 * i]; gy += xy[2 * i + 1]; }
    gx /= static_cast<float>(n); gy /= static_cast<float>(n);
    // feather only the perimeter edges (the fan's internal spokes are left untouched)
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        const float ax = xy[2 * i], ay = xy[2 * i + 1], bx = xy[2 * j], by = xy[2 * j + 1];
        float dx = bx - ax, dy = by - ay;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4f) continue;
        dx /= len; dy /= len;
        float nx = -dy, ny = dx;
        const float mx = (ax + bx) * 0.5f, my = (ay + by) * 0.5f;
        if (nx * (mx - gx) + ny * (my - gy) < 0.0f) { nx = -nx; ny = -ny; }
        featherEdge(ax, ay, bx, by, nx, ny, color);
    }
}

void UiDrawList::line(float x0, float y0, float x1, float y1, float width, uint32_t color) {
    float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    dx /= len; dy /= len;
    const float nx = -dy, ny = dx;                            // unit normal
    const float hw = width * 0.5f;
    const float px = nx * hw, py = ny * hw;
    solidQuad(x0 + px, y0 + py, x1 + px, y1 + py, x1 - px, y1 - py, x0 - px, y0 - py, color);
    featherEdge(x0 + px, y0 + py, x1 + px, y1 + py,  nx,  ny, color);   // + side
    featherEdge(x0 - px, y0 - py, x1 - px, y1 - py, -nx, -ny, color);   // - side
}

void UiDrawList::text(float x, float y, std::string_view s, uint32_t color, float scale) {
    const float gw = font_->cellWidth() * scale;
    const float gh = font_->cellHeight() * scale;
    float penX = x;
    for (char ch : s) {
        if (ch == '\n') { penX = x; y += gh; continue; }
        if (ch != ' ') {
            const Font::Glyph& g = font_->glyph(ch);
            vert(penX,      y,      g.u0, g.v0, color);
            vert(penX + gw, y,      g.u1, g.v0, color);
            vert(penX + gw, y + gh, g.u1, g.v1, color);
            vert(penX,      y,      g.u0, g.v0, color);
            vert(penX + gw, y + gh, g.u1, g.v1, color);
            vert(penX,      y + gh, g.u0, g.v1, color);
        }
        penX += gw;
    }
}

void UiDrawList::textRight(float x, float y, std::string_view s, uint32_t color, float scale) {
    text(x - textWidth(s, scale), y, s, color, scale);
}

void UiDrawList::textCentered(float cx, float y, std::string_view s, uint32_t color, float scale) {
    text(cx - textWidth(s, scale) * 0.5f, y, s, color, scale);
}

void UiDrawList::rectOutline(float x, float y, float w, float h, uint32_t color, float t) {
    rect(x, y, w, t, color);                 // top
    rect(x, y + h - t, w, t, color);         // bottom
    rect(x, y, t, h, color);                 // left
    rect(x + w - t, y, t, h, color);         // right
}

void UiDrawList::panel(float x, float y, float w, float h, uint32_t fill, uint32_t border, float t) {
    rect(x, y, w, h, fill);
    rectOutline(x, y, w, h, border, t);
}

void UiDrawList::bar(float x, float y, float w, float h, float frac, uint32_t track, uint32_t fill) {
    frac = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
    rect(x, y, w, h, track);
    if (frac > 0.0f) rect(x, y, w * frac, h, fill);
}

void UiDrawList::gradientV(float x, float y, float w, float h, uint32_t top, uint32_t bottom, int bands) {
    if (bands < 1) bands = 1;
    const float bh = h / static_cast<float>(bands);
    for (int i = 0; i < bands; ++i) {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(bands);
        rect(x, y + bh * static_cast<float>(i), w, bh + 1.0f, lerpColor(top, bottom, t));
    }
}

void UiDrawList::textTracked(float x, float y, std::string_view s, uint32_t color, float scale, float tracking) {
    const float gw = font_->cellWidth() * scale;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != ' ') text(x, y, std::string_view(&s[i], 1), color, scale);
        x += gw + tracking;
    }
}

// --- DISPLAY face (Chakra Petch, proportional): per-glyph advance, cell-quad art ----
float UiDrawList::textDWidth(std::string_view s, float scale) const {
    float w = 0.0f;
    for (char ch : s) w += font_->dAdvance(ch);
    return w * scale;
}

void UiDrawList::textD(float x, float y, std::string_view s, uint32_t color, float scale) {
    const float gw = font_->dCellWidth() * scale;   // drawn cell box (art fits inside)
    const float gh = font_->dCellHeight() * scale;
    float penX = x;
    for (char ch : s) {
        if (ch == '\n') { penX = x; y += gh; continue; }
        if (ch != ' ') {
            const Font::Glyph& g = font_->dGlyph(ch);
            vert(penX,      y,      g.u0, g.v0, color);
            vert(penX + gw, y,      g.u1, g.v0, color);
            vert(penX + gw, y + gh, g.u1, g.v1, color);
            vert(penX,      y,      g.u0, g.v0, color);
            vert(penX + gw, y + gh, g.u1, g.v1, color);
            vert(penX,      y + gh, g.u0, g.v1, color);
        }
        penX += font_->dAdvance(ch) * scale;
    }
}

void UiDrawList::textDRight(float x, float y, std::string_view s, uint32_t color, float scale) {
    textD(x - textDWidth(s, scale), y, s, color, scale);
}

void UiDrawList::textDCentered(float cx, float y, std::string_view s, uint32_t color, float scale) {
    textD(cx - textDWidth(s, scale) * 0.5f, y, s, color, scale);
}

} // namespace pulse
