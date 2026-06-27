#include "Game/Build.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace pulse {

const char* tierName(ItemTier tier) {
    switch (tier) {
        case ItemTier::Common: return "COMMON";
        case ItemTier::Uncommon: return "UNCOMMON";
        case ItemTier::Rare: return "RARE";
        case ItemTier::Legendary: return "LEGENDARY";
    }
    return "?";
}

const char* affinityName(Affinity a) {
    switch (a) {
        case Affinity::Pyro: return "PYRO";
        case Affinity::Volt: return "VOLT";
        case Affinity::Cryo: return "CRYO";
        case Affinity::Acid: return "ACID";
        case Affinity::Kinetic: return "KINETIC";
        case Affinity::Bulwark: return "BULWARK";
        case Affinity::None:
        case Affinity::Count: break;
    }
    return "";
}

namespace {

// Small parser helpers for config/pulse.content (a pipe-delimited structured table).
std::string ctrim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string clower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> csplit(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::stringstream ss(s);
    while (std::getline(ss, cur, sep)) out.push_back(cur);
    return out;
}

float cfloat(const std::string& s, float fallback = 0.0f) {
    try { return std::stof(s); } catch (...) { return fallback; }
}

ItemTier parseTier(const std::string& s) {
    const std::string t = clower(ctrim(s));
    if (t == "legendary") return ItemTier::Legendary;
    if (t == "rare") return ItemTier::Rare;
    if (t == "uncommon") return ItemTier::Uncommon;
    return ItemTier::Common;
}

Affinity parseAffinity(const std::string& s) {
    const std::string a = clower(ctrim(s));
    if (a == "pyro") return Affinity::Pyro;
    if (a == "volt") return Affinity::Volt;
    if (a == "cryo") return Affinity::Cryo;
    if (a == "acid") return Affinity::Acid;
    if (a == "kinetic") return Affinity::Kinetic;
    if (a == "bulwark") return Affinity::Bulwark;
    return Affinity::None;
}

WeaponArchetype parseArchetype(const std::string& s) {
    const std::string a = clower(ctrim(s));
    if (a == "burst") return WeaponArchetype::Burst;
    if (a == "spread") return WeaponArchetype::Spread;
    if (a == "projectile") return WeaponArchetype::Projectile;
    if (a == "beam") return WeaponArchetype::Beam;
    return WeaponArchetype::HitscanAuto;
}

bool parseEffectKind(const std::string& s, EffectKind& out) {
    const std::string k = clower(ctrim(s));
    using E = EffectKind;
    struct Row { const char* name; E kind; };
    static const Row table[] = {
        { "damagepct", E::DamagePct }, { "fireratepct", E::FireRatePct },
        { "reloadspeedpct", E::ReloadSpeedPct }, { "movespeedpct", E::MoveSpeedPct },
        { "dashcooldownpct", E::DashCooldownPct }, { "critchance", E::CritChance },
        { "critdamagepct", E::CritDamagePct }, { "maxhealthflat", E::MaxHealthFlat },
        { "maxshieldflat", E::MaxShieldFlat }, { "damagereductionpct", E::DamageReductionPct },
        { "knockbackpct", E::KnockbackPct }, { "healonkill", E::HealOnKill },
        { "lifeleechpct", E::LifeLeechPct }, { "leechpct", E::LifeLeechPct },
        { "shieldonkill", E::ShieldOnKill }, { "ammoonkill", E::AmmoOnKill },
        { "explodeonkill", E::ExplodeOnKill }, { "chainonhit", E::ChainOnHit },
        { "explodeonhit", E::ExplodeOnHit }, { "dashdamage", E::DashDamage },
        { "igniteonhit", E::IgniteOnHit }, { "shockonhit", E::ShockOnHit },
        { "chillonhit", E::ChillOnHit }, { "corrodeonhit", E::CorrodeOnHit },
    };
    for (const Row& r : table) {
        if (k == r.name) { out = r.kind; return true; }
    }
    return false;
}

} // namespace

