#pragma once

// Phase B: the stacking build layer (docs/Plan_PULSE_roguelite.md). Three axes
// stack into a run: passives (this file's item registry + inventory of RoR2-style
// counts), weapons, and abilities. The combat reads an aggregated BuildStats so
// items perturb feel without rewriting the combat primitives.
//
// EffectKind is the FIXED vocabulary: the hook points combat exposes. New items
// are data (recombinations of these effects + numbers); a genuinely new effect
// *type* is a new EffectKind here AND a new C++ read-site in PulseGame. Enumerate
// the hooks before growing the content (the plan's first rule for Phase B).

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/Math.hpp"

namespace pulse {

enum class EffectKind {
    // --- stat mods (summed across stacks, then folded into BuildStats) ---
    DamagePct,          // +% weapon damage
    FireRatePct,        // +% fire rate
    ReloadSpeedPct,     // +% reload speed
    MoveSpeedPct,       // +% move speed
    DashCooldownPct,    // shortens dash cooldown by this fraction
    CritChance,         // +chance (0..1) for a body shot to crit
    CritDamagePct,      // +% crit damage (over the base crit multiplier)
    MaxHealthFlat,      // +flat max HP
    MaxShieldFlat,      // +flat max shield
    DamageReductionPct, // +% incoming damage reduced
    KnockbackPct,       // +% hit knockback
    // --- on-event hooks ---
    HealOnKill,         // +flat HP per kill
    LifeLeechPct,       // +fraction of direct weapon-hit damage returned as HP
    ShieldOnKill,       // +flat shield per kill
    AmmoOnKill,         // +flat reserve ammo per kill
    ExplodeOnKill,      // AoE damage centred on a kill
    ChainOnHit,         // arc damage to a nearby enemy on every hit
    ExplodeOnHit,       // small AoE on every hit
    DashDamage,         // dash-mod: a dash deals AoE damage along its path (no button)
    // --- M2 status-element application (stacks applied per hit; see Game/Status.hpp) ---
    IgniteOnHit,        // +burn stacks per hit (DoT)
    ShockOnHit,         // +shock charge per hit (discharges into a chain at threshold)
    ChillOnHit,         // +chill per hit (slow -> freeze; frozen foes shatter)
    CorrodeOnHit,       // +corrode stacks per hit (amps incoming damage + status)
    Count
};

enum class ItemTier { Common, Uncommon, Rare, Legendary };

// M3: build IDENTITY. Each item belongs to an affinity; owning enough of one affinity unlocks
// SET BONUSES (a 3-set amplifier and a 5-set signature transform), which is how the build
// "transforms" rather than just stacks. The four element affinities (Pyro/Volt/Cryo/Acid) pair
// with the M2 status system; Kinetic (mobility/aggression) and Bulwark (defense) round it out.
enum class Affinity { None, Pyro, Volt, Cryo, Acid, Kinetic, Bulwark, Count };
const char* affinityName(Affinity a);

// Firing-archetype kernel (Phase B.2). Each weapon = an archetype + a power level
// (upgrades/dupes raise it) + intrinsic numbers. The archetype is the brand of
// distinct feel; numbers are data. This is the one place the combat core is
// genuinely reworked (tryFire/updateWeapon dispatch on it).
enum class WeaponArchetype {
    HitscanAuto, // one ray per shot, supports full-auto learnable spray
    Burst,       // precise multi-round burst per trigger pull, tight and punchy
    Spread,      // multi-pellet shotgun cone, devastating up close
    Projectile,  // travelling bolt the player leads with; can splash
    Beam         // very high cadence, low per-tick (treated as fast hitscan for now)
};

// M3 weapon ASPECTS (Pillar 2c): unlockable forms that rework a weapon as you invest power
// into it. Aspect 0 is the base weapon (implicit identity); aspects[k] is form k+1, unlocked at
// weapon power k+1. Forms perturb damage/cadence/reload and can grant an INTRINSIC element so a
// weapon itself becomes a status source (build becomes weapon-led). Kept to multipliers + an
// element so it layers on the existing firing kernel without rewriting it.
struct WeaponAspect {
    std::string name;
    std::string blurb;
    float damageMult = 1.0f;
    float fireRateMult = 1.0f;
    float reloadMult = 1.0f;
    int   element = 0;          // 0 none; 1 Burn, 2 Shock, 3 Cryo, 4 Corrode (maps to Status Element)
    float elementStacks = 0.0f; // stacks applied per hit by this form
};

struct WeaponDef {
    std::string id;
    std::string name;
    std::string blurb;
    WeaponArchetype archetype = WeaponArchetype::HitscanAuto;
    float damage = 30.0f;          // base per shot/pellet at power 1 (0 = use tunables)
    float fireRate = 9.7f;         // shots/sec (0 = use tunables)
    int   pellets = 1;             // Spread pellet count
    int   burst = 1;               // Burst round count
    float spreadDeg = 0.0f;        // cone half-angle for spread/burst jitter
    int   magazine = 30;           // rounds per magazine (0 = use tunables)
    float reload = 1.35f;          // reload seconds (0 = use tunables)
    float projectileSpeed = 0.0f;  // Projectile travel speed
    float splashRadius = 0.0f;     // Projectile AoE on impact (0 = single target)
    ItemTier tier = ItemTier::Common;
    bool automatic = true;         // false = semi-auto (one shot per trigger pull)
    float fireVolumeScale = 1.0f;  // per-weapon trim before the shared weapon SFX bus
    std::vector<WeaponAspect> aspects;  // M3: forms unlocked by power (aspect 0 = base, implicit)
};

struct EffectMod {
    EffectKind kind = EffectKind::DamagePct;
    float value = 0.0f;
};

struct ItemDef {
    std::string id;     // stable string id (save-safe; never an array index)
    std::string name;   // HUD label
    std::string blurb;  // one-line description for the reward card
    ItemTier tier = ItemTier::Common;
    Affinity affinity = Affinity::None;   // M3: which set this item counts toward
    std::vector<EffectMod> mods;
};

// Aggregated, combat-ready totals, recomputed whenever the inventory changes.
struct BuildStats {
    float damageMult = 1.0f;
    float fireRateMult = 1.0f;
    float reloadSpeedMult = 1.0f;
    float moveSpeedMult = 1.0f;
    float dashCooldownMult = 1.0f;
    float critChance = 0.0f;
    float critDamageMult = 1.5f;  // base crit multiplier; CritDamagePct adds on top
    int   maxHealthBonus = 0;
    int   maxShieldBonus = 0;
    float damageReduction = 0.0f;
    float knockbackMult = 1.0f;
    int   healOnKill = 0;
    float lifeLeechPct = 0.0f;
    int   shieldOnKill = 0;
    int   ammoOnKill = 0;
    float explodeOnKillDamage = 0.0f;
    float chainOnHitDamage = 0.0f;
    float explodeOnHitDamage = 0.0f;
    float dashDamage = 0.0f;        // dash-mod: AoE damage dealt on a dash
    // M2 status-element application amounts (stacks per hit; 0 = the element is off).
    float igniteOnHit = 0.0f;
    float shockOnHit = 0.0f;
    float chillOnHit = 0.0f;
    float corrodeOnHit = 0.0f;
    // M3 affinity SET BONUSES (derived from how many items of each affinity are owned).
    // The 3-set amplifies its element's application; the 5-set unlocks a signature transform.
    float burnApplyMult = 1.0f;     // Pyro 3-set: +burn application
    float shockApplyMult = 1.0f;    // Volt 3-set: +shock application
    float chillApplyMult = 1.0f;    // Cryo 3-set: +chill application
    float corrodeApplyMult = 1.0f;  // Acid 3-set: +corrode application
    bool  burnDetonateOnKill = false;  // Pyro 5-set: a burning enemy detonates its burn as AoE on death
    bool  shockConduct = false;        // Volt 5-set: shock discharges arc further + apply your on-hit elements
    bool  cryoNova = false;            // Cryo 5-set: a shattered (frozen) enemy emits a chilling nova
    bool  corrodeSpread = false;       // Acid 5-set: killing a corroded enemy spreads corrode to nearby foes
    int   affinityCount[static_cast<int>(Affinity::Count)] = {};  // owned items per affinity (HUD/synergy)
    int   topAffinity = 0;             // the affinity with the most items (0 = None), for synergy surfacing
};

class Build {
public:
    Build(); // registers the starter passive catalog

