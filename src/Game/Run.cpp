#include "Game/Run.hpp"

#include <algorithm>

#include "Game/Tunables.hpp"

namespace pulse {
namespace {

// Run shape: a few sectors, each opening with a fixed combat room, then a tunable run of
// BRANCHING choice steps (pick 1 of 3 typed rooms), then a boss room.
constexpr int kSectors = 3;
constexpr int kMinRoomsBeforeBoss = 2;
constexpr int kMaxRoomsBeforeBoss = 12;
constexpr int kMaxExtraRoomsBeforeBoss = 6;
constexpr int kOptionsPerStep = 3;  // a 3-way choice keeps the decision real (review fix)

// Per-sector enemy composition (hardens with sector: more ranged + tanks deeper in, so
// the threat space recombines as a triage puzzle, never gets spongier).
struct Comp {
    float ranged = 0.22f, tank = 0.10f, stalker = 0.12f, cadence = 0.85f;
    int cap = 7;
};

Comp sectorComp(const Tunables& t, int s) {
    const float baseRanged = std::clamp(t.spawnRangedChance, 0.05f, 0.50f);
    const float baseTank = std::clamp(t.spawnTankChance, 0.04f, 0.35f);
    const float baseStalker = std::clamp(t.spawnStalkerChance, 0.04f, 0.30f);
    Comp c;
    c.ranged = std::clamp(baseRanged * (0.70f + 0.22f * static_cast<float>(s)), 0.08f, 0.48f);
    c.tank = std::clamp(baseTank * (0.45f + 0.35f * static_cast<float>(s)), 0.04f, 0.32f);
    c.stalker = std::clamp(baseStalker * (0.75f + 0.22f * static_cast<float>(s)), 0.06f, 0.28f);
    c.cadence = std::clamp(t.spawnInterval, 0.35f, 1.40f);
    c.cap = std::clamp(t.spawnMaxConcurrent, 5, 12);
    return c;
}

// Escalating standard fight. depth = global step index, so all options of a step (and
// the room actually entered) share one escalation level regardless of the pick.
RoomSpec combatRoom(int s, int depth, const Comp& c) {
    RoomSpec room;
    room.sector = s;
    room.type = RoomType::Combat;
    // M5 balance: the opener is approachable at BASE power (no build yet, enemies are not
    // HP sponges but still take a few hits), then escalates by COUNT + CONCURRENCY with
    // depth - never by enemy HP. Early rooms keep the swarm small enough to fight through.
    const int waveCount = 2 + s; // 2..4 waves (brisker rooms)
    for (int wv = 0; wv < waveCount; ++wv) {
        WaveSpec w;
        w.count = 3 + depth / 2 + wv;               // room 0: 3,4 -> ramps with depth
        w.rangedChance = c.ranged;
        w.tankChance = c.tank;
        w.stalkerChance = c.stalker;
        w.spawnInterval = std::max(0.40f,
            c.cadence * (1.0f - 0.030f * static_cast<float>(depth) - 0.04f * static_cast<float>(wv)));
        w.maxConcurrent = std::min(11, 5 + depth / 2 + s); // room 0: 5 on-screen -> ramps to ~11
        room.waves.push_back(w);
    }
    return room;
}

// Fewer enemies, denser; the forced elite affixes (rolled in spawnEnemy) carry the
// threat, not bigger HP. Reward is tier-biased up in enterRoomCleared.
RoomSpec eliteRoom(int s, int depth, const Comp& c) {
    RoomSpec room;
    room.sector = s;
    room.type = RoomType::Elite;
    const int waveCount = s > 0 ? 2 : 1;
    for (int wv = 0; wv < waveCount; ++wv) {
        WaveSpec w;
        w.count = 3 + depth / 3 + wv;
        w.rangedChance = std::min(0.50f, c.ranged + 0.05f);
        w.tankChance = c.tank;
        w.stalkerChance = c.stalker;
        w.spawnInterval = std::max(0.40f, c.cadence * (0.95f - 0.03f * static_cast<float>(depth)));
        w.maxConcurrent = std::min(10, std::max(5, 4 + depth / 3 + s));
        room.waves.push_back(w);
    }
    return room;
}

// No waves: clears immediately into a guaranteed, tier-biased reward (low risk). A scrap
// cache variant arrives with the economy (M2).
RoomSpec cacheRoom(int s) {
    RoomSpec room;
    room.sector = s;
    room.type = RoomType::Cache;
    return room;
}

RoomSpec bossRoom(int s, int depth, const Comp& c) {
    RoomSpec room;
    room.sector = s;
    room.type = RoomType::Boss;
    room.boss = true;
    // M5: a lighter escort wave so the boss fight reads as the Warden (+ a few adds), not a
    // swarm-plus-boss. The Warden entity itself (spawnBoss) carries the threat.
    WaveSpec w;
    w.count = 3 + s;                       // 3..5 escorts (was 6..10)
    w.rangedChance = c.ranged * 0.6f;
    w.tankChance = std::min(0.40f, 0.16f + 0.06f * static_cast<float>(s));
    w.stalkerChance = c.stalker * 0.8f;
    w.spawnInterval = std::max(0.55f, c.cadence * (0.95f - 0.02f * static_cast<float>(depth)));
    w.maxConcurrent = std::min(8, 4 + s);  // 4..6 escorts on-screen (was 6..11)
    room.waves.push_back(w);
    return room;
}

} // namespace

void Run::begin(uint32_t seed, const Tunables& tunables) {
    seed_ = seed;
    rng_ = Rng(seed ? seed : 0x9E3779B9u);
    sectors_ = kSectors;
    roomIndex_ = 0;
    steps_.clear();
    const int minRoomsBeforeBoss = std::clamp(tunables.runMinRoomsBeforeBoss,
                                              kMinRoomsBeforeBoss, kMaxRoomsBeforeBoss);
    const int extraRoomsBeforeBoss = std::clamp(tunables.runExtraRoomsBeforeBoss,
                                                0, kMaxExtraRoomsBeforeBoss);

    // Weighted room-type roll for a choice option. Combat common; Elite/Cache rarer.
    // Shop/Event join the table in M2/M3 (with the "never two shops back to back" rule).
    auto rollType = [&](bool allowShop) -> RoomType {
        const int r = rng_.rangeInt(0, 10); // 0..10: 5 Combat, 2 Elite, 2 Cache, 1 Shop, 1 Event
        if (r < 5) return RoomType::Combat;
        if (r < 7) return RoomType::Elite;
        if (r < 9) return RoomType::Cache;
        if (r == 9) return allowShop ? RoomType::Shop : RoomType::Combat;
        return RoomType::Event;
    };
    auto noWaveRoom = [](int sec, RoomType t) {
        RoomSpec room;
        room.sector = sec;
        room.type = t; // Shop / Event: no waves; opens the shop / deal (Features 2 / 3)
        return room;
    };

    int depth = 0; // global step index, drives escalation
    bool prevShop = false; // never two Shops in back-to-back choice steps
    for (int s = 0; s < sectors_; ++s) {
        const Comp c = sectorComp(tunables, s);
        const int extraRooms = extraRoomsBeforeBoss > 0 ? rng_.rangeInt(0, extraRoomsBeforeBoss) : 0;
        const int roomsBeforeBoss = std::clamp(minRoomsBeforeBoss + extraRooms,
                                               kMinRoomsBeforeBoss, kMaxRoomsBeforeBoss);
        const int choiceSteps = roomsBeforeBoss - 1; // the fixed opener counts toward roomsBeforeBoss

        // Fixed sector opener: a standard combat room, no choice.
        { RoomStep st; st.options.push_back(combatRoom(s, depth, c)); steps_.push_back(st); ++depth; }

        // Branching choice steps: 3 typed options, with at least one "safe-ish" pick.
        for (int cs = 0; cs < choiceSteps; ++cs) {
            RoomStep st;
            bool hasSafe = false, stepHasShop = false;
            for (int k = 0; k < kOptionsPerStep; ++k) {
                RoomSpec opt;
                switch (rollType(!prevShop && !stepHasShop)) {
                    case RoomType::Elite: opt = eliteRoom(s, depth, c); break;
                    case RoomType::Cache: opt = cacheRoom(s); break;
                    case RoomType::Shop:  opt = noWaveRoom(s, RoomType::Shop); stepHasShop = true; break;
                    case RoomType::Event: opt = noWaveRoom(s, RoomType::Event); break;
                    default:              opt = combatRoom(s, depth, c); break;
                }
                if (opt.type != RoomType::Elite) hasSafe = true; // Combat/Cache/Shop are non-lethal-ish
                st.options.push_back(opt);
            }
            if (!hasSafe)
                st.options[static_cast<size_t>(rng_.rangeInt(0, kOptionsPerStep - 1))] = combatRoom(s, depth, c);
            prevShop = stepHasShop;
            steps_.push_back(st);
            ++depth;
        }

        // Boss closes the sector (single option, auto-picked on advance).
        { RoomStep st; st.boss = true; st.options.push_back(bossRoom(s, depth, c)); steps_.push_back(st); ++depth; }
    }

    if (!steps_.empty()) steps_[0].chosen = 0; // the opener is fixed -> ready to play
}

const RoomSpec& Run::currentRoom() const {
    const int idx = std::clamp(roomIndex_, 0, std::max(0, roomCount() - 1));
    const RoomStep& st = steps_[static_cast<size_t>(idx)];
    const int opt = (st.chosen >= 0 && st.chosen < static_cast<int>(st.options.size())) ? st.chosen : 0;
    return st.options[static_cast<size_t>(opt)];
}

RoomType Run::currentType() const { return currentRoom().type; }

bool Run::currentIsBoss() const {
    return !complete() && steps_[static_cast<size_t>(roomIndex_)].boss;
}

bool Run::needsChoice() const {
    if (complete()) return false;
    const RoomStep& st = steps_[static_cast<size_t>(roomIndex_)];
    return st.options.size() > 1 && st.chosen < 0;
}

const std::vector<RoomSpec>& Run::currentOptions() const {
    const int idx = std::clamp(roomIndex_, 0, std::max(0, roomCount() - 1));
    return steps_[static_cast<size_t>(idx)].options;
}

void Run::chooseOption(int i) {
    if (complete()) return;
    RoomStep& st = steps_[static_cast<size_t>(roomIndex_)];
    st.chosen = std::clamp(i, 0, static_cast<int>(st.options.size()) - 1);
}

std::vector<RoomType> Run::optionTypesAt(int stepIdx) const {
    std::vector<RoomType> out;
    if (stepIdx < 0 || stepIdx >= roomCount()) return out;
    for (const RoomSpec& o : steps_[static_cast<size_t>(stepIdx)].options) out.push_back(o.type);
    return out;
}

bool Run::advanceRoom() {
    ++roomIndex_;
    if (!complete()) {
        RoomStep& st = steps_[static_cast<size_t>(roomIndex_)];
        if (st.options.size() == 1) st.chosen = 0; // fixed step: nothing to choose
    }
    return !complete();
}

int Run::stepSector(int stepIdx) const {
    const int idx = std::clamp(stepIdx, 0, std::max(0, roomCount() - 1));
    const RoomStep& st = steps_[static_cast<size_t>(idx)];
    return st.options.empty() ? 0 : st.options.front().sector;
}

int Run::sector() const {
    if (complete()) return sectors_ - 1;
    return stepSector(roomIndex_);
}

int Run::roomsInSector(int sector) const {
    int n = 0;
    for (int i = 0; i < roomCount(); ++i)
        if (stepSector(i) == sector) ++n;
    return n;
}

int Run::roomInSector() const {
    if (steps_.empty()) return 1;
    const int idx = std::clamp(roomIndex_, 0, roomCount() - 1);
    const int sec = stepSector(idx);
    int n = 0;
    for (int i = 0; i <= idx; ++i)
        if (stepSector(i) == sec) ++n;
    return n;
}

} // namespace pulse