Build::Build() {
    registerDefaults();
    loadContentFile();   // optional config/pulse.content overrides/extends the defaults
    recompute();
}

void Build::registerDefaults() {
    // Built-in catalog (~25 items). All recombinations of the fixed effect vocabulary,
    // so they are pure data; a new effect *type* would need a hook. Synergy pivots
    // (bloodthirst, chain_reaction, executioner) combine effects so A enables B.
    catalog_.clear();
    weapons_.clear();
    using E = EffectKind;
    auto add = [&](const char* id, const char* name, const char* blurb, ItemTier tier,
                   std::vector<EffectMod> mods, Affinity aff = Affinity::None) {
        catalog_.push_back({ id, name, blurb, tier, aff, std::move(mods) });
    };

    add("carbine_rounds", "Carbine Rounds", "+15% weapon damage", ItemTier::Common,
        { { E::DamagePct, 0.15f } });
    add("light_trigger", "Light Trigger", "+12% fire rate", ItemTier::Common,
        { { E::FireRatePct, 0.12f } });
    add("quick_hands", "Quick Hands", "+25% reload speed", ItemTier::Common,
        { { E::ReloadSpeedPct, 0.25f } });
    add("light_frame", "Light Frame", "+12% move speed", ItemTier::Common,
        { { E::MoveSpeedPct, 0.12f } });
    add("kinetic_dash", "Kinetic Capacitor", "-18% dash cooldown", ItemTier::Common,
        { { E::DashCooldownPct, 0.18f } });
    add("vital_cells", "Vital Cells", "+25 max health", ItemTier::Common,
        { { E::MaxHealthFlat, 25.0f } });
    add("scavenger", "Scavenger", "+5 reserve ammo per kill", ItemTier::Common,
        { { E::AmmoOnKill, 5.0f } });
    add("heavy_impact", "Heavy Impact", "+60% hit knockback", ItemTier::Common,
        { { E::KnockbackPct, 0.60f } });

    add("hard_plating", "Hard Plating", "+20 max shield", ItemTier::Uncommon,
        { { E::MaxShieldFlat, 20.0f } });
    add("ablative_weave", "Ablative Weave", "-8% incoming damage", ItemTier::Uncommon,
        { { E::DamageReductionPct, 0.08f } });
    add("hollow_points", "Hollow Points", "+8% crit chance", ItemTier::Uncommon,
        { { E::CritChance, 0.08f } });
    add("focusing_lens", "Focusing Lens", "+35% crit damage", ItemTier::Uncommon,
        { { E::CritDamagePct, 0.35f } });
    add("siphon_rounds", "Siphon Rounds", "shots leech HP, kills heal", ItemTier::Uncommon,
        { { E::LifeLeechPct, 0.06f }, { E::HealOnKill, 2.0f } });
    add("capacitor", "Storm Capacitor", "+6 shield per kill", ItemTier::Uncommon,
        { { E::ShieldOnKill, 6.0f } });

    add("overcharge", "Overcharge Cell", "+25% weapon damage", ItemTier::Uncommon,
        { { E::DamagePct, 0.25f } });
    add("rapid_cycle", "Rapid Cycle", "+18% fire rate", ItemTier::Uncommon,
        { { E::FireRatePct, 0.18f } });
    add("trauma_plates", "Trauma Plates", "+35 max shield", ItemTier::Uncommon,
        { { E::MaxShieldFlat, 35.0f } });

    add("frag_payload", "Frag Payload", "kills detonate (AoE)", ItemTier::Rare,
        { { E::ExplodeOnKill, 35.0f } });
    add("arc_conductor", "Arc Conductor", "hits arc to a nearby foe", ItemTier::Rare,
        { { E::ChainOnHit, 18.0f } });
    add("volatile_rounds", "Volatile Rounds", "every hit splashes (AoE)", ItemTier::Rare,
        { { E::ExplodeOnHit, 10.0f } });
    // Designed build pivots: items that combine effects so A enables B (not just
    // additive stacks) - the synergy the plan asks the content pass to seed.
    add("bloodthirst", "Bloodthirst", "strong leech, +6 health/kill, +8% move", ItemTier::Rare,
        { { E::LifeLeechPct, 0.08f }, { E::HealOnKill, 6.0f }, { E::MoveSpeedPct, 0.08f } });
    add("chain_reaction", "Chain Reaction", "kills detonate + hits arc", ItemTier::Rare,
        { { E::ExplodeOnKill, 22.0f }, { E::ChainOnHit, 9.0f } });
    add("executioner", "Executioner", "+12% crit, +20% crit dmg", ItemTier::Rare,
        { { E::CritChance, 0.12f }, { E::CritDamagePct, 0.20f } });
    // Dash-mod (ability axis): turns the dash into an offensive verb - dashing through
    // a pack damages + scatters them, no button. Stacks for a dash-focused build.
    add("kinetic_strike", "Kinetic Strike", "dashing damages foes you pass", ItemTier::Uncommon,
        { { E::DashDamage, 40.0f }, { E::DashCooldownPct, 0.10f } });

    // M2 status-element infusions: each turns ON one element system; stacks deepen it. M3
    // gives them an AFFINITY so they count toward set bonuses (3-set amps, 5-set transform).
    add("incendiary_rounds", "Incendiary Rounds", "hits ignite foes (burn DoT)", ItemTier::Uncommon,
        { { E::IgniteOnHit, 1.4f } }, Affinity::Pyro);
    add("tesla_coil", "Tesla Coil", "hits build shock, then chain-arc", ItemTier::Uncommon,
        { { E::ShockOnHit, 1.3f } }, Affinity::Volt);
    add("cryo_rounds", "Cryo Rounds", "hits chill, then freeze + shatter", ItemTier::Uncommon,
        { { E::ChillOnHit, 0.22f } }, Affinity::Cryo);
    add("corrosive_rounds", "Corrosive Rounds", "hits corrode armor (+damage taken)", ItemTier::Uncommon,
        { { E::CorrodeOnHit, 1.2f } }, Affinity::Acid);
    // Rare element pivots: heavier single-element investment that pays off a status build.
    add("thermal_lance", "Thermal Lance", "+strong ignite, +15% damage", ItemTier::Rare,
        { { E::IgniteOnHit, 2.2f }, { E::DamagePct, 0.15f } }, Affinity::Pyro);
    add("permafrost", "Permafrost", "+strong chill, freezes fast", ItemTier::Rare,
        { { E::ChillOnHit, 0.35f }, { E::CritDamagePct, 0.20f } }, Affinity::Cryo);

    // M3 affinity FILLERS - cheaper element items so a 3/5-set is reachable in a run, each
    // also carrying a small supporting stat so they are never dead picks off-build.
    add("ember_rounds", "Ember Rounds", "light ignite, +10% fire rate", ItemTier::Common,
        { { E::IgniteOnHit, 0.8f }, { E::FireRatePct, 0.10f } }, Affinity::Pyro);
    add("static_charge", "Static Charge", "light shock, +8% crit", ItemTier::Common,
        { { E::ShockOnHit, 0.8f }, { E::CritChance, 0.08f } }, Affinity::Volt);
    add("frost_rounds", "Frost Rounds", "light chill, +10% reload", ItemTier::Common,
        { { E::ChillOnHit, 0.13f }, { E::ReloadSpeedPct, 0.10f } }, Affinity::Cryo);
    add("acid_rounds", "Acid Rounds", "light corrode, +12% damage", ItemTier::Common,
        { { E::CorrodeOnHit, 0.7f }, { E::DamagePct, 0.12f } }, Affinity::Acid);
    add("conductor_plate", "Conductor Plate", "+shock, +6 shield/kill", ItemTier::Uncommon,
        { { E::ShockOnHit, 1.0f }, { E::ShieldOnKill, 6.0f } }, Affinity::Volt);
    add("brimstone", "Brimstone", "+ignite, +knockback", ItemTier::Uncommon,
        { { E::IgniteOnHit, 1.2f }, { E::KnockbackPct, 0.40f } }, Affinity::Pyro);

    // Kinetic (mobility/aggression) + Bulwark (defense) affinities for non-element builds.
    add("sprinter", "Sprinter", "+10% move, kinetic", ItemTier::Common,
        { { E::MoveSpeedPct, 0.10f } }, Affinity::Kinetic);
    add("slipstream", "Slipstream", "-15% dash cooldown, kinetic", ItemTier::Common,
        { { E::DashCooldownPct, 0.15f } }, Affinity::Kinetic);
    add("momentum_core", "Momentum Core", "+8% move, +8% fire rate", ItemTier::Uncommon,
        { { E::MoveSpeedPct, 0.08f }, { E::FireRatePct, 0.08f } }, Affinity::Kinetic);
    add("bulwark_plate", "Bulwark Plate", "+25 shield, bulwark", ItemTier::Common,
        { { E::MaxShieldFlat, 25.0f } }, Affinity::Bulwark);
    add("bracer", "Reinforced Bracer", "+20 HP, -4% damage taken", ItemTier::Common,
        { { E::MaxHealthFlat, 20.0f }, { E::DamageReductionPct, 0.04f } }, Affinity::Bulwark);
    add("aegis_core", "Aegis Core", "+30 shield, +6 shield/kill", ItemTier::Uncommon,
        { { E::MaxShieldFlat, 30.0f }, { E::ShieldOnKill, 6.0f } }, Affinity::Bulwark);

    // M3 LEGENDARIES: build-defining capstones. Rare in the pool; tier-bias (heat/Pulse greed/
    // Elite rooms) favours them. Each pushes a specific archetype over the top.
    add("sunfire_core", "Sunfire Core", "huge ignite + every kill detonates burn", ItemTier::Legendary,
        { { E::IgniteOnHit, 2.0f }, { E::ExplodeOnKill, 18.0f } }, Affinity::Pyro);
    add("tesla_crown", "Tesla Crown", "huge shock + hits arc", ItemTier::Legendary,
        { { E::ShockOnHit, 2.0f }, { E::ChainOnHit, 16.0f } }, Affinity::Volt);
    add("absolute_zero", "Absolute Zero", "huge chill + +40% crit damage", ItemTier::Legendary,
        { { E::ChillOnHit, 0.4f }, { E::CritDamagePct, 0.40f }, { E::CritChance, 0.10f } }, Affinity::Cryo);
    add("dissolution", "Dissolution", "huge corrode + every hit splashes", ItemTier::Legendary,
        { { E::CorrodeOnHit, 1.8f }, { E::ExplodeOnHit, 12.0f } }, Affinity::Acid);
    add("juggernaut", "Juggernaut", "+60 HP, -15% damage, +30% knockback", ItemTier::Legendary,
        { { E::MaxHealthFlat, 60.0f }, { E::DamageReductionPct, 0.15f }, { E::KnockbackPct, 0.30f } }, Affinity::Bulwark);
    add("apex_predator", "Apex Predator", "+35% damage, +12% move, leech + kills heal", ItemTier::Legendary,
        { { E::DamagePct, 0.35f }, { E::MoveSpeedPct, 0.12f }, { E::LifeLeechPct, 0.05f }, { E::HealOnKill, 8.0f } }, Affinity::Kinetic);

    // Weapon archetypes. The starting Sidearm is a humble semi-auto pistol. AK-47
    // owns the full-auto AK feel; Carbine is a separate tactical burst weapon.
    weapons_.push_back({ "pistol", "Sidearm", "reliable semi-auto pistol",
        WeaponArchetype::HitscanAuto, 22.0f, 7.0f, 1, 1, 0.0f, 12, 1.0f, 0.0f, 0.0f, ItemTier::Common, false, 0.90f });
    weapons_.push_back({ "ak47", "AK-47", "600 RPM full-auto AK pattern",
        WeaponArchetype::HitscanAuto, 30.0f, 10.0f, 1, 1, 0.0f, 30, 1.35f, 0.0f, 0.0f, ItemTier::Common, true, 0.82f });
    weapons_.push_back({ "carbine", "Tactical Carbine", "controlled 3-round burst rifle",
        WeaponArchetype::Burst, 28.0f, 4.3f, 1, 3, 0.22f, 24, 1.35f, 0.0f, 0.0f, ItemTier::Uncommon, false, 0.95f });
    weapons_.push_back({ "pulse_smg", "Pulse SMG", "blistering low-recoil spray",
        WeaponArchetype::Beam, 14.0f, 16.0f, 1, 1, 0.6f, 45, 1.5f, 0.0f, 0.0f, ItemTier::Common });
    weapons_.push_back({ "scattergun", "Scattergun", "7-pellet close-range wall",
        WeaponArchetype::Spread, 11.0f, 2.4f, 7, 1, 5.0f, 8, 1.6f, 0.0f, 0.0f, ItemTier::Uncommon });
    weapons_.push_back({ "marksman", "Marksman", "tight 3-round burst",
        WeaponArchetype::Burst, 40.0f, 4.0f, 1, 3, 0.4f, 18, 1.5f, 0.0f, 0.0f, ItemTier::Uncommon });
    weapons_.push_back({ "railbolt", "Railbolt", "leading bolt, splashes on impact",
        WeaponArchetype::Projectile, 58.0f, 1.6f, 1, 1, 0.0f, 6, 1.8f, 22.0f, 1.8f, ItemTier::Rare });

    // M3 weapon ASPECTS: forms that rework a weapon as you pour power (dupes) into it. Unlocked
    // by power level; cycled in-run. Each gives a distinct identity (often an intrinsic element).
    auto aspectsFor = [&](const char* id, std::vector<WeaponAspect> a) {
        for (WeaponDef& w : weapons_) if (w.id == id) { w.aspects = std::move(a); return; }
    };
    aspectsFor("ak47", {
        { "Inferno", "rounds ignite; -8% fire rate", 1.05f, 0.92f, 1.0f, 1, 0.7f },     // Pyro lean
        { "Tempest", "rounds shock; +12% fire rate", 0.95f, 1.12f, 1.0f, 2, 0.6f },     // Volt lean
    });
    aspectsFor("scattergun", {
        { "Avalanche", "pellets chill; slower reload", 1.0f, 1.0f, 1.15f, 3, 0.10f },   // Cryo lean
        { "Meltdown", "pellets corrode; +10% damage", 1.10f, 1.0f, 1.0f, 4, 0.6f },     // Acid lean
    });
    aspectsFor("carbine", {
        { "Overload", "burst shocks; +15% damage", 1.15f, 1.0f, 1.0f, 2, 0.8f },
    });
    aspectsFor("railbolt", {
        { "Sunlance", "bolt ignites hard; +20% damage", 1.20f, 1.0f, 1.0f, 1, 2.2f },
    });
    aspectsFor("marksman", {
        { "Frostpierce", "rounds chill hard; +10% damage", 1.10f, 1.0f, 1.0f, 3, 0.25f },
    });
}

