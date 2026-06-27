// PULSE entry point. Drives the game on the D3D12 engine: windowed interactive
// play, or headless capture for the AI verify loop (--pose / --fire-pose /
// --bot-test / --screenshot / --render-pass / --record-dir), plus --gpu-info and offline
// audio render. A missing required asset fails loud (PROJECT_RULES.md).

#define PULSE_AGILITY_SDK_IMPL
#include "Engine/RHI/AgilitySDK.hpp"

#include "Engine/Engine.hpp"
#include "Engine/Audio.hpp"
#include "Engine/Input.hpp"
#include "Engine/Core/CrashHandler.hpp"
#include "Engine/Core/Image.hpp"
#include "Engine/Core/MeshFile.hpp"
#include "Engine/Core/Paths.hpp"
#include "Platform/Window.hpp"
#include "Game/PulseGame.hpp"
#include "Game/Settings.hpp"

#ifdef _WIN32
#include <windows.h>   // AttachConsole / GetStdHandle for the parent-console re-attach below
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace pulse;

namespace {

#ifdef _WIN32
// The game links on the WINDOWS subsystem, so a double-click opens no console window. But when it
// is launched from a terminal - or with stdout redirected to a file/pipe for the headless capture
// loop - we still want [pulse:info] logs and --gpu-info/--screenshot output to appear. If a std
// handle is already wired up (redirected/inherited) leave it; otherwise attach to the launching
// console if there is one. A plain double-click has no parent console, so nothing is shown.
void attachParentConsole() {
    if (GetStdHandle(STD_OUTPUT_HANDLE) != nullptr) return;   // stdout already redirected/inherited
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;        // no launching terminal (double-click)
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$",  "r", stdin);
}
#endif

// Build an InputState from the window each frame (edge-detect presses).
struct InputTracker {
    InputState state;
    std::array<bool, 256> prevKeys{};
    std::array<bool, 5>   prevMouse{};
    void poll(Window& win) {
        state.keyDown.fill(false); state.keyPressed.fill(false);
        for (int i = 0; i < 256; ++i) {
            const bool d = win.keyDown(i);
            state.keyDown[i] = d;
            if (d && !prevKeys[i]) state.keyPressed[i] = true;
            prevKeys[i] = d;
        }
        state.mouseDown.fill(false); state.mousePressed.fill(false);
        for (int i = 0; i < 5; ++i) {
            const bool d = win.mouseButton(i);
            state.mouseDown[i] = d;
            if (d && !prevMouse[i]) state.mousePressed[i] = true;
            prevMouse[i] = d;
        }
        state.mouseDeltaX = win.mouseDeltaX();
        state.mouseDeltaY = win.mouseDeltaY();
        state.mouseX = win.mouseX();
        state.mouseY = win.mouseY();
        state.hasFocus = true;
    }
};

// Locate a vendored tool by walking up from the executable folder.
std::string resolveUp(const std::string& rel) {
    const char* prefixes[] = { "", "../", "../../", "../../../" };
    for (const char* p : prefixes) {
        const std::string c = std::string(p) + rel;
        if (std::filesystem::exists(c)) return c;
    }
    return rel;
}

bool endsWithCI(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(),
                      [](char a, char b) { return std::tolower(a) == std::tolower(b); });
}

