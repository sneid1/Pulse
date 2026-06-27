#include "Engine/UI/Font.hpp"
#include "Engine/Core/Log.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace pulse {

namespace {
// The process CWD is the EXE folder (build/ or dist/), so bundled assets live at one of these
// prefixes relative to it - exactly like the game's resolveAsset(). Font.cpp previously used the
// raw cwd-relative "assets/fonts/..." which only exists in a packaged dist/; from a dev build/ it
// resolved to build/assets/fonts (absent), AddFontResourceExW failed, and GDI silently fell back
// to a generic face - so the ENTIRE UI rendered in the wrong typeface (not Chakra Petch +
// JetBrains Mono). Resolve against the same prefixes so the bundled fonts always load.
std::wstring resolveFontPath(const wchar_t* rel) {
    const wchar_t* prefixes[] = { L"", L"../", L"../../", L"../../../" };
    for (const wchar_t* p : prefixes) {
        std::wstring cand = std::wstring(p) + rel;
        std::error_code ec;
        if (std::filesystem::exists(cand, ec)) return cand;
    }
    return rel;
}

// Register the bundled TTFs privately to this process so CreateFontW can select them by face
// name even when they are not installed system-wide. The PULSE menu typography is load-bearing
// (the whole UI is authored in Chakra Petch + JetBrains Mono), so a failed registration
// is fatal, not a quiet substitute (PROJECT_RULES: never silently substitute a required asset).
void registerBundledFonts() {
    static bool done = false;
    if (done) return;
    done = true;
    const wchar_t* files[] = {
        L"assets/fonts/JetBrainsMono-Regular.ttf",
        L"assets/fonts/JetBrainsMono-Medium.ttf",
        L"assets/fonts/JetBrainsMono-Bold.ttf",
        L"assets/fonts/ChakraPetch-Regular.ttf",
        L"assets/fonts/ChakraPetch-Medium.ttf",
        L"assets/fonts/ChakraPetch-Bold.ttf",
    };
    for (const wchar_t* f : files) {
        const std::wstring path = resolveFontPath(f);
        if (AddFontResourceExW(path.c_str(), FR_PRIVATE, nullptr) == 0)
            fail("Font: could not register required UI typeface. Looked for the bundled TTFs under "
                 "assets/fonts/ (Chakra Petch + JetBrains Mono). Without them the UI renders in the wrong "
                 "font. Run from the repo root or a packaged dist/, or run tools/Provision-ThirdParty.");
    }
}
} // namespace