const WeaponDef* Build::findWeapon(const std::string& id) const {
    for (const WeaponDef& w : weapons_) {
        if (w.id == id) return &w;
    }
    return nullptr;
}

// Parse an optional config/pulse.content table that overrides existing records (by id)
// or adds new ones. Only recombinations of the FIXED effect vocabulary are data; a new
// effect *type* would still need a C++ hook. Format (pipe-delimited, '#' comments):
//   passive | id | tier | Name | Blurb | EffectKey:val EffectKey:val ...
//   weapon  | id | Archetype | tier | Name | Blurb | key:val key:val ...
bool Build::loadContentFile() {
    std::ifstream f;
    for (const char* path : { "config/pulse.content", "../config/pulse.content", "../../config/pulse.content" }) {
        f.open(path);
        if (f.good()) break;
        f.close();
    }
    if (!f.good()) return false;

    auto upsertItem = [&](const ItemDef& d) {
        for (ItemDef& it : catalog_) if (it.id == d.id) { it = d; return; }
        catalog_.push_back(d);
    };
    auto upsertWeapon = [&](const WeaponDef& w) {
        for (WeaponDef& it : weapons_) if (it.id == w.id) { it = w; return; }
        weapons_.push_back(w);
    };

    std::string raw;
    while (std::getline(f, raw)) {
        const size_t hash = raw.find('#');
        if (hash != std::string::npos) raw = raw.substr(0, hash);
        const std::string line = ctrim(raw);
        if (line.empty()) continue;

        std::vector<std::string> fields = csplit(line, '|');
        for (std::string& s : fields) s = ctrim(s);
        if (fields.empty()) continue;
        const std::string kind = clower(fields[0]);

        if (kind == "passive" && fields.size() >= 6) {
            ItemDef d;
            d.id = fields[1];
            d.tier = parseTier(fields[2]);
            d.name = fields[3];
            d.blurb = fields[4];
            std::stringstream ms(fields[5]);
            std::string tok;
            while (ms >> tok) {
                const size_t c = tok.find(':');
                if (c == std::string::npos) continue;
                // M7: an `affinity:pyro` token sets the item's set affinity (data-driven M3 content);
                // any other key is parsed as an effect of the fixed vocabulary.
                if (clower(tok.substr(0, c)) == "affinity") { d.affinity = parseAffinity(tok.substr(c + 1)); continue; }
                EffectKind ek{};
                if (parseEffectKind(tok.substr(0, c), ek)) d.mods.push_back({ ek, cfloat(tok.substr(c + 1)) });
            }
            if (!d.id.empty()) upsertItem(d);
        } else if (kind == "weapon" && fields.size() >= 7) {
            WeaponDef w;
            w.id = fields[1];
            w.archetype = parseArchetype(fields[2]);
            w.tier = parseTier(fields[3]);
            w.name = fields[4];
            w.blurb = fields[5];
            std::stringstream ws(fields[6]);
            std::string tok;
            while (ws >> tok) {
                const size_t c = tok.find(':');
                if (c == std::string::npos) continue;
                const std::string key = clower(tok.substr(0, c));
                const float v = cfloat(tok.substr(c + 1));
                if (key == "damage") w.damage = v;
                else if (key == "firerate") w.fireRate = v;
                else if (key == "pellets") w.pellets = static_cast<int>(v);
                else if (key == "burst") w.burst = static_cast<int>(v);
                else if (key == "spreaddeg") w.spreadDeg = v;
                else if (key == "magazine") w.magazine = static_cast<int>(v);
                else if (key == "reload") w.reload = v;
                else if (key == "projectilespeed") w.projectileSpeed = v;
                else if (key == "splashradius") w.splashRadius = v;
                else if (key == "automatic") w.automatic = (v != 0.0f);
                else if (key == "firevolume" || key == "firevolumescale") w.fireVolumeScale = v;
            }
            if (!w.id.empty()) upsertWeapon(w);
        }
    }
    return true;
}