bool parseWeaponEventType(const std::string& text, WeaponEventType& out) {
    std::string v = text;
    std::transform(v.begin(), v.end(), v.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    if (v == "dry" || v == "dryfire" || v == "dry_fire") { out = WeaponEventType::DryFire; return true; }
    if (v == "reload_start" || v == "reloadstart" || v == "start") { out = WeaponEventType::ReloadStart; return true; }
    if (v == "reload_end" || v == "reloadend" || v == "end") { out = WeaponEventType::ReloadEnd; return true; }
    if (v == "mag_out" || v == "magout") { out = WeaponEventType::MagOut; return true; }
    if (v == "mag_in" || v == "magin" || v == "insert") { out = WeaponEventType::MagIn; return true; }
    if (v == "bolt") { out = WeaponEventType::Bolt; return true; }
    if (v == "shell") { out = WeaponEventType::Shell; return true; }
    if (v == "equip") { out = WeaponEventType::Equip; return true; }
    return false;
}

bool parseEnemyEventType(const std::string& text, EnemyEventType& out) {
    std::string v = text;
    std::transform(v.begin(), v.end(), v.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    if (v == "telegraph" || v == "tell" || v == "windup" || v == "wind_up") { out = EnemyEventType::Telegraph; return true; }
    if (v == "shot" || v == "fire" || v == "projectile") { out = EnemyEventType::Shot; return true; }
    if (v == "impact" || v == "projectile_impact" || v == "orb_impact") { out = EnemyEventType::Impact; return true; }
    if (v == "beam" || v == "ray") { out = EnemyEventType::Beam; return true; }
    if (v == "lunge" || v == "pounce" || v == "slam") { out = EnemyEventType::Lunge; return true; }
    if (v == "melee_hit" || v == "meleehit" || v == "strike" || v == "hit_player") { out = EnemyEventType::MeleeHit; return true; }
    if (v == "hurt" || v == "enemy_hurt" || v == "body_hit") { out = EnemyEventType::Hurt; return true; }
    if (v == "death" || v == "die" || v == "kill") { out = EnemyEventType::Death; return true; }
    if (v == "boss_burst" || v == "bossburst" || v == "burst") { out = EnemyEventType::BossBurst; return true; }
    return false;
}

bool parseFeedbackEventType(const std::string& text, FeedbackEventType& out) {
    std::string v = text;
    std::transform(v.begin(), v.end(), v.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    static const std::pair<const char*, FeedbackEventType> kNames[] = {
        { "hitmarker", FeedbackEventType::Hitmarker }, { "hit_crit", FeedbackEventType::HitCrit },
        { "kill", FeedbackEventType::Kill }, { "kill_elite", FeedbackEventType::KillElite },
        { "dash", FeedbackEventType::Dash }, { "jump", FeedbackEventType::Jump },
        { "ability_tactical", FeedbackEventType::AbilityTactical },
        { "ability_ultimate", FeedbackEventType::AbilityUltimate },
        { "charge_ready", FeedbackEventType::ChargeReady }, { "explosion", FeedbackEventType::Explosion },
        { "shield_absorb", FeedbackEventType::ShieldAbsorb }, { "shield_break", FeedbackEventType::ShieldBreak },
        { "low_health", FeedbackEventType::LowHealth },
        { "pickup_health", FeedbackEventType::PickupHealth }, { "pickup_shield", FeedbackEventType::PickupShield },
        { "pickup_ammo", FeedbackEventType::PickupAmmo }, { "pickup_scrap", FeedbackEventType::PickupScrap },
        { "pickup_powerup", FeedbackEventType::PickupPowerup },
        { "element_burn", FeedbackEventType::ElementBurn }, { "burn", FeedbackEventType::ElementBurn },
        { "element_shock", FeedbackEventType::ElementShock }, { "shock", FeedbackEventType::ElementShock },
        { "element_cryo", FeedbackEventType::ElementCryo }, { "cryo", FeedbackEventType::ElementCryo },
        { "element_corrode", FeedbackEventType::ElementCorrode }, { "corrode", FeedbackEventType::ElementCorrode },
        { "element_combo", FeedbackEventType::ElementCombo }, { "combo", FeedbackEventType::ElementCombo },
        { "element_leech", FeedbackEventType::ElementLeech }, { "leech", FeedbackEventType::ElementLeech },
        { "ui_move", FeedbackEventType::UiMove }, { "ui_confirm", FeedbackEventType::UiConfirm },
        { "ui_cancel", FeedbackEventType::UiCancel }, { "ui_reward", FeedbackEventType::UiReward },
        { "run_win", FeedbackEventType::RunWin }, { "run_lose", FeedbackEventType::RunLose },
    };
    for (const auto& [name, type] : kNames) {
        if (v == name) { out = type; return true; }
    }
    return false;
}

class StartupSplash {
public:
    void start() {
        running_.store(true);
        thread_ = std::thread([this] { threadMain(); });
    }

    void stop() {
        running_.store(false);
        if (HWND hwnd = hwnd_.load()) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    ~StartupSplash() {
        stop();
    }

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_TIMER) {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (msg == WM_KEYDOWN && wparam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int w = rc.right - rc.left;
            const int h = rc.bottom - rc.top;
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
            HGDIOBJ oldBmp = SelectObject(mem, bmp);

            auto fill = [&](int x0, int y0, int x1, int y1, COLORREF color) {
                RECT r{ x0, y0, x1, y1 };
                HBRUSH b = CreateSolidBrush(color);
                FillRect(mem, &r, b);
                DeleteObject(b);
            };
            auto line = [&](int x0, int y0, int x1, int y1, COLORREF color, int thickness = 1) {
                HPEN pen = CreatePen(PS_SOLID, thickness, color);
                HGDIOBJ oldPen = SelectObject(mem, pen);
                MoveToEx(mem, x0, y0, nullptr);
                LineTo(mem, x1, y1);
                SelectObject(mem, oldPen);
                DeleteObject(pen);
            };

            // Timing: an ease-in progress curve so the bar reads like a genuine
            // load (asymptotic toward ~98%), and a wall-clock t for the animation.
            static const ULONGLONG bootTick = GetTickCount64();
            const double t = static_cast<double>(GetTickCount64() - bootTick) / 1000.0;
            const double progress = (1.0 - std::exp(-t / 1.8)) * 0.985;
            const int pct = static_cast<int>(progress * 100.0 + 0.5);

            auto lerpCol = [](COLORREF a, COLORREF b, double f) -> COLORREF {
                f = f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
                return RGB(
                    static_cast<int>(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * f),
                    static_cast<int>(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * f),
                    static_cast<int>(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * f));
            };

            // Palette mirrors the in-game pal:: Neon Ink Brutalism tokens: void
            // black, one cyan energy accent, ion-white ink. No magenta anywhere.
            const COLORREF cVoid    = RGB(4, 6, 10);        // #04060a (matches the in-game deep bg)
            const COLORREF cPanelT  = RGB(12, 20, 30);
            const COLORREF cPanelB  = RGB(6, 10, 16);
            const COLORREF cBorder  = RGB(38, 54, 66);      // ~ rgba(90,170,200,.16) over dark
            const COLORREF cLine    = RGB(20, 30, 40);
            const COLORREF cAccent  = RGB(63, 224, 255);    // pal::accent #3fe0ff
            const COLORREF cGlow    = RGB(150, 240, 255);   // pal::accentGlow
            const COLORREF cIon     = RGB(238, 246, 251);   // pal::textHi #eef6fb
            const COLORREF cTextMid = RGB(169, 188, 199);   // pal::textMid
            const COLORREF cTextDim = RGB(111, 138, 153);   // pal::textDim #6f8a99
            const COLORREF cTextFnt = RGB(72, 96, 110);     // pal::textFaint
            const COLORREF cTrace   = RGB(26, 78, 92);

            const int m = 20;                       // panel inset
            const int px0 = m, py0 = m, px1 = w - m, py1 = h - m;
            const int cx = 56;                       // content left edge
            const int cxr = w - 56;                  // content right edge

            // Background: void fill, then a vertical-gradient inner panel for depth.
            fill(0, 0, w, h, cVoid);
            for (int y = py0; y < py1; ++y) {
                const double f = static_cast<double>(y - py0) / std::max(1, py1 - py0);
                fill(px0, y, px1, y + 1, lerpCol(cPanelT, cPanelB, f));
            }
            // Notched hairline frame: the top-left + bottom-right corners are cut at 45 degrees,
            // the same signature notch the in-game panels and the START RUN bar use.
            const int nc = 18;
            line(px0 + nc, py0, px1, py0, cBorder, 1);          // top
            line(px1, py0, px1, py1 - nc, cBorder, 1);          // right
            line(px1, py1 - nc, px1 - nc, py1, cBorder, 1);     // BR cut
            line(px1 - nc, py1, px0, py1, cBorder, 1);          // bottom
            line(px0, py1, px0, py0 + nc, cBorder, 1);          // left
            line(px0, py0 + nc, px0 + nc, py0, cBorder, 1);     // TL cut
            line(px0 + nc + 1, py0 + 1, px1 - 1, py0 + 1, RGB(44, 58, 72), 1);   // top inner bevel
            // Brutalist corner registration ticks.
            const COLORREF tick = lerpCol(cBorder, cAccent, 0.5);
            const int tl = 16, ti = 10;
            line(px0 + ti, py0 + ti, px0 + ti + tl, py0 + ti, tick, 1);
            line(px0 + ti, py0 + ti, px0 + ti, py0 + ti + tl, tick, 1);
            line(px1 - ti, py0 + ti, px1 - ti - tl, py0 + ti, tick, 1);
            line(px1 - ti, py0 + ti, px1 - ti, py0 + ti + tl, tick, 1);
            line(px0 + ti, py1 - ti, px0 + ti + tl, py1 - ti, tick, 1);
            line(px0 + ti, py1 - ti, px0 + ti, py1 - ti - tl, tick, 1);
            line(px1 - ti, py1 - ti, px1 - ti - tl, py1 - ti, tick, 1);
            line(px1 - ti, py1 - ti, px1 - ti, py1 - ti - tl, tick, 1);

            SetBkMode(mem, TRANSPARENT);

            // Register the bundled brand typefaces (Chakra Petch + JetBrains Mono) with GDI once so
            // this boot splash renders in the SAME faces as the rest of the UI, not a generic system
            // fallback. Resolved against the same dev/packaged prefixes the engine font loader uses.
            static const bool s_splashFonts = [] {
                const wchar_t* files[] = {
                    L"assets/fonts/ChakraPetch-Bold.ttf",      L"assets/fonts/ChakraPetch-Medium.ttf",
                    L"assets/fonts/JetBrainsMono-Regular.ttf", L"assets/fonts/JetBrainsMono-Medium.ttf",
                    L"assets/fonts/JetBrainsMono-Bold.ttf",
                };
                const wchar_t* prefixes[] = { L"", L"../", L"../../", L"../../../" };
                for (const wchar_t* rel : files)
                    for (const wchar_t* p : prefixes) {
                        std::wstring cand = std::wstring(p) + rel;
                        std::error_code ec;
                        if (std::filesystem::exists(cand, ec)) {
                            AddFontResourceExW(cand.c_str(), FR_PRIVATE, nullptr);
                            break;
                        }
                    }
                return true;
            }();
            (void)s_splashFonts;

            // Kicker label (JetBrains Mono, wide-tracked - the telemetry/label face).
            const wchar_t* kKick = L"// PULSE COMBAT ENGINE";
            HFONT fKick = CreateFontW(13, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"JetBrains Mono");
            HFONT fOld = static_cast<HFONT>(SelectObject(mem, fKick));
            SetTextColor(mem, cTextDim);
            SetTextCharacterExtra(mem, 3);
            TextOutW(mem, cx, 40, kKick, lstrlenW(kKick));
            SetTextCharacterExtra(mem, 0);
            SelectObject(mem, fOld);
            DeleteObject(fKick);

            // PULSE wordmark (Chakra Petch - the brand display face, squared/beveled letterforms).
            const wchar_t* kTitle = L"PULSE";
            HFONT fTitle = CreateFontW(80, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Chakra Petch");
            fOld = static_cast<HFONT>(SelectObject(mem, fTitle));
            SetTextColor(mem, cIon);
            SetTextCharacterExtra(mem, 4);
            TextOutW(mem, cx - 2, 58, kTitle, lstrlenW(kTitle));
            SetTextCharacterExtra(mem, 0);
            SelectObject(mem, fOld);
            DeleteObject(fTitle);

            // Divider: a bright accent stub running into a dim full-width hairline.
            const int divY = 150;
            line(cx, divY, cxr, divY, cLine, 1);
            line(cx, divY, cx + 132, divY, cAccent, 2);

            // Subtitle (JetBrains Mono, tracked uppercase - the doc's panel-label treatment).
            const wchar_t* kSub = L"BOOTING COMBAT SIMULATION";
            HFONT fSub = CreateFontW(14, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"JetBrains Mono");
            fOld = static_cast<HFONT>(SelectObject(mem, fSub));
            SetTextColor(mem, cTextMid);
            SetTextCharacterExtra(mem, 3);
            TextOutW(mem, cx, 162, kSub, lstrlenW(kSub));
            SetTextCharacterExtra(mem, 0);
            SelectObject(mem, fOld);
            DeleteObject(fSub);

            // ---- Heartbeat waveform: the signature "pulse" motif. -------------
            const int baseY = 250;
            const int amp   = 46;
            const int wx0 = cx, wx1 = cxr;
            line(wx0, baseY, wx1, baseY, RGB(20, 28, 38), 1);     // faint monitor baseline

            auto bump = [](double x, double c, double wd, double a) {
                const double d = (x - c) / wd; return a * std::exp(-d * d);
            };
            auto beat = [&](double q) {                            // one cardiac cycle, q in [0,1)
                double y = 0.0;
                y += bump(q, 0.300, 0.025, 0.16);                  // P wave
                y -= bump(q, 0.385, 0.010, 0.22);                  // Q
                y += bump(q, 0.420, 0.013, 1.00);                  // R spike
                y -= bump(q, 0.460, 0.016, 0.40);                  // S
                y += bump(q, 0.610, 0.050, 0.26);                  // T wave
                return y;
            };
            const double beatLen = 232.0;
            const double scroll  = 0.34;                           // beats/sec, trace travels left
            auto sampleY = [&](int x) {
                const double s = (x - wx0) / beatLen + t * scroll;
                const double q = s - std::floor(s);
                return baseY - amp * beat(q);
            };
            int prevX = wx0; double prevY = sampleY(wx0);
            for (int x = wx0 + 2; x <= wx1; x += 2) {
                const double yv = sampleY(x);
                const double fx = static_cast<double>(x - wx0) / (wx1 - wx0);   // 0 left .. 1 right
                COLORREF col = lerpCol(cTrace, cAccent, fx);                    // brighter toward the lead
                if (fx > 0.82) col = lerpCol(col, cGlow, (fx - 0.82) / 0.18);   // hot leading edge
                const double peak = (baseY - yv) / amp;                         // glow on the spikes
                if (peak > 0.18) col = lerpCol(col, cGlow, std::min(1.0, peak) * 0.55);
                line(prevX, static_cast<int>(prevY + 0.5), x, static_cast<int>(yv + 0.5), col, 2);
                prevX = x; prevY = yv;
            }
            // Bright scan dot at the live (right) edge of the trace.
            {
                const int lx = wx1;
                const int ly = static_cast<int>(sampleY(wx1) + 0.5);
                HBRUSH gb = CreateSolidBrush(cGlow);
                HGDIOBJ ob = SelectObject(mem, gb);
                HGDIOBJ op = SelectObject(mem, GetStockObject(NULL_PEN));
                Ellipse(mem, lx - 5, ly - 5, lx + 5, ly + 5);
                SelectObject(mem, op);
                SelectObject(mem, ob);
                DeleteObject(gb);
            }
            // Emitter at the trace origin: a core dot with expanding, fading rings.
            {
                const int ex = wx0, ey = baseY;
                const double per = 0.92;
                for (int k = 0; k < 2; ++k) {
                    double ph = t / per + k * 0.5; ph -= std::floor(ph);
                    const int rr = static_cast<int>(6 + ph * 30.0);
                    HPEN rp = CreatePen(PS_SOLID, 1, lerpCol(cAccent, cPanelB, ph));
                    HGDIOBJ op = SelectObject(mem, rp);
                    HGDIOBJ ob = SelectObject(mem, GetStockObject(NULL_BRUSH));
                    Ellipse(mem, ex - rr, ey - rr, ex + rr, ey + rr);
                    SelectObject(mem, ob);
                    SelectObject(mem, op);
                    DeleteObject(rp);
                }
                const int cr = 3 + static_cast<int>((0.5 + 0.5 * std::sin(t * 6.2831853 / per)) * 2.0);
                HBRUSH cb = CreateSolidBrush(cGlow);
                HGDIOBJ ob = SelectObject(mem, cb);
                HGDIOBJ op = SelectObject(mem, GetStockObject(NULL_PEN));
                Ellipse(mem, ex - cr, ey - cr, ex + cr, ey + cr);
                SelectObject(mem, op);
                SelectObject(mem, ob);
                DeleteObject(cb);
            }

            // ---- Status line + percent, advancing with progress. -------------
            static const wchar_t* kStatus[] = {
                L"Compiling renderer passes",
                L"Streaming arena kit",
                L"Uploading weapon rigs",
                L"Binding audio banks",
                L"Building first room",
            };
            const int statusIndex = std::min(4, static_cast<int>(progress * 5.0));
            const int statY = h - 86;
            const wchar_t* statStr = kStatus[statusIndex];
            HFONT fStat = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"JetBrains Mono");
            fOld = static_cast<HFONT>(SelectObject(mem, fStat));
            SetTextColor(mem, cTextMid);
            TextOutW(mem, cx, statY, statStr, lstrlenW(statStr));
            SelectObject(mem, fOld);
            DeleteObject(fStat);

            wchar_t pctBuf[16];
            wsprintfW(pctBuf, L"%d%%", pct);
            HFONT fPct = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"JetBrains Mono");
            fOld = static_cast<HFONT>(SelectObject(mem, fPct));
            SetTextColor(mem, cAccent);
            RECT pctRc{ cxr - 80, statY, cxr, statY + 22 };
            DrawTextW(mem, pctBuf, -1, &pctRc, DT_RIGHT | DT_SINGLELINE | DT_TOP);
            SelectObject(mem, fOld);
            DeleteObject(fPct);

            // ---- Progress bar: track, accent gradient fill, bright head. -----
            const int barY = h - 58, barH = 6;
            fill(cx, barY, cxr, barY + barH, RGB(16, 23, 31));
            line(cx, barY, cxr, barY, cBorder, 1);
            line(cx, barY + barH, cxr, barY + barH, cBorder, 1);
            const int barW = cxr - cx;
            const int fillW = static_cast<int>(barW * progress);
            for (int i = 0; i < fillW; ++i) {
                const double f = static_cast<double>(i) / std::max(1, barW);
                fill(cx + i, barY + 1, cx + i + 1, barY + barH, lerpCol(RGB(26, 120, 150), cAccent, f));
            }
            if (fillW > 2) {
                const int hx = cx + fillW;
                fill(hx - 2, barY, hx + 1, barY + barH, cGlow);
            }

            // ---- Footer engine tag. ------------------------------------------
            const wchar_t* kFoot = L"D3D12 - DXR   /   REVERSE-Z   /   BINDLESS SM6.6";
            HFONT fFoot = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"JetBrains Mono");
            fOld = static_cast<HFONT>(SelectObject(mem, fFoot));
            SetTextColor(mem, cTextFnt);
            SetTextCharacterExtra(mem, 2);
            TextOutW(mem, cx, h - 36, kFoot, lstrlenW(kFoot));
            SetTextCharacterExtra(mem, 0);
            SelectObject(mem, fOld);
            DeleteObject(fFoot);

            BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        if (msg == WM_CLOSE) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (msg == WM_DESTROY) {
            KillTimer(hwnd, 1);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    void threadMain() {
        HINSTANCE hinst = GetModuleHandleW(nullptr);
        const wchar_t* className = L"PulseStartupSplashClass";
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &StartupSplash::wndProc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = className;
        RegisterClassExW(&wc);

        constexpr int w = 760;
        constexpr int h = 408;
        const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        const int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, className, L"PULSE Loading",
                                    WS_POPUP,
                                    x, y, w, h, nullptr, nullptr, hinst, nullptr);
        hwnd_.store(hwnd);
        if (hwnd) {
            SetTimer(hwnd, 1, 24, nullptr);
            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
        }

        MSG msg{};
        while (running_.load()) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    running_.store(false);
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (running_.load()) {
                WaitMessage();
            }
        }
        if (hwnd && IsWindow(hwnd)) {
            DestroyWindow(hwnd);
        }
        hwnd_.store(nullptr);
    }

    std::atomic<bool> running_{ false };
    std::atomic<HWND> hwnd_{ nullptr };
    std::thread thread_;
};

// Offline asset import: OBJ -> GLB (cgltf-loadable), or an image -> BCn DDS (BC7
// for colour/sRGB, BC5 for normal maps, BC7 linear for ORM-style maps) via the
// vendored DirectXTex texconv. dst is the output path.
bool importAsset(const std::string& src, const std::string& dst) {
    if (endsWithCI(src, ".obj")) {
        std::vector<pulse::StaticVertex> verts; std::vector<uint32_t> indices;
        if (!pulse::loadObjMesh(src, verts, indices)) { std::fprintf(stderr, "import: cannot read OBJ %s\n", src.c_str()); return false; }
        if (!pulse::writeGlb(dst, verts, indices)) { std::fprintf(stderr, "import: cannot write GLB %s\n", dst.c_str()); return false; }
        std::printf("import: %s -> %s (%zu verts, %zu indices)\n", src.c_str(), dst.c_str(), verts.size(), indices.size());
        return true;
    }
    // Image -> DDS via texconv.
    std::string lname = src; std::transform(lname.begin(), lname.end(), lname.begin(), [](char c){ return static_cast<char>(std::tolower(c)); });
    const char* fmt = "BC7_UNORM_SRGB";            // albedo / colour default
    if (lname.find("_nor") != std::string::npos || lname.find("normal") != std::string::npos || lname.find("nrm") != std::string::npos)
        fmt = "BC5_UNORM";                          // tangent-space normal (XY; Z reconstructed in-shader)
    else if (lname.find("orm") != std::string::npos || lname.find("arm") != std::string::npos ||
             lname.find("rough") != std::string::npos || lname.find("metal") != std::string::npos || lname.find("_ao") != std::string::npos)
        fmt = "BC7_UNORM";                          // linear ORM-style data map
    const std::string texconv = resolveUp("third_party/directxtex/texconv.exe");
    if (!std::filesystem::exists(texconv)) { std::fprintf(stderr, "import: texconv not found at %s\n", texconv.c_str()); return false; }
    const std::filesystem::path dstPath(dst);
    const std::string outDir = dstPath.has_parent_path() ? dstPath.parent_path().string() : std::string(".");
    std::filesystem::create_directories(outDir);
    // -dx10 forces the extended DX10 header (explicit DXGI format) for every output,
    // so the engine's DDS loader reads one header layout (BC5 would otherwise emit a
    // legacy ATI2/BC5U fourCC).
    const std::string cmd = "\"\"" + texconv + "\" -y -nologo -dx10 -m 0 -f " + fmt + " -o \"" + outDir + "\" \"" + src + "\"\"";
    const int rc = std::system(cmd.c_str());
    if (rc != 0) { std::fprintf(stderr, "import: texconv failed (%d)\n", rc); return false; }
    // texconv writes <srcstem>.dds in outDir; rename to dst if needed.
    const std::filesystem::path produced = std::filesystem::path(outDir) / (std::filesystem::path(src).stem().string() + ".dds");
    std::error_code ec;
    if (produced != dstPath && std::filesystem::exists(produced)) {
        std::filesystem::remove(dstPath, ec);
        std::filesystem::rename(produced, dstPath, ec);
    }
    std::printf("import: %s -> %s (%s)\n", src.c_str(), dst.c_str(), fmt);
    return std::filesystem::exists(dstPath);
}

Engine::Config headlessConfig(const Tunables& tn, bool forceRaster,
                              bool enableSsgi = true, bool enableSsr = true, uint32_t rtRayCount = 3) {
    Engine::Config cfg;
    cfg.hwnd = nullptr;
    cfg.width = static_cast<uint32_t>(tn.windowWidth);
    cfg.height = static_cast<uint32_t>(tn.windowHeight);
    cfg.forceRaster = forceRaster;
    cfg.enableSsgi = enableSsgi;
    cfg.enableSsr = enableSsr;
    cfg.rtRayCount = rtRayCount;
#if defined(PULSE_DEBUG_LAYER)
    cfg.enableDebugLayer = true;
#endif
#if defined(PULSE_GPU_VALIDATION)
    cfg.enableGpuValidation = true;
#endif
    return cfg;
}

int runWindowed(PulseGame& game, bool forceRaster,
                bool enableSsgi = true, bool enableSsr = true, uint32_t rtRayCount = 3) {
    const Tunables& tn = game.tunables();
    StartupSplash splash;
    splash.start();

    // Read the persisted display mode so the very first frame is already in the right
    // presentation (borderless fullscreen by default). The game owns the authoritative
    // copy once it loads its settings; we poll game.displayMode() below for live changes.
    Settings displaySettings;
    displaySettings.load();
    auto toMode = [](int m) {
        return m == 0 ? Window::DisplayMode::Windowed : Window::DisplayMode::BorderlessFullscreen;
    };

    Window window;
    window.create(L"PULSE", tn.windowWidth, tn.windowHeight, false, toMode(displaySettings.displayMode));
    // Mouse capture is driven per-frame by the game now (released while a menu is open).

    Engine engine;
    Engine::Config cfg;
    cfg.hwnd = window.hwnd();
    cfg.width = static_cast<uint32_t>(window.width());    // actual client size (full monitor in borderless)
    cfg.height = static_cast<uint32_t>(window.height());
    cfg.forceRaster = forceRaster;
    cfg.enableSsgi = enableSsgi;
    cfg.enableSsr = enableSsr;
    cfg.rtRayCount = rtRayCount;
#if defined(PULSE_DEBUG_LAYER)
    cfg.enableDebugLayer = true;
#endif
    engine.init(cfg);
    std::printf("%s", engine.device().gpuInfoString().c_str());

    AudioSystem audio;
    InputTracker input;
    game.enterMainMenu();           // boot into the front-end shell (main menu)

    // First-frame resource creation loads the arena kit, models, textures, and GPU resources.
    // Do it before the window is visible so Windows never paints a blank client area and flags it hung.
    engine.renderFrame(game.buildFrame(engine, static_cast<int>(engine.width()), static_cast<int>(engine.height())));
    splash.stop();
    window.show();

    bool prevCapture = false;
    using Clock = std::chrono::high_resolution_clock;
    auto last = Clock::now();

    int appliedDisplayMode = displaySettings.displayMode;
    while (window.pumpMessages()) {
        // Live display-mode switch from the Options menu (Windowed <-> Borderless fullscreen).
        if (game.displayMode() != appliedDisplayMode) {
            appliedDisplayMode = game.displayMode();
            window.setDisplayMode(toMode(appliedDisplayMode));
        }
        if (window.consumeResize())
            engine.resize(static_cast<uint32_t>(window.width()), static_cast<uint32_t>(window.height()));
        const bool capture = game.wantsMouseCapture();
        window.setMouseCapture(capture);
        window.updateMouseLook();
        if (capture && !prevCapture) window.clearMouseDelta();   // no view jump when (re)capturing
        prevCapture = capture;
        input.poll(window);

        const auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        dt = dt > 0.1f ? 0.1f : dt;     // clamp huge stalls

        game.update(input.state, audio, dt, static_cast<int>(engine.width()), static_cast<int>(engine.height()));
        if (game.wantsQuit()) break;    // Quit to desktop from a menu
        engine.renderFrame(game.buildFrame(engine, static_cast<int>(engine.width()), static_cast<int>(engine.height())));
    }
    return 0;
}

} // namespace

int runPulse(int argc, char** argv, const std::filesystem::path& launchDir) {
    PulseGame game;
    const Tunables& tn = game.tunables();

    bool gpuInfo = false, pose = false, smoke = false, forceRaster = false, assetSelftest = false, importAll = false;
    bool marketingBot = false;
    bool reloadPose = false, rewardPose = false, hubPose = false, bossPose = false, contentDump = false;
    bool pathPose = false, shopPose = false, eventPose = false, coverPose = false, runOverPose = false, enemyTracerPose = false;
    bool doorsPose = false, codexPose = false;
    int  codexTab = 1;   // --codex-tab N: which field-manual tab to capture (0..8)
    bool dumpWeaponProfiles = false, weaponTest = false, captureWeaponMatrix = false;
    bool firePose = false;
    int  qualityOverride = -1;   // W6: --quality low|medium|high|ultra (-1 = use saved setting)
    float botTestSeconds = 0.0f;
    int   balanceSimRuns = 0;
    float reloadPoseProgress = 0.0f;
    float firePosePitch = 0.0f;
    std::string firePoseElements;  // QA: --fire-elements burn+shock|all seeds the build so --fire-pose shows element/combo muzzles
    int   poseStep = 0;
    int   forcedBiome = -1;    // M4 dev: --biome N forces Foundry(0)/Furnace(1)/Reliquary(2) for capture
    std::string forcedRoom;    // dev/QA: --room <name> forces a specific room template (+ its biome) for capture
    bool  topDownCapture = false;  // dev/QA: --topdown near-overhead capture camera
    std::string screenshotPath, recordDir, recordAudioPath, renderPass, captureLabel;
    std::string forceWeapon, forceEnemy, menuPose, inspectMesh;
    std::string musicWavPath, musicStateName = "combat", musicBiomeName = "foundry";
    std::string musicStingerWavPath, musicStingerName;
    std::string sfxWavPath, weaponEventWavPath, weaponEventName, enemyEventWavPath, enemyEventName, bakeAudioDir, importSrc, importDst;
    std::string feedbackEventWavPath, feedbackEventName;
    std::string pathTracePath;
    int ptSamples = 256, ptBounces = 4;
    float musicWavSeconds = 0.0f, musicStingerWavSeconds = 0.0f, musicIntensity = 0.85f, sfxWavSeconds = 0.0f, weaponEventWavSeconds = 0.0f, enemyEventWavSeconds = 0.0f, recordFps = 6.0f;
    bool musicOverpulse = false;
    // v4: providing either flag opts the render into the v4 path (duress submerge + heartbeat,
    // boss escalation). --music-trace dumps the per-frame music context during a --bot-test run.
    float musicDuress = 0.0f, musicBossEscalation = 0.0f;
    bool musicV4Render = false;
    std::string musicTracePath;
    float feedbackEventWavSeconds = 0.0f;
    float emitterX = 0.0f, emitterY = 0.0f; bool emitterSet = false;   // --emitter: spatial render placement

    // --sim-mods "EnemyDamagePct:0.5,ScrapPct:0.3": force a RunMods set into every sim run
    // so the M0 foundation can be asserted (win-rate moves the expected direction). Heat
    // (M4) reuses the same run-start seeding path. Whitespace in the spec is ignored.
    std::vector<RunModifier> simMods;
    int simHeat = -1;   // --sim-heat N: force a heat level into every sim run (M4 per-heat curve)
    auto parseSimMods = [](std::string spec) {
        std::vector<RunModifier> out;
        spec.erase(std::remove(spec.begin(), spec.end(), ' '), spec.end());
        size_t pos = 0;
        while (pos < spec.size()) {
            const size_t comma = spec.find(',', pos);
            const std::string tok = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            pos = (comma == std::string::npos) ? spec.size() : comma + 1;
            const size_t colon = tok.find(':');
            if (colon == std::string::npos) continue;
            const ModKind kind = modKindFromString(tok.substr(0, colon));
            if (kind == ModKind::Count) continue;
            out.push_back(RunModifier{ kind, std::stof(tok.substr(colon + 1)), "sim" });
        }
        return out;
    };
    auto lowerCopy = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    auto parseMusicState = [&](std::string s) {
        s = lowerCopy(s);
        if (s == "silent") return MusicState::Silent;
        if (s == "hub") return MusicState::Hub;
        if (s == "reward") return MusicState::Reward;
        if (s == "boss") return MusicState::Boss;
        if (s == "runover" || s == "run_over") return MusicState::RunOver;
        return MusicState::Combat;
    };
    auto parseMusicBiome = [&](std::string s) {
        s = lowerCopy(s);
        if (s == "1" || s == "furnace" || s == "forest") return MusicBiome::Furnace;
        if (s == "2" || s == "reliquary" || s == "ruins") return MusicBiome::Reliquary;
        return MusicBiome::Foundry;
    };
    auto parseMusicStinger = [&](std::string s) {
        s = lowerCopy(s);
        if (s == "reward") return MusicStingerType::Reward;
        if (s == "boss_intro" || s == "bossintro") return MusicStingerType::BossIntro;
        if (s == "overpulse") return MusicStingerType::Overpulse;
        if (s == "run_win" || s == "runwin" || s == "win") return MusicStingerType::RunWin;
        if (s == "run_lose" || s == "runlose" || s == "lose" || s == "loss") return MusicStingerType::RunLose;
        if (s == "sector_foundry" || s == "foundry") return MusicStingerType::SectorFoundry;
        if (s == "sector_furnace" || s == "furnace") return MusicStingerType::SectorFurnace;
        if (s == "sector_reliquary" || s == "reliquary") return MusicStingerType::SectorReliquary;
        if (s == "boss_phase" || s == "bossphase") return MusicStingerType::BossPhase;
        if (s == "boss_enrage" || s == "bossenrage" || s == "enrage") return MusicStingerType::BossEnrage;
        if (s == "anticipation" || s == "anticipate") return MusicStingerType::Anticipation;
        return MusicStingerType::RoomClear;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--gpu-info") gpuInfo = true;
        else if (a == "--force-raster") forceRaster = true;
        else if (a == "--quality" && i + 1 < argc) {
            const std::string q = argv[++i];
            if      (q == "low")    qualityOverride = 0;
            else if (q == "medium") qualityOverride = 1;
            else if (q == "high")   qualityOverride = 2;
            else if (q == "ultra")  qualityOverride = 3;
        }
        else if (a == "--smoke") smoke = true;
        else if (a == "--pose") pose = true;
        else if (a == "--reward-pose") rewardPose = true;
        else if (a == "--hub-pose") hubPose = true;
        else if (a == "--path-pose") pathPose = true;
        else if (a == "--doors-pose") doorsPose = true;
        else if (a == "--codex-pose") codexPose = true;
        else if (a == "--codex-tab" && i + 1 < argc) codexTab = std::stoi(argv[++i]);
        else if (a == "--enemy-tracer-pose") enemyTracerPose = true;
        else if (a == "--shop-pose") shopPose = true;
        else if (a == "--event-pose") eventPose = true;
        else if (a == "--runover-pose") runOverPose = true;
        else if (a == "--cover-pose") coverPose = true;
        else if (a == "--boss-pose") bossPose = true;
        else if (a == "--menu-pose" && i + 1 < argc) menuPose = argv[++i];
        else if (a == "--reload-pose" && i + 1 < argc) { reloadPose = true; reloadPoseProgress = std::stof(argv[++i]); }
        else if (a == "--fire-pose" && i + 1 < argc) { firePose = true; firePosePitch = std::stof(argv[++i]); }
        else if (a == "--fire-elements" && i + 1 < argc) firePoseElements = argv[++i];
        else if (a == "--pose-step" && i + 1 < argc) poseStep = std::stoi(argv[++i]);
        else if (a == "--weapon" && i + 1 < argc) forceWeapon = argv[++i];
        else if (a == "--enemy" && i + 1 < argc) forceEnemy = argv[++i];
        else if (a == "--biome" && i + 1 < argc) forcedBiome = std::stoi(argv[++i]);  // M4: force a biome (0/1/2) for capture
        else if (a == "--room" && i + 1 < argc) forcedRoom = argv[++i];                // dev/QA: force a named room template for capture
        else if (a == "--topdown") topDownCapture = true;                              // dev/QA: near-overhead capture camera
        else if (a == "--pathtrace" && i + 1 < argc) pathTracePath = argv[++i];
        else if (a == "--pt-spp" && i + 1 < argc) ptSamples = std::stoi(argv[++i]);
        else if (a == "--pt-bounces" && i + 1 < argc) ptBounces = std::stoi(argv[++i]);
        else if (a == "--bot-test" && i + 1 < argc) botTestSeconds = std::stof(argv[++i]);
        else if (a == "--marketing-bot") marketingBot = true;
        else if (a == "--balance-sim" && i + 1 < argc) balanceSimRuns = std::stoi(argv[++i]);
        else if (a == "--sim-mods" && i + 1 < argc) simMods = parseSimMods(argv[++i]);
        else if (a == "--sim-heat" && i + 1 < argc) simHeat = std::stoi(argv[++i]);
        else if (a == "--screenshot" && i + 1 < argc) screenshotPath = argv[++i];
        else if (a == "--render-pass" && i + 1 < argc) renderPass = argv[++i];
        else if (a == "--record-dir" && i + 1 < argc) recordDir = argv[++i];
        else if (a == "--record-fps" && i + 1 < argc) recordFps = std::stof(argv[++i]);
        else if (a == "--record-audio" && i + 1 < argc) recordAudioPath = argv[++i];
        else if (a == "--capture-label" && i + 1 < argc) captureLabel = argv[++i];
        else if (a == "--render-music" && i + 2 < argc) { musicWavPath = argv[++i]; musicWavSeconds = std::stof(argv[++i]); }
        else if (a == "--music-state" && i + 1 < argc) musicStateName = argv[++i];
        else if (a == "--music-biome" && i + 1 < argc) musicBiomeName = argv[++i];
        else if (a == "--music-intensity" && i + 1 < argc) musicIntensity = std::stof(argv[++i]);
        else if (a == "--music-overpulse") musicOverpulse = true;
        else if (a == "--music-duress" && i + 1 < argc) { musicDuress = std::stof(argv[++i]); musicV4Render = true; }
        else if (a == "--music-boss-escalation" && i + 1 < argc) { musicBossEscalation = std::stof(argv[++i]); musicV4Render = true; }
        else if (a == "--music-trace" && i + 1 < argc) musicTracePath = argv[++i];
        else if (a == "--render-music-stinger" && i + 3 < argc) { musicStingerName = argv[++i]; musicStingerWavPath = argv[++i]; musicStingerWavSeconds = std::stof(argv[++i]); }
        else if (a == "--render-sfx" && i + 2 < argc) { sfxWavPath = argv[++i]; sfxWavSeconds = std::stof(argv[++i]); }
        else if (a == "--render-weapon-event" && i + 3 < argc) { weaponEventName = argv[++i]; weaponEventWavPath = argv[++i]; weaponEventWavSeconds = std::stof(argv[++i]); }
        else if (a == "--render-enemy-event" && i + 3 < argc) { enemyEventName = argv[++i]; enemyEventWavPath = argv[++i]; enemyEventWavSeconds = std::stof(argv[++i]); }
        else if (a == "--emitter" && i + 2 < argc) { emitterX = std::stof(argv[++i]); emitterY = std::stof(argv[++i]); emitterSet = true; }
        else if (a == "--render-feedback-event" && i + 3 < argc) { feedbackEventName = argv[++i]; feedbackEventWavPath = argv[++i]; feedbackEventWavSeconds = std::stof(argv[++i]); }
        else if (a == "--bake-audio") { bakeAudioDir = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "assets/audio"; }
        else if (a == "--import-asset" && i + 2 < argc) { importSrc = argv[++i]; importDst = argv[++i]; }
        else if (a == "--inspect-mesh" && i + 1 < argc) inspectMesh = argv[++i];
        else if (a == "--import-assets-all") importAll = true;
        else if (a == "--asset-selftest") assetSelftest = true;
        else if (a == "--content-dump") contentDump = true;
        else if (a == "--dump-weapon-profiles") dumpWeaponProfiles = true;
        else if (a == "--weapon-test") weaponTest = true;
        else if (a == "--capture-weapon-matrix") captureWeaponMatrix = true;
    }

    auto resolveOutputPath = [&](std::string& path) {
        if (path.empty()) return;
        const std::filesystem::path p(path);
        if (p.is_relative()) path = (launchDir / p).lexically_normal().string();
    };
    resolveOutputPath(screenshotPath);
    resolveOutputPath(pathTracePath);
    resolveOutputPath(recordDir);
    resolveOutputPath(recordAudioPath);
    resolveOutputPath(musicTracePath);
    resolveOutputPath(musicWavPath);
    resolveOutputPath(musicStingerWavPath);
    resolveOutputPath(sfxWavPath);
    resolveOutputPath(weaponEventWavPath);
    resolveOutputPath(enemyEventWavPath);
    resolveOutputPath(feedbackEventWavPath);
    resolveOutputPath(importSrc);
    resolveOutputPath(importDst);

    // The game has one arena mode (brutalist); only the biome override remains for capture.
    game.setForcedBiome(forcedBiome);
    game.setForcedRoom(forcedRoom);   // dev/QA: --room <name> builds a specific template for inspection
    game.setTopDownCapture(topDownCapture);   // dev/QA: --topdown near-overhead inspection camera
    game.setMarketingBot(marketingBot);        // capture-only bot profile for cleaner marketing footage

    // W6 quality ladder (doc 14): CLI --quality overrides the persisted preset; otherwise
    // use the saved one (default High). Low/Medium run the raster tier (Low also drops
    // SSGI + SSR -> flat ambient + analytic reflections); High/Ultra request the RT tier
    // (the device caps fall back to raster on a non-RT GPU). --force-raster still forces
    // raster. The art style (bands, outlines, hatching, palette, enemy cores) holds at
    // every preset. Reading is side-effect-free; persistence belongs to the Options menu.
    Settings gfxSettings;
    gfxSettings.load();
    const int quality = (qualityOverride >= 0) ? qualityOverride : gfxSettings.graphicsQuality;
    const bool enableSsgi = (quality != 0);   // Medium+ : screen-space GI (raster tier)
    const bool enableSsr = (quality != 0);    // Medium+ : screen-space reflections (raster tier)
    if (quality <= 1) forceRaster = true;     // Low + Medium = raster tier; High/Ultra = RT
    const uint32_t rtRayCount = (quality >= 3) ? 6u : 3u;  // Ultra: fuller, cleaner RT GI (more rays/pixel)

    // Offline asset import (OBJ -> GLB, or image -> BCn DDS). No device needed.
    if (!importSrc.empty()) {
        const bool ok = importAsset(importSrc, importDst);
        return ok ? 0 : 2;
    }

    // Inspect a glTF/GLB: print triangle count + the axis-aligned bounds. CPU-only (no
    // device). Used to derive the per-asset import transform (Meshy/Blender outputs vary
    // wildly in scale + orientation, the R5 gotcha) BEFORE binding a hero mesh into a
    // scene, so the placement is right the first time instead of tuned blind.
    if (!inspectMesh.empty()) {
        const std::string path = resolveUp(inspectMesh);
        std::vector<StaticVertex> verts; std::vector<uint32_t> indices;
        if (!loadGltfMesh(path, verts, indices) || verts.empty()) {
            std::printf("[inspect] FAILED to load %s\n", path.c_str());
            return 2;
        }
        Vec3f mn = verts[0].pos, mx = verts[0].pos;
        for (const StaticVertex& v : verts) {
            mn.x = std::min(mn.x, v.pos.x); mn.y = std::min(mn.y, v.pos.y); mn.z = std::min(mn.z, v.pos.z);
            mx.x = std::max(mx.x, v.pos.x); mx.y = std::max(mx.y, v.pos.y); mx.z = std::max(mx.z, v.pos.z);
        }
        const Vec3f size{ mx.x - mn.x, mx.y - mn.y, mx.z - mn.z };
        const Vec3f ctr{ (mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f };
        const float maxDim = std::max(size.x, std::max(size.y, size.z));
        std::printf("[inspect] %s\n", path.c_str());
        std::printf("  verts=%zu  tris=%zu\n", verts.size(), indices.size() / 3);
        std::printf("  min   = (%.4f, %.4f, %.4f)\n", mn.x, mn.y, mn.z);
        std::printf("  max   = (%.4f, %.4f, %.4f)\n", mx.x, mx.y, mx.z);
        std::printf("  size  = (%.4f, %.4f, %.4f)  maxDim=%.4f\n", size.x, size.y, size.z, maxDim);
        std::printf("  center= (%.4f, %.4f, %.4f)\n", ctr.x, ctr.y, ctr.z);
        if (maxDim > 1e-6f)
            std::printf("  scale-to-3m = %.5f   (multiply model matrix to make the long axis ~3 world units)\n", 3.0f / maxDim);
        return 0;
    }

    // Transcode every sourced PBR texture under assets/external to BCn DDS (build
    // step). Walks the tree so new asset sets (arena CC0 sets, hero assets) are all
    // covered; importAsset picks BC7/BC5 + colour space from the filename. Idempotent:
    // skips a map whose .dds is already newer than its source.
    if (importAll) {
        namespace fs = std::filesystem;
        const std::string root = resolveUp("assets/external");
        bool ok = true; int done = 0, skipped = 0;
        if (fs::exists(root)) {
            for (const auto& e : fs::recursive_directory_iterator(root)) {
                if (!e.is_regular_file()) continue;
                const fs::path p = e.path();
                std::string ext = p.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](char c){ return static_cast<char>(std::tolower(c)); });
                if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".tga") continue;
                const fs::path dst = p.parent_path() / (p.stem().string() + ".dds");
                std::error_code ec;
                if (fs::exists(dst) && fs::last_write_time(dst, ec) >= fs::last_write_time(p, ec)) { ++skipped; continue; }
                ok = importAsset(p.string(), dst.string()) && ok; ++done;
            }
        }
        std::printf("transcoded %d texture maps to DDS (%d up-to-date)\n", done, skipped);
        return ok ? 0 : 2;
    }

    // Bake the required sample assets from the synth (no device needed).
    if (!bakeAudioDir.empty()) {
        AudioSystem audio(false);
        const bool ok = audio.bakeSamples(bakeAudioDir);
        std::printf("%s\n", ok ? "Audio bake ok" : "Audio bake failed");
        return ok ? 0 : 2;
    }

    // Offline audio render (no device / no engine needed).
    if (!musicStingerWavPath.empty()) {
        AudioSystem audio(false);
        const MusicStingerType type = parseMusicStinger(musicStingerName);
        const bool quantized = !(type == MusicStingerType::RunWin || type == MusicStingerType::RunLose);
        const bool ok = audio.renderMusicStingerToWav(musicStingerWavPath, musicStingerWavSeconds,
                                                       tn.technoBpm, tn.technoBaseVolume, type,
                                                       parseMusicBiome(musicBiomeName), quantized);
        std::printf("%s\n", ok ? "Music stinger render ok" : "Music stinger render failed");
        return ok ? 0 : 2;
    }
    if (!musicWavPath.empty()) {
        AudioSystem audio(false);
        const MusicState mstate = parseMusicState(musicStateName);
        const MusicBiome mbiome = parseMusicBiome(musicBiomeName);
        const bool ok = musicV4Render
            ? audio.renderMusicToWav(musicWavPath, musicWavSeconds, tn.technoBpm, tn.technoBaseVolume,
                                     musicIntensity, mstate, mbiome, musicOverpulse, musicDuress, musicBossEscalation)
            : audio.renderMusicToWav(musicWavPath, musicWavSeconds, tn.technoBpm, tn.technoBaseVolume,
                                     musicIntensity, mstate, mbiome, musicOverpulse);
        std::printf("%s\n", ok ? "Music render ok" : "Music render failed");
        return ok ? 0 : 2;
    }
    if (!sfxWavPath.empty()) {
        AudioSystem audio(false);
        float sfxFireRate = tn.weaponFireRate;
        float sfxVolume = 0.95f;
        if (!forceWeapon.empty()) {
            if (const WeaponProfile* profile = game.weaponProfiles().find(forceWeapon)) {
                sfxFireRate = profile->fireRate;
                sfxVolume = profile->fireVolume;
            }
        }
        const bool ok = audio.renderShotsToWav(sfxWavPath, sfxWavSeconds, SoundEventType::Fire, sfxFireRate, sfxVolume, forceWeapon);
        std::printf("%s\n", ok ? "Sfx render ok" : "Sfx render failed");
        return ok ? 0 : 2;
    }
    if (!weaponEventWavPath.empty()) {
        if (forceWeapon.empty()) {
            std::fprintf(stderr, "--render-weapon-event requires --weapon <id>\n");
            return 2;
        }
        WeaponEventType event = WeaponEventType::ReloadStart;
        if (!parseWeaponEventType(weaponEventName, event)) {
            std::fprintf(stderr, "unknown weapon event: %s\n", weaponEventName.c_str());
            return 2;
        }
        AudioSystem audio(false);
        const bool ok = audio.renderWeaponEventToWav(weaponEventWavPath, weaponEventWavSeconds, forceWeapon, event, 1.6f, 1.0f);
        std::printf("%s\n", ok ? "Weapon event render ok" : "Weapon event render failed");
        return ok ? 0 : 2;
    }
    if (!enemyEventWavPath.empty()) {
        if (forceEnemy.empty()) {
            std::fprintf(stderr, "--render-enemy-event requires --enemy <id>\n");
            return 2;
        }
        EnemyEventType event = EnemyEventType::Telegraph;
        if (!parseEnemyEventType(enemyEventName, event)) {
            std::fprintf(stderr, "unknown enemy event: %s\n", enemyEventName.c_str());
            return 2;
        }
        AudioSystem audio(false);
        const bool ok = audio.renderEnemyEventToWav(enemyEventWavPath, enemyEventWavSeconds, forceEnemy, event, 1.8f, 1.0f,
                                                    emitterSet, emitterX, emitterY);
        std::printf("%s\n", ok ? "Enemy event render ok" : "Enemy event render failed");
        return ok ? 0 : 2;
    }
    if (!feedbackEventWavPath.empty()) {
        FeedbackEventType event = FeedbackEventType::Hitmarker;
        if (!parseFeedbackEventType(feedbackEventName, event)) {
            std::fprintf(stderr, "unknown feedback event: %s\n", feedbackEventName.c_str());
            return 2;
        }
        AudioSystem audio(false);
        const bool ok = audio.renderFeedbackEventToWav(feedbackEventWavPath, feedbackEventWavSeconds, event, 2.0f, 1.0f);
        std::printf("%s\n", ok ? "Feedback event render ok" : "Feedback event render failed");
        return ok ? 0 : 2;
    }

    if (gpuInfo) {
        Engine engine; engine.init(headlessConfig(tn, forceRaster));
        std::printf("%s", engine.device().gpuInfoString().c_str());
        return 0;
    }

    // Dump the resolved content catalog (built-ins + config/pulse.content) to verify
    // the data-driven pool. No engine needed.
    if (contentDump) {
        Build b;
        std::printf("Content: %zu passives, %zu weapons (built-ins + config/pulse.content)\n",
                    b.catalog().size(), b.weaponCatalog().size());
        for (const ItemDef& d : b.catalog())
            std::printf("  passive  %-16s [%-8s] %s\n", d.id.c_str(), tierName(d.tier), d.name.c_str());
        for (const WeaponDef& w : b.weaponCatalog())
            std::printf("  weapon   %-16s [%-8s] %s\n", w.id.c_str(), tierName(w.tier), w.name.c_str());
        return 0;
    }

    if (dumpWeaponProfiles) {
        std::printf("%s", game.weaponProfiles().dump().c_str());
        return 0;
    }

    if (weaponTest) {
        std::string report;
        const bool ok = game.runWeaponSelfTest(report);
        std::printf("%s", report.c_str());
        return ok ? 0 : 3;
    }

    // Balance simulation: exploit the determinism + headless bot to sim many seeded
    // runs (no GPU/engine - update() is pure CPU) and report run-clear distributions.
    // This is the automated signal for bullet-sponge drift / degenerate builds the
    // plan calls the dominant risk. Stats are read at each run-end (pre-reset).
    if (balanceSimRuns > 0) {
        AudioSystem audio(false);
        InputState input;
        if (!simMods.empty()) game.setForcedMods(simMods);   // M0 assert: force a RunMods set
        if (simHeat >= 0) game.setSimHeat(simHeat);          // M4: force a heat level (bypasses the unlock clamp)
        game.resetForSim();
        const int w = tn.windowWidth, h = tn.windowHeight;
        int maxRoomCount = std::max(1, game.runRoomCount());
        std::vector<int> roomHist(static_cast<size_t>(maxRoomCount) + 1, 0); // furthest room reached (1-based)

        int recorded = 0, wins = 0, bossReached = 0, timeouts = 0;
        long long roomsSum = 0, plannedRoomsSum = 0, scoreSum = 0, durSum = 0, scrapSum = 0;
        int prevEnded = 0, prevWon = 0, prevBoss = 0, runStartFrame = 0;
        int frame = 0;
        // Cap each run so a stuck/endless run is abandoned, but scale it with heat (heat makes
        // runs longer) and keep it generous so a slow WINNING run is not cut off and miscounted
        // as a loss (the M0 review's timeout-vs-death fix; win-rate is also reported over
        // FINISHED runs, excluding timeouts).
        const int heatForCap = simHeat >= 0 ? simHeat : 0;
        const int worstCaseRoomCount = std::max(maxRoomCount,
            3 * (std::clamp(tn.runMinRoomsBeforeBoss, 2, 12) +
                 std::clamp(tn.runExtraRoomsBeforeBoss, 0, 6) + 1));
        const int perRunCap = (80 + 25 * worstCaseRoomCount + 40 * heatForCap) * 60;   // scales with route length so slow wins are not cut off
        const long long maxFrames = static_cast<long long>(balanceSimRuns) * perRunCap + 600000;

        auto record = [&](int rooms, bool won, bool reachedBoss, bool timedOut) {
            const int plannedRooms = std::max(1, game.runRoomCount());
            maxRoomCount = std::max(maxRoomCount, plannedRooms);
            if (static_cast<int>(roomHist.size()) <= maxRoomCount) roomHist.resize(static_cast<size_t>(maxRoomCount) + 1, 0);
            ++recorded;
            if (won) ++wins;
            if (reachedBoss) ++bossReached;
            if (timedOut) ++timeouts;
            roomsSum += rooms;
            plannedRoomsSum += plannedRooms;
            scoreSum += game.score();
            scrapSum += game.runScrap();
            durSum += (frame - runStartFrame);
            if (rooms >= 1 && rooms < static_cast<int>(roomHist.size())) roomHist[static_cast<size_t>(rooms)]++;
            runStartFrame = frame;
        };

        while (recorded < balanceSimRuns && frame < maxFrames) {
            const float elapsed = static_cast<float>(frame) / 60.0f;
            game.buildBotInput(input, elapsed);
            game.update(input, audio, 1.0f / 60.0f, w, h);
            input.endFrame();
            ++frame;
            if (game.runsEnded() > prevEnded) {              // a run ended (death/win) this frame
                record(game.runRoomIndex() + 1, game.runsWon() > prevWon, game.bossesReached() > prevBoss, false);
                prevEnded = game.runsEnded();
                prevWon = game.runsWon();
                prevBoss = game.bossesReached();
            } else if (frame - runStartFrame > perRunCap) {  // stuck / survived too long -> abandon
                const bool reachedBoss = game.bossesReached() > prevBoss;
                record(game.runRoomIndex() + 1, false, reachedBoss, true);
                game.abandonRun();
                prevBoss = game.bossesReached();
            }
        }

        const double n = std::max(1, recorded);
        std::printf("Balance sim: runs=%d frames=%d (fresh in-memory profile, cumulative meta)\n", recorded, frame);
        if (simHeat >= 0) std::printf("  forced heat      : %d\n", simHeat);
        if (!simMods.empty()) {
            std::printf("  forced mods      :");
            for (const RunModifier& m : simMods) std::printf(" %s=%+.2f", modKindName(m.kind), m.value);
            std::printf("\n");
        }
        const int finished = std::max(1, recorded - timeouts);   // runs that ended in win/death
        std::printf("  win rate         : %.1f%%  (%d/%d overall)\n", 100.0 * wins / n, wins, recorded);
        std::printf("  win rate finished: %.1f%%  (%d/%d, timeouts excluded)\n", 100.0 * wins / finished, wins, finished);
        std::printf("  reached a boss   : %.1f%%  (%d/%d)\n", 100.0 * bossReached / n, bossReached, recorded);
        std::printf("  timed out        : %.1f%%  (%d/%d, cap %ds)\n", 100.0 * timeouts / n, timeouts, recorded, perRunCap / 60);
        std::printf("  avg furthest room: %.2f / %.2f planned\n", roomsSum / n, plannedRoomsSum / n);
        std::printf("  avg score        : %.1f\n", scoreSum / n);
        std::printf("  avg scrap earned : %.1f\n", scrapSum / n);
        std::printf("  avg run cycle    : %.1fs (approx)\n", durSum / n / 60.0);
        // M1 Pulse: mean momentum + how much combat time was spent hot (the greed signal).
        std::printf("  avg pulse (combat): %.2f\n", game.avgPulse());
        std::printf("  pulse time        : Cold=%.0f%% Warm=%.0f%% Hot=%.0f%% Burning=%.0f%% Over=%.0f%%\n",
                    100.0 * game.pulseTierFraction(0), 100.0 * game.pulseTierFraction(1),
                    100.0 * game.pulseTierFraction(2), 100.0 * game.pulseTierFraction(3),
                    100.0 * game.pulseTierFraction(4));
        std::printf("  status uptime     : %.0f%% of enemy-frames carry an element (M2)\n", 100.0 * game.statusUptime());
        // Room-type mix entered across the sim (Feature 1-3 telemetry).
        static const char* kTypeNames[6] = { "Combat", "Elite", "Cache", "Shop", "Event", "Boss" };
        std::printf("  rooms entered by type:");
        for (int t = 0; t < 6; ++t) std::printf(" %s=%d", kTypeNames[t], game.roomTypeEntered(t));
        std::printf("\n");
        std::printf("  furthest-room histogram:\n");
        for (int r = 1; r < static_cast<int>(roomHist.size()); ++r)
            std::printf("    room %d: %d\n", r, roomHist[static_cast<size_t>(r)]);
        return 0;
    }

    if (assetSelftest) {
        bool ok = true;
        std::string weaponReport;
        const bool weaponOk = game.runWeaponSelfTest(weaponReport);
        std::printf("%s", weaponReport.c_str());
        ok = ok && weaponOk;
        // 1. Mesh round-trip: sourced weapon glTF -> GLB (writeGlb) -> cgltf (loadGltfMesh).
        auto testWeaponMesh = [&](const std::string& rel, const std::string& tag) {
            std::vector<StaticVertex> ov, gv; std::vector<uint32_t> oi, gi;
            const std::string src = resolveUp(rel);
            const std::string glb = (std::filesystem::temp_directory_path() / ("pulse_" + tag + "_asset_test.glb")).string();
            const bool meshOk = loadGltfMesh(src, ov, oi) && writeGlb(glb, ov, oi) && loadGltfMesh(glb, gv, gi);
            const bool pass = meshOk && gv.size() == ov.size() && gi.size() == oi.size();
            std::printf("[selftest] mesh %s glTF(%zu v,%zu i) -> GLB -> cgltf(%zu v,%zu i): %s\n",
                        tag.c_str(), ov.size(), oi.size(), gv.size(), gi.size(), pass ? "PASS" : "FAIL");
            return pass;
        };
        ok = testWeaponMesh("assets/bumstrum/fps_ak_animated/scene.gltf", "ak47") && ok;
        ok = testWeaponMesh("assets/bumstrum/fps_animated_carbine/scene.gltf", "carbine") && ok;

        // 2. Texture import (texconv -> BCn DDS) + DDS load (uploadDDS).
        Engine engine; engine.init(headlessConfig(tn, forceRaster));
        auto testTex = [&](const std::string& png, const std::string& tag) {
            const std::string dds = (std::filesystem::temp_directory_path() / (tag + ".dds")).string();
            const bool imported = importAsset(resolveUp(png), dds);
            const TextureHandle h = imported ? engine.createTextureDDS(dds) : TextureHandle::Invalid;
            const bool pass = imported && h != TextureHandle::Invalid;
            std::printf("[selftest] tex %s -> DDS -> load: %s\n", tag.c_str(), pass ? "PASS" : "FAIL");
            return pass;
        };
        ok = testTex("assets/external/polyhaven/concrete_floor_02/concrete_floor_02_diff_1k.png", "diff") && ok;
        ok = testTex("assets/external/polyhaven/concrete_floor_02/concrete_floor_02_nor_gl_1k.png", "nor") && ok;
        std::printf("[selftest] %s\n", ok ? "ALL PASS" : "FAILURES");
        return ok ? 0 : 3;
    }

    const bool headless = pose || reloadPose || firePose || rewardPose || hubPose || pathPose || doorsPose || enemyTracerPose || shopPose || eventPose || coverPose || bossPose || runOverPose || !menuPose.empty() || smoke || captureWeaponMatrix || botTestSeconds > 0.0f || !screenshotPath.empty() ||
                          !recordDir.empty() || !recordAudioPath.empty() || !renderPass.empty() || !pathTracePath.empty();
    if (!headless) {
        return runWindowed(game, forceRaster, enableSsgi, enableSsr, rtRayCount);
    }

    // --- headless capture paths ---
    Engine engine; engine.init(headlessConfig(tn, forceRaster, enableSsgi, enableSsr, rtRayCount));
    AudioSystem audio(recordAudioPath.empty());
    if (!recordAudioPath.empty()) {
        const std::filesystem::path wavPath(recordAudioPath);
        if (wavPath.has_parent_path()) std::filesystem::create_directories(wavPath.parent_path());
        audio.beginOfflineCapture(48000);
    }
    InputState input;
    const int w = static_cast<int>(engine.width()), h = static_cast<int>(engine.height());

    if (captureWeaponMatrix) {
        namespace fs = std::filesystem;
        const fs::path dir = launchDir / "build" / "weapon_matrix";
        fs::create_directories(dir);
        bool ok = true;
        int captured = 0;
        for (const WeaponProfile& profile : game.weaponProfiles().profiles()) {
            if (profile.id != "pistol" && !game.weaponProfiles().rewardEligible(profile.id)) {
                std::printf("[weapon-matrix] skip %s (locked: missing authored profile asset)\n", profile.id.c_str());
                continue;
            }
            auto capture = [&](const std::string& suffix) {
                Image img;
                const SceneFrame& frame = game.buildFrame(engine, w, h);
                const fs::path out = dir / (profile.id + "_" + suffix + ".bmp");
                const bool wrote = engine.captureFrame(frame, img) && img.saveBmp(out.string());
                std::printf("[weapon-matrix] %s %s\n", out.string().c_str(), wrote ? "PASS" : "FAIL");
                ok = ok && wrote;
                ++captured;
            };

            game.debugForceWeapon(profile.id);
            game.debugPose();
            capture("idle");
            game.debugFire(audio, 0.0f);
            capture("fire");
            for (float p : { 0.25f, 0.50f, 0.72f, 0.95f }) {
                game.debugForceWeapon(profile.id);
                game.debugReloadPose(p);
                char label[32];
                std::snprintf(label, sizeof(label), "reload_%02d", static_cast<int>(std::lround(p * 100.0f)));
                capture(label);
            }
        }
        std::printf("[weapon-matrix] captured=%d dir=%s\n", captured, dir.string().c_str());
        return ok ? 0 : 3;
    }

    // Headless pose helpers need the generated environment/player spawn before they place
    // enemies relative to the camera; ensureGpuResources builds that environment.
    const bool forcedRoomEnvStill = !forcedRoom.empty() && topDownCapture && !screenshotPath.empty() &&
                                    botTestSeconds <= 0.0f && recordDir.empty();
    if (pose || firePose || bossPose || reloadPose || coverPose || doorsPose || enemyTracerPose || forcedRoomEnvStill) {
        game.buildFrame(engine, w, h);
    }
    if (forcedRoomEnvStill) {
        game.debugBeginScriptedCapture();
    }

    if (!forceWeapon.empty()) {
        game.debugForceWeapon(forceWeapon);
    }
    if (pose) {
        game.debugPose();
        if (poseStep > 0) { game.debugKillAll(); for (int s = 0; s < poseStep; ++s) game.update(input, audio, 1.0f / 60.0f, w, h); }
    }
    if (firePose) {
        if (!pose) {
            game.debugPose();
        }
        if (!firePoseElements.empty()) game.debugSeedShotElements(firePoseElements);
        game.debugFire(audio, firePosePitch);
    }
    if (rewardPose) {
        game.debugRewardScreen();
    }
    if (hubPose) {
        game.debugHubScreen();
    }
    if (pathPose) {
        game.debugPathScreen();
    }
    if (doorsPose) {
        game.debugDoorsScreen();
    }
    if (codexPose) {
        game.debugCodexScreen(codexTab);
    }
    if (enemyTracerPose) {
        game.debugEnemyTracerPose();
    }
    if (shopPose) {
        game.debugShopScreen();
    }
    if (eventPose) {
        game.debugEventScreen();
    }
    if (runOverPose) {
        game.debugRunOverScreen();
    }
    if (coverPose) {
        game.debugStandOnCover();
    }
    if (bossPose) {
        game.debugBossPose();
    }
    if (!menuPose.empty()) {
        game.debugMenuScreen(menuPose);
    }

    // M3 reference path-traced still: accumulate many samples and AgX-resolve.
    if (!pathTracePath.empty()) {
        Image img;
        const SceneFrame& frame = game.buildFrame(engine, w, h);
        engine.capturePathTraced(frame, static_cast<uint32_t>(std::max(1, ptSamples)),
                                 static_cast<uint32_t>(std::max(1, ptBounces)), img);
        if (!img.saveBmp(pathTracePath)) { std::fprintf(stderr, "could not write %s\n", pathTracePath.c_str()); return 2; }
        std::printf("path-traced %s (%d spp, %d bounces)\n", pathTracePath.c_str(), ptSamples, ptBounces);
        return 0;
    }
    if (reloadPose) {
        game.debugReloadPose(reloadPoseProgress);
    }

    if (!recordDir.empty()) std::filesystem::create_directories(recordDir);
    recordFps = recordFps < 1.0f ? 1.0f : recordFps;
    const float headlessHz = (botTestSeconds > 0.0f && (!recordDir.empty() || !recordAudioPath.empty()))
        ? std::max(60.0f, recordFps)
        : 60.0f;
    int frames = botTestSeconds > 0.0f
        ? static_cast<int>(std::ceil(botTestSeconds * headlessHz))
        : ((pose || reloadPose || firePose || rewardPose || hubPose || pathPose || doorsPose || enemyTracerPose || shopPose || eventPose || coverPose || bossPose || runOverPose || !menuPose.empty() || forcedRoomEnvStill) ? 0 : 120);
    // The bot loop below only steps update() (no render), so build one frame up front to
    // force ensureGpuResources - which loads the dungeon and generates the first room - so
    // --bot-test exercises the real environment instead of the unloaded box-arena fallback.
    // The loaded brutalist arena has spatial doors; the bot cannot walk through them, so it
    // resolves each door choice by policy instantly (matching the headless balance sim).
    if (botTestSeconds > 0.0f) { game.setAutoResolveDoors(true); game.buildFrame(engine, w, h); }
    if (!musicTracePath.empty()) game.setMusicTracePath(musicTracePath);   // v4 (S3): per-frame context CSV
    float nextRecord = 0.0f; int recordFrame = 0;

    for (int i = 0; i < frames; ++i) {
        const float elapsed = static_cast<float>(i) / headlessHz;
        if (botTestSeconds > 0.0f) game.buildBotInput(input, elapsed);
        game.update(input, audio, 1.0f / headlessHz, w, h);
        if (!recordAudioPath.empty()) audio.advanceOfflineCapture(1.0f / headlessHz);
        if (!recordDir.empty() && elapsed + 0.0001f >= nextRecord) {
            Image img;
            engine.captureFrame(game.buildFrame(engine, w, h), img);
            char name[64]; std::snprintf(name, sizeof(name), "frame_%04d.bmp", recordFrame++);
            img.saveBmp((std::filesystem::path(recordDir) / name).string());
            nextRecord += 1.0f / recordFps;
        }
        input.endFrame();
    }

    if (!screenshotPath.empty() || (recordDir.empty() && botTestSeconds <= 0.0f)) {
        Image img;
        const SceneFrame& frame = game.buildFrame(engine, w, h);
        if (!renderPass.empty()) engine.capturePass(frame, renderPass, img);
        else engine.captureFrame(frame, img);
        const std::string out = screenshotPath.empty() ? std::string("pulse_capture.bmp") : screenshotPath;
        if (!img.saveBmp(out)) { std::fprintf(stderr, "could not write %s\n", out.c_str()); return 2; }
        std::printf("captured %s%s\n", out.c_str(), renderPass.empty() ? "" : (" pass=" + renderPass).c_str());
    }

    if (!recordAudioPath.empty()) {
        if (!audio.writeOfflineCaptureWav(recordAudioPath)) {
            std::fprintf(stderr, "could not write audio capture %s\n", recordAudioPath.c_str());
            return 2;
        }
        std::printf("captured audio %s\n", recordAudioPath.c_str());
    }

    if (botTestSeconds > 0.0f) {
        std::printf("Bot playtest: seconds=%.1f score=%d best=%d hp=%d shield=%d enemies=%d\n",
                    botTestSeconds, game.score(), game.bestScore(), game.playerHp(), game.playerShield(), game.activeEnemyCount());
        std::printf("Run state: phase=%s room=%d/%d roomsCleared=%d bossesReached=%d runsEnded=%d runsWon=%d items=%d weapons=%d\n",
                    game.phaseName(), game.runRoomIndex() + 1, game.runRoomCount(),
                    game.roomsCleared(), game.bossesReached(), game.runsEnded(), game.runsWon(),
                    game.buildItemCount(), game.loadoutSize());
        std::printf("Meta: currency=%d unlocked=%d\n", game.metaCurrency(), game.metaUnlocked());
        if (game.score() < 0 || game.playerHp() < 0) return 3;
    }
    (void)captureLabel;
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    attachParentConsole();   // windows-subsystem app: show output only when run from a terminal
#endif
    installCrashHandler();   // unhandled-exception -> pulse_crash.log + pulse_crash.dmp
    try {
        const std::filesystem::path launchDir = std::filesystem::current_path();
        setWorkingDirectoryToExecutableFolder();
        return runPulse(argc, argv, launchDir);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 99;
    }
}
