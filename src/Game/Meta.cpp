#include "Game/Meta.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace pulse {
namespace {

constexpr const char* kSaveVersion = "pulse_save_v2";
constexpr const char* kLegacySaveVersion = "pulse_save_v1";

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::string migrateLegacyWeaponId(std::string id, bool legacyCarbineWasAk) {
    if (!legacyCarbineWasAk) return id;
    if (id == "carbine") return "ak47";
    if (id == "w:carbine") return "w:ak47";
    return id;
}

} // namespace

Meta::Meta() {
    // The gated content catalog: a few advanced items + weapons start locked and
    // drip in via meta-currency, expanding the run pools as choices (never stats).
    unlockables_ = {
        { "w:marksman", "Marksman (burst)", 15 },
        { "w:railbolt", "Railbolt (projectile)", 30 },
        { "p:frag_payload", "Frag Payload", 20 },
        { "p:arc_conductor", "Arc Conductor", 25 },
        { "p:volatile_rounds", "Volatile Rounds", 25 },
    };
}

namespace {
// Mirror node tables (parallel to Meta::MirrorNode). Costs escalate per level; all hard-capped.
struct MirrorDef { const char* name; const char* desc; int maxLevel; int baseCost; int costStep; };
const MirrorDef kMirror[Meta::MirrorCount] = {
    { "Vigor",      "+6 max health / level",        5, 15, 10 },
    { "Plating",    "+8 starting shield / level",   5, 12, 8  },
    { "Momentum",   "start with more Pulse / level",4, 20, 14 },
    { "Avarice",    "+8 starting scrap / level",    5, 12, 8  },
    { "Fortune",    "+reward tier bias / level",    4, 25, 16 },
    { "Adrenaline", "+6% ability charge / level",   4, 22, 14 },
};
} // namespace

int Meta::mirrorMax(int node) const { return (node >= 0 && node < MirrorCount) ? kMirror[node].maxLevel : 0; }

const char* Meta::mirrorName(int node) const { return (node >= 0 && node < MirrorCount) ? kMirror[node].name : ""; }
const char* Meta::mirrorDesc(int node) const { return (node >= 0 && node < MirrorCount) ? kMirror[node].desc : ""; }

int Meta::mirrorCost(int node) const {
    if (node < 0 || node >= MirrorCount) return -1;
    const int lvl = mirrorLevel(node);
    if (lvl >= kMirror[node].maxLevel) return -1;   // maxed
    return kMirror[node].baseCost + lvl * kMirror[node].costStep;
}

bool Meta::upgradeMirror(int node) {
    const int cost = mirrorCost(node);
    if (cost < 0 || currency_ < cost) return false;
    currency_ -= cost;
    ++mirror_[static_cast<size_t>(node)];
    return true;
}

bool Meta::isGated(const std::string& id) const {
    for (const UnlockDef& u : unlockables_) {
        if (u.id == id) return true;
    }
    return false;
}

bool Meta::isUnlocked(const std::string& id) const {
    return unlocked_.find(id) != unlocked_.end();
}

bool Meta::buy(const std::string& id) {
    if (!isGated(id) || isUnlocked(id)) return false;
    int cost = 0;
    for (const UnlockDef& u : unlockables_) {
        if (u.id == id) { cost = u.cost; break; }
    }
    if (currency_ < cost) return false;
    currency_ -= cost;
    unlocked_.insert(id);
    return true;
}

std::vector<std::string> Meta::lockedContent() const {
    std::vector<std::string> out;
    for (const UnlockDef& u : unlockables_) {
        if (!isUnlocked(u.id)) out.push_back(u.id);
    }
    return out;
}

std::vector<std::string> Meta::starterOptions() const {
    std::vector<std::string> out;
    out.push_back("pistol");                          // always available
    for (const UnlockDef& u : unlockables_) {
        if (u.id.rfind("w:", 0) == 0 && isUnlocked(u.id)) out.push_back(u.id.substr(2));
    }
    return out;
}