bool Build::reloadContent() {
    registerDefaults();
    const bool found = loadContentFile();
    recompute();
    return found;
}

Build::RewardView Build::describeReward(const std::string& prefixedId) const {
    RewardView v;
    const bool isWeapon = prefixedId.rfind("w:", 0) == 0;
    v.rawId = (prefixedId.size() > 2 && prefixedId[1] == ':') ? prefixedId.substr(2) : prefixedId;
    if (isWeapon) {
        if (const WeaponDef* w = findWeapon(v.rawId)) {
            v.valid = true; v.isWeapon = true;
            v.name = w->name; v.blurb = w->blurb; v.tier = w->tier;
        }
    } else if (const ItemDef* d = find(v.rawId)) {
        v.valid = true; v.isWeapon = false;
        v.name = d->name; v.blurb = d->blurb; v.tier = d->tier; v.affinity = d->affinity;
    }
    return v;
}

const ItemDef* Build::find(const std::string& id) const {
    for (const ItemDef& d : catalog_) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

void Build::clear() {
    inventory_.clear();
    recompute();
}

void Build::add(const std::string& id, int count) {
    if (!find(id) || count == 0) return;
    inventory_[id] += count;
    recompute();
}

int Build::stacks(const std::string& id) const {
    const auto it = inventory_.find(id);
    return it == inventory_.end() ? 0 : it->second;
}

int Build::totalItems() const {
    int n = 0;
    for (const auto& kv : inventory_) n += kv.second;
    return n;
}

void Build::recompute() {
    std::array<float, static_cast<size_t>(EffectKind::Count)> sum{};
    for (const auto& kv : inventory_) {
        const ItemDef* d = find(kv.first);
        if (!d) continue;
        for (const EffectMod& m : d->mods) {
            sum[static_cast<size_t>(m.kind)] += m.value * static_cast<float>(kv.second);
        }
    }
    const auto S = [&](EffectKind k) { return sum[static_cast<size_t>(k)]; };

    stats_ = BuildStats{};
    stats_.damageMult = 1.0f + S(EffectKind::DamagePct);
    stats_.fireRateMult = 1.0f + S(EffectKind::FireRatePct);
    stats_.reloadSpeedMult = 1.0f + S(EffectKind::ReloadSpeedPct);
    stats_.moveSpeedMult = 1.0f + S(EffectKind::MoveSpeedPct);
    stats_.dashCooldownMult = std::max(0.2f, 1.0f - S(EffectKind::DashCooldownPct));
    stats_.critChance = clamp(S(EffectKind::CritChance), 0.0f, 1.0f);
    stats_.critDamageMult = 1.5f + S(EffectKind::CritDamagePct);
    stats_.maxHealthBonus = static_cast<int>(std::lround(S(EffectKind::MaxHealthFlat)));
    stats_.maxShieldBonus = static_cast<int>(std::lround(S(EffectKind::MaxShieldFlat)));
    stats_.damageReduction = clamp(S(EffectKind::DamageReductionPct), 0.0f, 0.85f);
    stats_.knockbackMult = 1.0f + S(EffectKind::KnockbackPct);
    stats_.healOnKill = static_cast<int>(std::lround(S(EffectKind::HealOnKill)));
    stats_.lifeLeechPct = clamp(S(EffectKind::LifeLeechPct), 0.0f, 0.35f);
    stats_.shieldOnKill = static_cast<int>(std::lround(S(EffectKind::ShieldOnKill)));
    stats_.ammoOnKill = static_cast<int>(std::lround(S(EffectKind::AmmoOnKill)));
    stats_.explodeOnKillDamage = S(EffectKind::ExplodeOnKill);
    stats_.chainOnHitDamage = S(EffectKind::ChainOnHit);
    stats_.explodeOnHitDamage = S(EffectKind::ExplodeOnHit);
    stats_.dashDamage = S(EffectKind::DashDamage);
    stats_.igniteOnHit = S(EffectKind::IgniteOnHit);
    stats_.shockOnHit = S(EffectKind::ShockOnHit);
    stats_.chillOnHit = S(EffectKind::ChillOnHit);
    stats_.corrodeOnHit = S(EffectKind::CorrodeOnHit);

    // M3 affinity SET BONUSES. Count owned items per affinity (a stack of N counts as N),
    // then a 3-set amplifies that element's application and a 5-set unlocks a signature
    // transform. This is the "build identity" payoff - committing to one affinity changes
    // HOW the build plays, not just its numbers.
    for (int& c : stats_.affinityCount) c = 0;
    for (const auto& kv : inventory_) {
        const ItemDef* d = find(kv.first);
        if (!d || d->affinity == Affinity::None) continue;
        stats_.affinityCount[static_cast<int>(d->affinity)] += kv.second;
    }
    const auto setCount = [&](Affinity a) { return stats_.affinityCount[static_cast<int>(a)]; };
    const auto three = [](int n) { return n >= 3; };
    const auto five  = [](int n) { return n >= 5; };
    if (three(setCount(Affinity::Pyro))) stats_.burnApplyMult = 1.5f;
    if (three(setCount(Affinity::Volt))) stats_.shockApplyMult = 1.5f;
    if (three(setCount(Affinity::Cryo))) stats_.chillApplyMult = 1.5f;
    if (three(setCount(Affinity::Acid))) stats_.corrodeApplyMult = 1.5f;
    stats_.burnDetonateOnKill = five(setCount(Affinity::Pyro));
    stats_.shockConduct       = five(setCount(Affinity::Volt));
    stats_.cryoNova           = five(setCount(Affinity::Cryo));
    stats_.corrodeSpread      = five(setCount(Affinity::Acid));
    // Kinetic 3/5: lean into mobility + aggression. Bulwark 3/5: survivability.
    if (three(setCount(Affinity::Kinetic))) { stats_.moveSpeedMult += 0.10f; stats_.dashCooldownMult *= 0.85f; }
    if (five(setCount(Affinity::Kinetic)))  { stats_.damageMult += 0.15f; }
    if (three(setCount(Affinity::Bulwark))) { stats_.damageReduction = clamp(stats_.damageReduction + 0.08f, 0.0f, 0.85f); }
    if (five(setCount(Affinity::Bulwark)))  { stats_.maxHealthBonus += 40; }
    // topAffinity = the affinity with the most owned items (for reward synergy surfacing).
    stats_.topAffinity = 0; int best = 0;
    for (int a = 1; a < static_cast<int>(Affinity::Count); ++a)
        if (stats_.affinityCount[a] > best) { best = stats_.affinityCount[a]; stats_.topAffinity = a; }
}

std::vector<std::string> Build::rollRewards(Rng& rng, int n, const std::vector<std::string>& excluded,
                                            float tierBias) const {
    // Tier-weighted pick without replacement across a mixed pool: passives ("p:")
    // plus weapons ("w:", weighted lower so they show occasionally, not every room).
    // tierBias (RunMods) scales the higher tiers up: Uncommon by (1+bias), Rare by
    // (1+2*bias) so a positive bias favours rares harder. bias 0 leaves the base 6/3/1.
    const float ub = std::max(0.0f, 1.0f + tierBias);
    const float rb = std::max(0.0f, 1.0f + 2.0f * tierBias);
    const float lb = std::max(0.0f, 1.0f + 3.0f * tierBias);
    auto weightOf = [ub, rb, lb](ItemTier t) -> float {
        switch (t) {
            case ItemTier::Common: return 6.0f;
            case ItemTier::Uncommon: return 3.0f * ub;
            case ItemTier::Rare: return 1.0f * rb;
            case ItemTier::Legendary: return 0.28f * lb;   // build-defining; rare, tier-bias favours it hard
        }
        return 1.0f;
    };
    auto isExcluded = [&](const std::string& id) {
        for (const std::string& e : excluded) if (e == id) return true;
        return false;
    };

    struct Entry { std::string id; float weight; };
    std::vector<Entry> pool;
    pool.reserve(catalog_.size() + weapons_.size());
    for (const ItemDef& d : catalog_) {
        const std::string id = "p:" + d.id;
        if (!isExcluded(id)) pool.push_back({ id, weightOf(d.tier) });
    }
    for (const WeaponDef& w : weapons_) {
        if (w.id == "pistol") continue;           // the starting sidearm is never a drop
        const std::string id = "w:" + w.id;
        if (!isExcluded(id)) pool.push_back({ id, weightOf(w.tier) * 1.5f }); // weapons show more (early power spike)
    }

    std::vector<std::string> out;
    const int picks = std::min(n, static_cast<int>(pool.size()));
    for (int p = 0; p < picks; ++p) {
        float total = 0.0f;
        for (const Entry& e : pool) total += e.weight;
        float r = rng.range(0.0f, total);
        int chosen = 0;
        for (int slot = 0; slot < static_cast<int>(pool.size()); ++slot) {
            if (r <= pool[static_cast<size_t>(slot)].weight) { chosen = slot; break; }
            r -= pool[static_cast<size_t>(slot)].weight;
            chosen = slot;
        }
        out.push_back(pool[static_cast<size_t>(chosen)].id);
        pool.erase(pool.begin() + chosen);
    }
    return out;
}

} // namespace pulse
