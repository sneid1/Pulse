#pragma once

// M2: the status-element combat layer (docs/Plan_PULSE_real_deal.md, Pillar 2a). Damage can
// carry an ELEMENT; enemies accumulate per-element stacks that tick and trigger. The four
// elements are a SYSTEM, not item flavor - owning a single status item turns on the whole
// behavior (Burn always DoTs, Cryo always chills/freezes, Corrode always amps, Shock always
// chains at threshold). M3 layers proc coefficients + many more sources on top; M2 is the
// foundation.
//
//   Burn   : damage-over-time proportional to stacks; decays. (M3 adds "detonate" consumers.)
//   Shock  : a charge that DISCHARGES at a threshold - a burst + an arc to nearby foes.
//   Cryo   : chill 0..1 slows the enemy; at full it FREEZES (hard CC); frozen foes SHATTER
//            (bonus damage on the next hit).
//   Corrode: stacks AMP incoming damage and further status application (armor melt); lingers.
//
// Element pair reactions:
//   Burn + Shock    : PLASMA SURGE, a short splash burst and extra shock charge.
//   Burn + Cryo     : THERMAL SHOCK, a steam burst that strips burn/chill into damage.
//   Burn + Corrode  : CAUSTIC FIRE, burn sticks harder and spits acid to nearby foes.
//   Shock + Cryo    : SUPERCONDUCT, a cold arc burst that primes nearby enemies.
//   Shock + Corrode : GALVANIC MELT, corrosion forces an immediate shock discharge.
//
// This module owns the per-enemy state + the decay + tuning + display helpers. The combat
// EFFECTS (DoT application, freeze CC, chain, shatter, amp) live in PulseGame, which has the
// enemy list, particles, and audio. All deterministic - no RNG - so --balance-sim reproduces.

#include "Engine/Math.hpp"
#include "Engine/Core/Mat.hpp"   // Vec3f (used by elementTint)

namespace pulse {

enum class Element { None, Burn, Shock, Cryo, Corrode, Count };

const char* elementName(Element e);
Vec3f elementTint(Element e);   // HDR particle/glow tint per element

// Per-enemy elemental accumulator (embedded in Enemy). Compact and POD; cleared on spawn.
struct StatusState {
    float burn = 0.0f;       // burn stacks (each ~ one DoT unit); decays
    float burnTick = 0.0f;   // internal: accumulator for the DoT cadence
    float shock = 0.0f;      // shock charge; discharges (chain) at kShockThreshold
    float chill = 0.0f;      // 0..1; movement slow = chill * kChillMaxSlow
    float frozen = 0.0f;     // remaining hard-freeze seconds (immobilized + brittle/shatter)
    float corrode = 0.0f;    // corrosion stacks; amps incoming damage + status

    bool any() const { return burn > 0.0f || shock > 0.0f || chill > 0.0f || frozen > 0.0f || corrode > 0.0f; }
    void clear() { *this = StatusState{}; }
};

// Decay all stacks toward zero over time (called per enemy per frame). Frozen counts down and,
// while frozen, chill is held at full so it does not melt mid-freeze.
void statusDecay(StatusState& s, float dt);

// Tuning (sim-tunable; balance lives here, never per-enemy HP).
namespace stat {
    constexpr float kBurnDecay      = 2.2f;   // stacks/sec
    constexpr float kBurnDotPerStack = 5.0f;  // damage/sec per burn stack
    constexpr float kBurnTickRate   = 0.25f;  // seconds between DoT ticks
    constexpr float kBurnCap        = 12.0f;

    constexpr float kShockDecay     = 0.6f;
    constexpr float kShockThreshold = 4.0f;   // charge needed to discharge a chain
    constexpr float kShockBurst     = 16.0f;  // damage to the struck enemy on discharge
    constexpr float kShockArc       = 11.0f;  // damage to each arced neighbour
    constexpr float kShockArcRange  = 5.0f;   // world units the arc reaches
    constexpr int   kShockArcCount  = 2;      // neighbours hit per discharge

    constexpr float kChillDecay     = 0.45f;
    constexpr float kChillMaxSlow   = 0.65f;  // max movement slow at full chill
    constexpr float kFreezeDuration = 1.6f;   // hard-CC time when chill maxes
    constexpr float kShatterMult    = 1.8f;   // bonus damage multiplier on a frozen/primed foe

    constexpr float kCorrodeDecay   = 0.25f;  // lingers
    constexpr float kCorrodeCap     = 10.0f;
    constexpr float kCorrodeAmpPerStack = 0.06f;  // +6% incoming damage per corrode stack
    constexpr float kCorrodeStatusAmpPerStack = 0.05f; // +5% status application per stack

    constexpr float kComboCooldown = 0.45f;       // per-enemy spam guard for pair reactions
    constexpr float kPlasmaDamage = 10.0f;
    constexpr float kThermalShockDamage = 18.0f;
    constexpr float kCausticFireBurn = 1.8f;
    constexpr float kSuperconductDamage = 14.0f;
    constexpr float kGalvanicMeltDamage = 12.0f;
}

} // namespace pulse