std::string Meta::savePath() {
    // %LOCALAPPDATA%\Pulse\profile.sav, falling back to the working dir if the env
    // var is missing (headless/CI). Directory is created on save.
    const char* base = std::getenv("LOCALAPPDATA");
    std::filesystem::path dir = base ? std::filesystem::path(base) / "Pulse" : std::filesystem::path(".");
    return (dir / "profile.sav").string();
}

void Meta::load() {
    // Always start from a clean fresh profile, then overlay any valid saved data.
    currency_ = 0;
    unlocked_.clear();
    startingWeapon_ = "pistol";
    heat_ = 0;
    maxHeatUnlocked_ = 0;
    mirror_.fill(0);
    contractMask_ = 0;

    std::ifstream f(savePath());
    if (!f.good()) return;                 // no profile yet -> fresh

    std::string version;
    if (!std::getline(f, version)) {
        return;                            // unknown/corrupt header -> fresh profile
    }
    version = trim(version);
    const bool legacyCarbineWasAk = (version == kLegacySaveVersion);
    if (version != kSaveVersion && !legacyCarbineWasAk) {
        return;                            // unknown/corrupt header -> fresh profile
    }

    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "currency") {
            try { currency_ = std::max(0, std::stoi(value)); } catch (...) { currency_ = 0; }
        } else if (key == "unlocked") {
            std::stringstream ss(value);
            std::string id;
            while (std::getline(ss, id, ',')) {
                const std::string t = trim(id);
                if (!t.empty()) unlocked_.insert(migrateLegacyWeaponId(t, legacyCarbineWasAk));
            }
        } else if (key == "starting") {
            if (!value.empty()) startingWeapon_ = migrateLegacyWeaponId(value, legacyCarbineWasAk);
        } else if (key == "maxheat") {
            try { maxHeatUnlocked_ = std::max(0, std::stoi(value)); } catch (...) { maxHeatUnlocked_ = 0; }
        } else if (key == "heat") {
            try { heat_ = std::max(0, std::stoi(value)); } catch (...) { heat_ = 0; }
        } else if (key == "contracts") {
            try { contractMask_ = static_cast<unsigned>(std::max(0L, std::stol(value))); } catch (...) { contractMask_ = 0; }
        } else if (key == "mirror") {
            // CSV of node levels (M6). Missing/short -> remaining nodes stay 0 (old saves degrade).
            std::stringstream ss(value);
            std::string tok; int i = 0;
            while (std::getline(ss, tok, ',') && i < MirrorCount) {
                try { mirror_[static_cast<size_t>(i)] = std::max(0, std::min(mirrorMax(i), std::stoi(trim(tok)))); }
                catch (...) { mirror_[static_cast<size_t>(i)] = 0; }
                ++i;
            }
        }
        // Unknown keys ignored -> a save written by an older build still loads.
    }
    if (maxHeatUnlocked_ > kMaxHeat) maxHeatUnlocked_ = kMaxHeat;
    if (heat_ > kMaxHeat) heat_ = kMaxHeat;   // heat is freely selectable (no ascension gate)
    if (heat_ < 0) heat_ = 0;
}

bool Meta::save() const {
    if (!persist_) return true;   // balance-sim: keep the run in memory, never touch disk
    const std::string path = savePath();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

    std::ofstream f(path, std::ios::trunc);
    if (!f.good()) return false;
    f << kSaveVersion << "\n";
    f << "currency=" << currency_ << "\n";
    f << "starting=" << startingWeapon_ << "\n";
    f << "heat=" << heat_ << "\n";
    f << "maxheat=" << maxHeatUnlocked_ << "\n";
    f << "contracts=" << contractMask_ << "\n";
    f << "mirror=";
    for (int i = 0; i < MirrorCount; ++i) { if (i) f << ","; f << mirror_[static_cast<size_t>(i)]; }
    f << "\n";
    f << "unlocked=";
    bool first = true;
    for (const std::string& id : unlocked_) {
        if (!first) f << ",";
        f << id;
        first = false;
    }
    f << "\n";
    return f.good();
}

} // namespace pulse