void Font::bake(rhi::Heaps& heaps, rhi::Uploader& uploader, int pixelHeight) {
    registerBundledFonts();

    const int cols = 16;
    const int rows = (95 + cols - 1) / cols;   // 6 rows per face
    // Per-face supersample: keep the LOGICAL metrics fixed while baking more source pixels than
    // the on-screen glyphs need. The font atlas is uploaded without mips, so this resolves big
    // headings without bitmap upscaling and keeps small mono labels crisp instead of box-filtered.
    // NEGATIVE lfHeight (below) makes the requested size the glyph EM height (CSS font-size
    // semantics), so Chakra Petch renders at the intended size.
    const int monoSS = 4;
    const int dispSS = 8;
    const int monoGdi = pixelHeight * monoSS;
    const int dispGdi = pixelHeight * dispSS;
    const int monoGutter = monoSS;
    const int dispGutter = dispSS;

    HDC screenDC = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screenDC);
    ReleaseDC(nullptr, screenDC);

    // MONO face: JetBrains Mono (the doc's telemetry face), medium weight, fixed pitch.
    HFONT monoFont = CreateFontW(-monoGdi, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        FIXED_PITCH | FF_MODERN, L"JetBrains Mono");
    // DISPLAY face: Chakra Petch (the doc's headline face), heavy weight, proportional.
    HFONT dispFont = CreateFontW(-dispGdi, 0, 0, 0, FW_EXTRABOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        VARIABLE_PITCH | FF_SWISS, L"Chakra Petch");

    // ---- measure both faces ----
    // Cell height is the typographic EM (the requested size), NOT tmHeight: JetBrains Mono and
    // Chakra Petch and JetBrains Mono both report a tmHeight above the em (built-in line gap for accents we never
    // render), and the logical cell height feeds fpx() as the normaliser - so using tmHeight made
    // EVERY label render ~0.6x its intended size. We bake into an em-tall cell and drop the glyph
    // baseline 0.78 down it; ASCII has no ink in the trimmed top gap so nothing clips.
    SelectObject(dc, monoFont);
    TEXTMETRICW mtm{}; GetTextMetricsW(dc, &mtm);
    const int monoCellW = mtm.tmAveCharWidth + monoGutter;
    const int monoCellH = monoGdi + monoGutter;
    const int monoBaseline = static_cast<int>(monoGdi * 0.78f);

    SelectObject(dc, dispFont);
    TEXTMETRICW dtm{}; GetTextMetricsW(dc, &dtm);
    const int dispCellH = dispGdi + dispGutter;
    const int dispBaseline = static_cast<int>(dispGdi * 0.80f);
    // display cell width = widest glyph advance + a little headroom (proportional glyphs are
    // left-aligned in their cell; the per-glyph advance drives layout).
    int dispMaxAdv = 0;
    int dispAdv[95];
    for (int i = 0; i < 95; ++i) {
        const wchar_t ch = static_cast<wchar_t>(32 + i);
        SIZE sz{}; GetTextExtentPoint32W(dc, &ch, 1, &sz);
        dispAdv[i] = sz.cx;
        if (sz.cx > dispMaxAdv) dispMaxAdv = sz.cx;
    }
    const int dispCellW = dispMaxAdv + dispGutter * 2;

    // ---- atlas layout: mono grid on top, display grid below ----
    // Each glyph's sampled cell is monoCellW x monoCellH, but cells are spaced a half-em farther
    // apart with EMPTY padding. The box-filtered mip chain can then only bleed dead space into a
    // minified cell, never a neighbouring glyph - which was the faint stub artifact under small
    // labels. The UV still samples just the cell, so text size and spacing are unchanged.
    const int monoPad = monoGdi / 2;
    const int dispPad = dispGdi / 2;
    const int monoStrideW = monoCellW + monoPad;
    const int monoStrideH = monoCellH + monoPad;
    const int dispStrideW = dispCellW + dispPad;
    const int dispStrideH = dispCellH + dispPad;
    const int monoGridW = cols * monoStrideW + 4 * monoSS;   // + white texel column
    const int dispGridW = cols * dispStrideW;
    const int atlasW = monoGridW > dispGridW ? monoGridW : dispGridW;
    const int monoGridH = rows * monoStrideH;
    const int dispGridH = rows * dispStrideH;
    const int atlasH = monoGridH + dispGridH;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = atlasW;
    bi.bmiHeader.biHeight = -atlasH;           // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, dib);

    RECT full{ 0, 0, atlasW, atlasH };
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &full, black);
    DeleteObject(black);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));

    // mono glyphs (top band)
    SelectObject(dc, monoFont);
    for (int i = 0; i < 95; ++i) {
        const int col = i % cols, row = i / cols;
        const int gx = col * monoStrideW, gy = row * monoStrideH;
        const wchar_t ch = static_cast<wchar_t>(32 + i);
        TextOutW(dc, gx, gy + monoBaseline - mtm.tmAscent, &ch, 1);
        Glyph& g = glyphs_[i];
        g.u0 = static_cast<float>(gx) / atlasW;
        g.v0 = static_cast<float>(gy) / atlasH;
        g.u1 = static_cast<float>(gx + monoCellW) / atlasW;
        g.v1 = static_cast<float>(gy + monoCellH) / atlasH;
    }

    // display glyphs (bottom band, offset by monoGridH)
    SelectObject(dc, dispFont);
    for (int i = 0; i < 95; ++i) {
        const int col = i % cols, row = i / cols;
        const int gx = col * dispStrideW;
        const int gy = monoGridH + row * dispStrideH;
        const wchar_t ch = static_cast<wchar_t>(32 + i);
        TextOutW(dc, gx, gy + dispBaseline - dtm.tmAscent, &ch, 1);
        Glyph& g = dGlyphs_[i];
        g.u0 = static_cast<float>(gx) / atlasW;
        g.v0 = static_cast<float>(gy) / atlasH;
        g.u1 = static_cast<float>(gx + dispCellW) / atlasW;
        g.v1 = static_cast<float>(gy + dispCellH) / atlasH;
        dAdv_[i] = static_cast<float>(dispAdv[i]) / static_cast<float>(dispSS);
    }

    // white texel block in the mono band's right margin.
    RECT whiteRect{ atlasW - 3 * monoSS, 0, atlasW, 3 * monoSS };
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &whiteRect, white);
    DeleteObject(white);
    whiteU_ = static_cast<float>(atlasW - 2 * monoSS) / atlasW;
    whiteV_ = static_cast<float>(monoSS) * 1.5f / atlasH;

    GdiFlush();

    std::vector<uint8_t> rgba(static_cast<size_t>(atlasW) * atlasH * 4);
    const uint8_t* src = static_cast<const uint8_t*>(bits);
    for (int p = 0; p < atlasW * atlasH; ++p) {
        const uint8_t coverage = src[p * 4 + 0];
        rgba[p * 4 + 0] = coverage;
        rgba[p * 4 + 1] = coverage;
        rgba[p * 4 + 2] = coverage;
        rgba[p * 4 + 3] = coverage;
    }

    SelectObject(dc, oldBmp);
    DeleteObject(dib);
    DeleteObject(monoFont);
    DeleteObject(dispFont);
    DeleteDC(dc);

    atlas_ = uploader.uploadTexture2DNoMips(rgba.data(), atlasW, atlasH, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"pulse.font.atlas");
    uploader.flush();
    atlasSrv_ = heaps.createTextureSrv(atlas_);

    // logical metrics (atlas px / per-face ss).
    cellW_ = static_cast<float>(monoCellW) / static_cast<float>(monoSS);
    cellH_ = static_cast<float>(monoCellH) / static_cast<float>(monoSS);
    dCellW_ = static_cast<float>(dispCellW) / static_cast<float>(dispSS);
    dCellH_ = static_cast<float>(dispCellH) / static_cast<float>(dispSS);
}

} // namespace pulse
