#pragma once

// Phase C: meta-progression (docs/Plan_PULSE_roguelite.md). A persistent profile in
// the user/app dir holds a meta-currency and a set of unlocked content ids. Meta
// spending is CONTENT/OPTION UNLOCKS ONLY (new items + starting options = more
// choices), never permanent stat power - that would grind-gate and cut against the
// clean-skill thesis. Content is keyed by stable prefixed string ids ("w:"/"p:"),
// never an array index, so adding content later never invalidates an old save.

#include <array>
#include <string>
#include <unordered_set>
#include <vector>

namespace pulse {

struct UnlockDef {
    std::string id;     // prefixed content id, e.g. "w:railbolt" / "p:frag_payload"
    std::string name;   // Hub label
    int cost = 20;
};

class Meta {
public:
    Meta();              // registers the unlockable catalog

    void load();         // read the profile; a missing/corrupt file degrades to fresh
    bool save() const;   // write the versioned profile to the user/app dir

    int currency() const { return currency_; }
    void addCurrency(int amount) { if (amount > 0) currency_ += amount; }

    // Balance-sim support: run from a clean in-memory profile without touching disk.
    void resetToFresh() { currency_ = 0; unlocked_.clear(); startingWeapon_ = "pistol"; heat_ = 0; maxHeatUnlocked_ = 0; mirror_.fill(0); contractMask_ = 0; }
    void setPersistence(bool on) { persist_ = on; }

    const std::vector<UnlockDef>& unlockables() const { return unlockables_; }
    bool isGated(const std::string& id) const;     // appears in the unlock catalog
    bool isUnlocked(const std::string& id) const;
    bool buy(const std::string& id);               // spend to unlock; false if can't

    int unlockedCount() const { return static_cast<int>(unlocked_.size()); }

    // Heat / ascension (Feature 4): a stackable, unlock-gated difficulty selected before a
    // run. maxHeatUnlocked_ rises when a run is cleared at the current heat. Both persist.
    static constexpr int kMaxHeat = 10;
    int heat() const { return heat_; }
    int maxHeatUnlocked() const { return maxHeatUnlocked_; }
    void setHeat(int h) { heat_ = h < 0 ? 0 : (h > kMaxHeat ? kMaxHeat : h); }  // freely selectable (UI Overhaul: [+] add modifier)
    void unlockHeat(int h) { if (h > maxHeatUnlocked_) maxHeatUnlocked_ = (h > kMaxHeat ? kMaxHeat : h); }

    // RUN CONTRACTS (UI Overhaul): a bitmask of the active named contracts, selected in the Hub.
    // Each toggles a difficulty knob and raises the payout (see RunMods contractMods/Payout).
    // Persisted so a preferred contract loadout sticks between sessions.
    unsigned contractMask() const { return contractMask_; }
    void toggleContract(int i) { if (i >= 0 && i < 31) contractMask_ ^= (1u << i); }
    void setContractMask(unsigned m) { contractMask_ = m; }

    // M6 hybrid meta: the MIRROR - a small, capped, permanent upgrade board bought with meta
    // currency. Skill-first stays the rule (the decision was "hybrid, not a grind-gate"), so every
    // node is modest and hard-capped; they give a reason to come back without trivializing a run.
    // Levels persist in the save (a new key, so old saves load with all-zero Mirror).
    enum MirrorNode { MirrorVigor, MirrorPlating, MirrorMomentum, MirrorAvarice, MirrorFortune, MirrorAdrenaline, MirrorCount };
    int  mirrorLevel(int node) const { return (node >= 0 && node < MirrorCount) ? mirror_[static_cast<size_t>(node)] : 0; }
    int  mirrorMax(int node) const;
    int  mirrorCost(int node) const;          // cost of the NEXT level, or -1 if maxed
    const char* mirrorName(int node) const;
    const char* mirrorDesc(int node) const;
    bool upgradeMirror(int node);             // spend currency to raise a node; false if can't
    // Run-start bonuses derived from the Mirror levels (applied in resetRun).
    int   mirrorBonusHealth() const { return 6 * mirrorLevel(MirrorVigor); }
    int   mirrorBonusShield() const { return 8 * mirrorLevel(MirrorPlating); }
    int   mirrorBonusScrap()  const { return 8 * mirrorLevel(MirrorAvarice); }
    float mirrorStartPulse()  const { return 0.08f * static_cast<float>(mirrorLevel(MirrorMomentum)); }
    float mirrorTierBias()    const { return 0.04f * static_cast<float>(mirrorLevel(MirrorFortune)); }
    float mirrorChargeRate()  const { return 1.0f + 0.06f * static_cast<float>(mirrorLevel(MirrorAdrenaline)); }

    // Starting-loadout pick (Phase C Hub): the weapon a run begins with. Options are
    // the pistol plus any weapon unlocked via meta (more unlocks = more start options).
    const std::string& startingWeapon() const { return startingWeapon_; }
    void setStartingWeapon(const std::string& id) { startingWeapon_ = id; }
    std::vector<std::string> starterOptions() const;   // raw weapon ids (e.g. "pistol","railbolt")

    // Prefixed content ids that are gated AND not yet unlocked -> excluded from the
    // in-run reward pool until bought (this is how unlocks expand the run pools).
    std::vector<std::string> lockedContent() const;

private:
    static std::string savePath();

    int currency_ = 0;
    bool persist_ = true;   // false in balance-sim mode (no disk writes)
    int heat_ = 0;              // selected ascension heat for the next run (balance-sim path)
    int maxHeatUnlocked_ = 0;   // highest heat unlocked so far (gates the selector)
    unsigned contractMask_ = 0; // active RUN CONTRACTS bitmask (interactive difficulty/payout)
    std::string startingWeapon_ = "pistol";
    std::array<int, MirrorCount> mirror_{};   // M6 Mirror node levels (persisted)
    std::unordered_set<std::string> unlocked_;
    std::vector<UnlockDef> unlockables_;
};

} // namespace pulse
