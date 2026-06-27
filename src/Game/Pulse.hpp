#pragma once

// M1: The Pulse - PULSE's signature momentum mechanic (docs/Plan_PULSE_real_deal.md,
// Pillar 1). Aggression builds momentum; momentum AMPLIFIES build-defined systems,
// biases loot toward greed, and drives the techno. Taking a hit knocks it down; idling
// drains it. This promotes the old cosmetic combatIntensity_ float into the real
// gameplay + music driver: PulseGame derives its music intensity from the meter and
// reads the amplifiers at the status / charge / loot sites.
//
// IMPORTANT (PULSE UI Overhaul decision): the Pulse is NOT a generic damage multiplier.
// It scales STATUS (element application / set amplifiers), KIT (ability + aspect charge),
// and LOOT tier, plus a modest momentum mobility bonus. Do not reintroduce a flat
// damage/fire-rate grant - escalation stays count / aggression / composition + the Pulse.
//
// The meter is a single [0,1] value. Discrete STATE BANDS (DORMANT / CHARGED / SURGING /
// BURNING / OVERPULSE) are derived from thresholds at 20 / 50 / 80 / 100 for the HUD +
// music; the amplifiers ramp CONTINUOUSLY with the meter so there is no power cliff at a
// band edge. The HUD also reports a DIRECTION (RISING / STABLE / FALLING / LOCKED) read
// from the meter's smoothed velocity. All tuning lives here, and it never touches
// per-enemy HP. Deterministic: fed only by gameplay events, no RNG, so --balance-sim
// reproduces it byte-for-byte.

#include <algorithm>
#include <cmath>

namespace pulse {

// State bands (spec): DORMANT 0-19, CHARGED 20-49, SURGING 50-79, BURNING 80-99,
// OVERPULSE 100. Names kept stable for the HUD / Field Manual / music driver.
enum class PulseTier { Dormant, Charged, Surging, Burning, Overpulse };
const char* pulseTierName(PulseTier t);

// Live direction of travel for the HUD's STATE . DIRECTION readout.
enum class PulseDir { Rising, Stable, Falling, Locked };
const char* pulseDirName(PulseDir d);

class Pulse {
public:
    void reset() {
        meter_ = 0.0f; chain_ = 0; chainTimer_ = 0.0f;
        lastMeter_ = 0.0f; velocity_ = 0.0f; lossFlash_ = 0.0f;
    }

    // Per-frame: drain toward 0 (slow while a kill-chain is live, so the groove holds),
    // decay the chain window, and update the smoothed velocity + loss-flash. inCombat=false
    // drains hard so momentum dies in safe rooms.
    void update(float dt, bool inCombat);

    // Aggression feeds (rises). onKill ramps with the live kill-chain; headshots hit harder.
    void onKill(bool headshot);
    void onDashThrough(int enemiesNear);   // dashing into a pack is momentum
    void bump(float v) { meter_ = std::min(1.0f, meter_ + (v > 0.0f ? v : 0.0f)); }

    // Taking a hit costs momentum: drop a chunk, break the chain, flash the loss.
    void onHit();

    float meter01() const { return meter_; }
    PulseTier tier() const;
    PulseDir  dir() const;
    bool overpulse() const { return meter_ >= kOver; }
    int  chain() const { return chain_; }
    // The live integer shown in the meter's number cap (0-100; clamps to 100 at OVERPULSE).
    int  value() const {
        if (overpulse()) return 100;
        int v = static_cast<int>(std::lround(meter_ * 100.0f));
        return v < 0 ? 0 : (v > 99 ? 99 : v);
    }
    // 0..1 brief flash after taking a hit (drives a red loss tell on the meter).
    float lossFlash01() const { return lossFlash_; }

    // Amplifiers (continuous in the meter; 1.0 at empty). Modest by design - the Pulse is a
    // flow reward that scales what your BUILD already does, not a build replacement.
    //   STATUS: element application (on-hit stacks) + set-bonus amplifier magnitudes.
    float statusMult() const { return 1.0f + meter_ * kStatusGain + (overpulse() ? kOverStatus : 0.0f); }
    //   KIT: ability + aspect charge speed.
    float chargeMult() const { return 1.0f + meter_ * kChargeGain; }
    //   Momentum mobility (feel, not damage): a small move-speed lift while hot.
    float moveMult() const { return 1.0f + meter_ * kMoveGain; }
    //   LOOT greed: extra loot tier-bias + scrap multiplier earned by fighting hot.
    float lootTierBias() const { return meter_ * kLootBias; }
    float scrapMult() const { return 1.0f + meter_ * kScrapGain; }

    // State thresholds (also the HUD meter notch positions): 20 / 50 / 80 / 100.
    static constexpr float kCharged = 0.20f, kSurging = 0.50f, kBurning = 0.80f, kOver = 0.995f;
    // Greed loot-tier bias ceiling. Public so PulseGame can apply the ROOM-PEAK Pulse as a
    // reward bias on clear (you fought hot -> better loot), not just the live meter.
    static constexpr float kLootBias = 0.35f;

private:
    // Amplifier ceilings (at full meter).
    static constexpr float kStatusGain = 0.60f;   // up to +60% status application / set magnitude
    static constexpr float kOverStatus = 0.25f;   // +25% more in OVERPULSE
    static constexpr float kChargeGain = 0.50f;   // ability + aspect charge speed
    static constexpr float kMoveGain   = 0.12f;   // momentum mobility (feel)
    static constexpr float kScrapGain  = 0.50f;
    // Rise / drain model. A small bleed runs even while a chain is live, so holding the very
    // top (OVERPULSE) demands a CONTINUOUS high kill-rate rather than just "don't get hit" -
    // most play settles in SURGING/BURNING and OVERPULSE is a genuine streak peak (sim-tuned).
    static constexpr float kRiseBase      = 0.075f;  // per kill at chain 0
    static constexpr float kRiseChainStep = 0.010f;  // + per chain link
    static constexpr float kRiseSoftCap   = 0.45f;   // rise *= (1 - kRiseSoftCap*meter): top is sticky-hard
    static constexpr int   kChainCap      = 12;
    static constexpr float kChainWindow   = 3.5f;    // seconds a chain stays live (a brief lull keeps the groove)
    static constexpr float kHitDrop       = 0.30f;   // momentum lost per hit
    static constexpr float kDrainChain    = 0.035f;  // per sec even while a chain is live
    static constexpr float kDrainCombat   = 0.120f;  // per sec in combat once the chain lapses
    static constexpr float kDrainIdle     = 0.55f;   // per sec out of combat
    // Direction model.
    static constexpr float kDirEps        = 0.020f;  // |velocity| below this reads STABLE
    static constexpr float kLossFlash     = 0.55f;   // seconds the hit loss tell lingers

    float meter_ = 0.0f;
    int   chain_ = 0;
    float chainTimer_ = 0.0f;
    float lastMeter_ = 0.0f;   // meter at the end of the previous update (for velocity)
    float velocity_ = 0.0f;    // smoothed per-second change (drives the direction readout)
    float lossFlash_ = 0.0f;   // seconds remaining on the take-a-hit loss flash
};

} // namespace pulse