    const std::vector<ItemDef>& catalog() const { return catalog_; }
    const ItemDef* find(const std::string& id) const;

    // Weapon registry (Phase B.2). AK-47 is id "ak47"; "carbine" is a distinct
    // tactical burst rifle and must not be used as an AK alias.
    const std::vector<WeaponDef>& weaponCatalog() const { return weapons_; }
    const WeaponDef* findWeapon(const std::string& id) const;

    // Resolve a reward id from rollRewards (prefixed "p:" passive / "w:" weapon) into
    // display fields + the raw id, for the reward HUD and the grant logic.
    struct RewardView {
        bool valid = false;
        bool isWeapon = false;
        std::string rawId;   // the unprefixed registry id
        std::string name;
        std::string blurb;
        ItemTier tier = ItemTier::Common;
        Affinity affinity = Affinity::None;   // M3: for reward-UI synergy surfacing
    };
    RewardView describeReward(const std::string& prefixedId) const;

    void clear();                                  // reset inventory (new run)
    void add(const std::string& id, int count = 1);
    int  stacks(const std::string& id) const;
    int  totalItems() const;
    const std::unordered_map<std::string, int>& inventory() const { return inventory_; }

    const BuildStats& stats() const { return stats_; }

    // Roll n distinct reward item ids, tier-weighted, using the given (run-)RNG.
    // Passive ids are prefixed "p:" and weapon ids "w:" so a mixed pool round-trips.
    // Prefixed ids in `excluded` are kept out of the pool (Phase C meta gating).
    // tierBias (RunMods.rewardTierBias from heat/Elite rooms) shifts the roll toward
    // higher tiers: 0 = unchanged; positive favours Uncommon/Rare.
    std::vector<std::string> rollRewards(Rng& rng, int n, const std::vector<std::string>& excluded = {},
                                         float tierBias = 0.0f) const;

    // Re-read config/pulse.content over the built-in defaults (F5 hot-reload). The
    // inventory is preserved; ids that vanish are simply ignored by the combat reads.
    // Returns true if a content file was found and applied.
    bool reloadContent();

private:
    void registerDefaults();      // the built-in catalog (recombinations of the vocabulary)
    bool loadContentFile();       // optional config/pulse.content: override-or-add items/weapons
    void recompute();

    std::vector<ItemDef> catalog_;
    std::vector<WeaponDef> weapons_;
    std::unordered_map<std::string, int> inventory_;
    BuildStats stats_;
};

const char* tierName(ItemTier tier);

} // namespace pulse
