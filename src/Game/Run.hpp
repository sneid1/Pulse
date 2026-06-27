#pragma once

// Phase A: the run/room state machine that wraps the proven combat in a roguelite
// shell (docs/Plan_PULSE_roguelite.md). A run is a deterministic, seeded sequence
// of rooms grouped into sectors; each room is an ordered list of escalating waves.
//
// The Run owns the dedicated run-RNG, kept separate from the combat-jitter rng_ in
// PulseGame (which tryFire draws for spread) so a seed reproduces the room and
// enemy-composition sequence no matter how the player shoots. Difficulty escalates
// by count / aggression / composition, never by per-enemy HP (Design principles).

#include <cstdint>
#include <vector>

#include "Engine/Math.hpp"

namespace pulse {

struct Tunables;

// The top-level game-state phase. Hub is filled in by Phase C; Boss is a stub
// until Phase D (a boss room is just an escalated, tank-heavy wave flagged so the
// music/HUD can react - there is no boss entity yet).
enum class RunPhase {
    Hub,
    ChoosePath,   // Feature 1: pick the next room from typed options before entering it
    Shop,         // Feature 2: in-run shop, spend scrap on gear / heal / reroll
    Event,        // Feature 3: risk/reward deal (accept a boon + curse, or decline)
    InRoom,
    RoomCleared,
    Boss,
    RunOver,
    DoorsOpen     // Spatial doors: after a clear, exits open; walk through one to pick reward+route
                  // (the Hades-style merged choice that replaces RoomCleared+ChoosePath for door arenas).
                  // Appended last so existing values keep their numbering.
};

// One wave of a room: how many enemies, their composition, and the cadence/cap at
// which they stream in. All escalation lives in these numbers.
struct WaveSpec {
    int count = 6;              // total enemies spawned over the wave
    float rangedChance = 0.22f; // P(Ranged) when an enemy spawns
    float tankChance = 0.10f;   // P(Tank)
    float stalkerChance = 0.12f; // P(Stalker); the remainder are Rushers
    float spawnInterval = 0.85f; // seconds between spawns within the wave
    int maxConcurrent = 7;      // cap of simultaneously-live enemies
};

// The kind of room a step offers (Feature 1). Combat/Elite/Cache carry wave + reward
// semantics in M1; Shop/Event are filled by Features 2/3 (M2/M3); Boss closes a sector.
enum class RoomType { Combat, Elite, Cache, Shop, Event, Boss };

// One room: an ordered list of waves plus the sector (biome) it belongs to. A boss
// room is flagged so the phase machine raises RunPhase::Boss.
struct RoomSpec {
    std::vector<WaveSpec> waves;
    int sector = 0;
    bool boss = false;
    RoomType type = RoomType::Combat;
};

// One position in the run: a "choose 1 of N" set of typed rooms. A single-option step
// is fixed (no choice - the sector opener and the boss); a 3-option step is a branch.
// chosen indexes options once the player/bot picks (-1 until then).
struct RoomStep {
    std::vector<RoomSpec> options;
    int chosen = -1;
    bool boss = false;
};

class Run {
public:
    // Build the whole sector/step/option/wave sequence deterministically from a seed.
    void begin(uint32_t seed, const Tunables& tunables);

    // The room currently being played: the chosen option of the current step (defaults
    // to option 0 before a choice is committed, so sector()/HUD reads stay valid).
    const RoomSpec& currentRoom() const;
    RoomType currentType() const;
    bool currentIsBoss() const;
    bool currentIsFinal() const { return roomIndex_ == roomCount() - 1; }

    // Branching choice (Feature 1). A step with >1 un-chosen option needs a ChoosePath
    // before its room is entered; chooseOption commits the pick. currentOptions drives
    // the choice cards; optionTypesAt(stepIdx) drives the run-rail lookahead.
    bool needsChoice() const;
    const std::vector<RoomSpec>& currentOptions() const;
    void chooseOption(int i);
    std::vector<RoomType> optionTypesAt(int stepIdx) const;

    // Advance to the next step. Single-option steps auto-pick their only option. Returns
    // false once the run is exhausted (the final step was the current one) -> a win.
    bool advanceRoom();

    bool complete() const { return roomIndex_ >= static_cast<int>(steps_.size()); }

    int roomIndex() const { return roomIndex_; }
    int roomCount() const { return static_cast<int>(steps_.size()); }
    int sector() const;
    int sectorCount() const { return sectors_; }
    int roomInSector() const;                 // 1-based room number within the sector
    int roomsInSector(int sector) const;
    uint32_t seed() const { return seed_; }

    // M4 route map: per-step sector (=biome) + boss flag, so the HUD can draw the whole run as a
    // rail of biome-colored nodes with a "you are here" marker.
    int  sectorOfStep(int stepIdx) const { return stepSector(stepIdx); }
    bool stepIsBoss(int stepIdx) const {
        return stepIdx >= 0 && stepIdx < roomCount() && steps_[static_cast<size_t>(stepIdx)].boss;
    }

    // The dedicated, seeded run-RNG (composition rolls, and Phase B reward rolls).
    Rng& rng() { return rng_; }

private:
    int stepSector(int stepIdx) const;        // the sector a step belongs to (option 0)

    uint32_t seed_ = 0;
    int sectors_ = 3;
    int roomIndex_ = 0;                        // index of the current STEP
    std::vector<RoomStep> steps_;
    Rng rng_{0x9E3779B9u};
};

} // namespace pulse
