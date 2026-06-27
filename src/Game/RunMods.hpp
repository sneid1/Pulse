#pragma once

// Roguelike-structure foundation (docs/Plan_PULSE_roguelike_structure.md, "Shared
// foundation: RunMods"). A single aggregate of run-scoped multipliers, mirroring
// BuildStats. Deals (Feature 3) push sources in mid-run, the heat table (Feature 4)
// seeds them at run start, and the balance-sim can force a set (--sim-mods); combat
// reads the folded aggregate. Difficulty escalates by COUNT / CADENCE / COMPOSITION /
// damage-to-player, never by per-enemy HP (Design principles).

#include <string>
#include <vector>

namespace pulse {

// The kinds of run-scoped modifier a source can push. Every "...Pct" is a percent
// stored as a fraction (0.15 = +15%); EliteChancePct / RewardTierBias are absolute.
enum class ModKind {
    EnemyDamagePct,    // +% damage enemies deal to the player
    EnemyCountPct,     // +% wave count (and concurrency, so it adds pressure not a slog)
    EnemyCadencePct,   // +% spawn rate (shorter interval -> faster waves)
    EliteChancePct,    // +absolute elite-affix chance
    HealReceivedPct,   // -% healing the player receives (curses/heat only ever reduce)
    ScrapPct,          // +% scrap drops (Feature 2 economy)
    RewardTierBias,    // shifts reward rolls toward higher tiers
    MetaPayoutPct,     // +% end-of-run meta currency (the heat climb incentive)
    Count
};

// One modifier source. sourceId names who pushed it (deal/heat/sim id) for the HUD
// and for scoped removal (a sector-scoped curse drops at the next sector).
struct RunModifier {
    ModKind kind = ModKind::EnemyDamagePct;
    float value = 0.0f;
    std::string sourceId;
};

// Aggregated, combat-ready totals. Folded from a RunModifier list by recomputeRunMods.
struct RunMods {
    float enemyDamageMult  = 1.0f;
    float enemyCountMult    = 1.0f;
    float enemyCadenceMult  = 1.0f;
    float eliteChanceAdd    = 0.0f;
    float healMult          = 1.0f;
    float scrapMult         = 1.0f;
    float rewardTierBias    = 0.0f;
    float metaPayoutMult    = 1.0f;
};

// Feature 4: the heat / ascension table. Returns the cumulative RunModifiers for a heat
// level (each level stacks a new knob). Heat is a difficulty/greed TRADE - it raises the
// difficulty knobs (damage/count/cadence/elite/less-heal) AND the persistent payout, but
// the power knobs stay small so win-rate decreases with heat (the monotonicity constraint).
// Used by the balance-sim (--sim-heat); the interactive Hub uses RUN CONTRACTS below.
std::vector<RunModifier> heatMods(int heat);

// RUN CONTRACTS (UI Overhaul): named, individually-toggleable difficulty modifiers selected in
// the Hub. Each raises difficulty and the persistent payout (payout = 1 + sum of active
// weights). This is the player-facing replacement for the single heat dial.
struct RunContract {
    const char* id;
    const char* name;     // tracked-uppercase label (SWARM / BLITZ / ...)
    const char* effect;   // one-line UI string
    float payoutWeight;   // contribution to the payout multiplier
    RunModifier mods[2];  // up to two difficulty knobs (unused entries have kind = Count)
    int modCount;
};
const std::vector<RunContract>& runContracts();           // the static catalog
std::vector<RunModifier> contractMods(unsigned mask);     // folded mods for the active set (+ payout)
float contractPayout(unsigned mask);                      // 1 + sum of active contract weights
int contractCount(unsigned mask);                         // number of active contracts
unsigned contractMaskForHeat(int heat);                   // HEAT ladder: the first `heat` contracts active

// Fold a modifier list into the aggregate. FOLD CONVENTION (the balance-critical
// detail): every "...Pct" accumulates ADDITIVELY (sum the values, then mult = 1 + sum),
// never multiplicatively - four systems stack into these fields and compounding
// explodes; additive keeps the sim's per-mod assert linear and legible. eliteChanceAdd
// and rewardTierBias are absolute sums. healMult is clamped to [0,1] (curse-only).
RunMods recomputeRunMods(const std::vector<RunModifier>& mods);

// Enum-name <-> ModKind, for --sim-mods parsing and the future config/pulse.content
// heat/deal tables. modKindFromString returns Count on no match.
ModKind modKindFromString(const std::string& name);
const char* modKindName(ModKind kind);

} // namespace pulse
