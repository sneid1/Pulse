#include "Game/RunMods.hpp"

#include <algorithm>

namespace pulse {

RunMods recomputeRunMods(const std::vector<RunModifier>& mods) {
    float dmg = 0.0f, count = 0.0f, cadence = 0.0f, elite = 0.0f;
    float heal = 0.0f, scrap = 0.0f, tier = 0.0f, payout = 0.0f;
    for (const RunModifier& m : mods) {
        switch (m.kind) {
            case ModKind::EnemyDamagePct:  dmg     += m.value; break;
            case ModKind::EnemyCountPct:   count   += m.value; break;
            case ModKind::EnemyCadencePct: cadence += m.value; break;
            case ModKind::EliteChancePct:  elite   += m.value; break;
            case ModKind::HealReceivedPct: heal    += m.value; break;
            case ModKind::ScrapPct:        scrap   += m.value; break;
            case ModKind::RewardTierBias:  tier    += m.value; break;
            case ModKind::MetaPayoutPct:   payout  += m.value; break;
            case ModKind::Count:           break;
        }
    }
    RunMods r;
    r.enemyDamageMult  = std::max(0.0f, 1.0f + dmg);
    r.enemyCountMult   = std::max(0.0f, 1.0f + count);
    r.enemyCadenceMult = std::max(0.05f, 1.0f + cadence); // divided into the interval; never 0
    r.eliteChanceAdd   = elite;
    r.healMult         = std::clamp(1.0f + heal, 0.0f, 1.0f); // curse-only: never amplifies healing
    r.scrapMult        = std::max(0.0f, 1.0f + scrap);
    r.rewardTierBias   = tier;
    r.metaPayoutMult   = std::max(0.0f, 1.0f + payout);
    return r;
}

std::vector<RunModifier> heatMods(int heat) {
    std::vector<RunModifier> out;
    if (heat <= 0) return out;
    // Each level unlocks/stacks one difficulty knob, cycling through the five (the plan's
    // staged table: L1 damage, L2 count, L3 less-heal, L4 elite, L5 cadence, then repeat).
    float dmg = 0.0f, cnt = 0.0f, heal = 0.0f, elite = 0.0f, cad = 0.0f;
    for (int L = 1; L <= heat; ++L) {
        switch ((L - 1) % 5) {
            case 0: dmg   += 0.15f; break; // enemies hit harder
            case 1: cnt   += 0.20f; break; // more enemies per wave
            case 2: heal  -= 0.25f; break; // less healing
            case 3: elite += 0.10f; break; // more elites
            case 4: cad   += 0.20f; break; // faster waves
        }
    }
    auto add = [&](ModKind k, float v) { if (v != 0.0f) out.push_back({ k, v, "heat" }); };
    add(ModKind::EnemyDamagePct, dmg);
    add(ModKind::EnemyCountPct, cnt);
    add(ModKind::HealReceivedPct, heal);
    add(ModKind::EliteChancePct, elite);
    add(ModKind::EnemyCadencePct, cad);
    // Greed: persistent payout is the climb incentive (it does NOT ease the current run);
    // scrap / reward-tier bias kept small so heat stays net harder.
    out.push_back({ ModKind::MetaPayoutPct, 0.20f * static_cast<float>(heat), "heat" });
    out.push_back({ ModKind::ScrapPct, 0.05f * static_cast<float>(heat), "heat" });
    out.push_back({ ModKind::RewardTierBias, 0.04f * static_cast<float>(heat), "heat" });
    return out;
}

const std::vector<RunContract>& runContracts() {
    // Six named contracts (the heat knobs, made individually selectable). Weights sum into the
    // payout multiplier; difficulty stays count/cadence/composition/damage-to-player, never HP.
    // Order = the HEAT ladder (UI Overhaul doc): [+] adds the next modifier in this order.
    static const std::vector<RunContract> kContracts = {
        { "swarm",  "SWARM",  "+23% enemy count",        0.15f, { { ModKind::EnemyCountPct,   0.23f, "contract" }, {} }, 1 },
        { "blitz",  "BLITZ",  "waves spawn faster",      0.10f, { { ModKind::EnemyCadencePct, 0.20f, "contract" }, {} }, 1 },
        { "drain",  "DRAIN",  "-40% healing received",   0.12f, { { ModKind::HealReceivedPct, -0.40f, "contract" }, {} }, 1 },
        { "elites", "ELITES", "+20% elite chance",       0.12f, { { ModKind::EliteChancePct,  0.20f, "contract" }, {} }, 1 },
        { "glass",  "GLASS",  "+25% enemy damage",       0.15f, { { ModKind::EnemyDamagePct,  0.25f, "contract" }, {} }, 1 },
        { "frenzy", "FRENZY", "faster AND harder hits",  0.18f, { { ModKind::EnemyCadencePct, 0.20f, "contract" }, { ModKind::EnemyDamagePct, 0.15f, "contract" } }, 2 },
    };
    return kContracts;
}

unsigned contractMaskForHeat(int heat) {
    const int sz = static_cast<int>(runContracts().size());
    if (heat < 0) heat = 0; if (heat > sz) heat = sz;
    unsigned m = 0;
    for (int i = 0; i < heat; ++i) m |= (1u << i);
    return m;
}

int contractCount(unsigned mask) {
    int n = 0;
    const int sz = static_cast<int>(runContracts().size());
    for (int i = 0; i < sz; ++i) if (mask & (1u << i)) ++n;
    return n;
}

float contractPayout(unsigned mask) {
    float w = 0.0f;
    const std::vector<RunContract>& cs = runContracts();
    for (int i = 0; i < static_cast<int>(cs.size()); ++i) if (mask & (1u << i)) w += cs[static_cast<size_t>(i)].payoutWeight;
    return 1.0f + w;
}

std::vector<RunModifier> contractMods(unsigned mask) {
    std::vector<RunModifier> out;
    const std::vector<RunContract>& cs = runContracts();
    float payoutW = 0.0f; int active = 0;
    for (int i = 0; i < static_cast<int>(cs.size()); ++i) {
        if (!(mask & (1u << i))) continue;
        const RunContract& c = cs[static_cast<size_t>(i)];
        for (int m = 0; m < c.modCount; ++m) out.push_back(c.mods[m]);
        payoutW += c.payoutWeight;
        ++active;
    }
    if (active > 0) {
        out.push_back({ ModKind::MetaPayoutPct, payoutW, "contract" });        // payout = 1 + sum(weights)
        out.push_back({ ModKind::ScrapPct, 0.05f * static_cast<float>(active), "contract" });
        out.push_back({ ModKind::RewardTierBias, 0.03f * static_cast<float>(active), "contract" });
    }
    return out;
}

namespace {
struct KindName { ModKind kind; const char* name; };
constexpr KindName kKindNames[] = {
    { ModKind::EnemyDamagePct,  "EnemyDamagePct"  },
    { ModKind::EnemyCountPct,   "EnemyCountPct"   },
    { ModKind::EnemyCadencePct, "EnemyCadencePct" },
    { ModKind::EliteChancePct,  "EliteChancePct"  },
    { ModKind::HealReceivedPct, "HealReceivedPct" },
    { ModKind::ScrapPct,        "ScrapPct"        },
    { ModKind::RewardTierBias,  "RewardTierBias"  },
    { ModKind::MetaPayoutPct,   "MetaPayoutPct"   },
};
} // namespace

ModKind modKindFromString(const std::string& name) {
    for (const KindName& kn : kKindNames)
        if (name == kn.name) return kn.kind;
    return ModKind::Count;
}

const char* modKindName(ModKind kind) {
    for (const KindName& kn : kKindNames)
        if (kn.kind == kind) return kn.name;
    return "?";
}

} // namespace pulse
