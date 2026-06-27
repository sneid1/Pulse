#include "Game/Pulse.hpp"

namespace pulse {

void Pulse::update(float dt, bool inCombat) {
    if (chainTimer_ > 0.0f) {
        chainTimer_ = std::max(0.0f, chainTimer_ - dt);
        if (chainTimer_ <= 0.0f) chain_ = 0;
    }
    // Drain: a small bleed even while a chain is live (so the top demands a steady kill-rate),
    // a faster bleed once the chain lapses, and a hard bleed out of combat (momentum dies in a
    // safe room).
    float drain;
    if (!inCombat) drain = kDrainIdle;
    else if (chainTimer_ > 0.0f) drain = kDrainChain;
    else drain = kDrainCombat;
    meter_ = std::max(0.0f, meter_ - drain * dt);

    // Smoothed direction: rate captures kills (which raised meter_ since the last update) plus
    // this frame's drain. An EMA keeps the readout from chattering between RISING/FALLING.
    const float h = std::max(dt, 1e-4f);
    const float rate = (meter_ - lastMeter_) / h;
    velocity_ += (rate - velocity_) * std::min(1.0f, dt * 8.0f);
    lastMeter_ = meter_;

    if (lossFlash_ > 0.0f) lossFlash_ = std::max(0.0f, lossFlash_ - dt / kLossFlash);
}

void Pulse::onKill(bool headshot) {
    const int link = std::min(chain_, kChainCap);
    float rise = kRiseBase + kRiseChainStep * static_cast<float>(link);
    if (headshot) rise *= 1.5f;
    rise *= (1.0f - kRiseSoftCap * meter_);   // the last stretch to OVERPULSE is sticky-hard
    meter_ = std::min(1.0f, meter_ + rise);
    chainTimer_ = kChainWindow;
    ++chain_;
}

void Pulse::onDashThrough(int enemiesNear) {
    if (enemiesNear <= 0) return;
    meter_ = std::min(1.0f, meter_ + 0.04f * static_cast<float>(std::min(enemiesNear, 4)));
    chainTimer_ = std::max(chainTimer_, kChainWindow * 0.6f);  // keep the groove alive through the dash
}

void Pulse::onHit() {
    meter_ = std::max(0.0f, meter_ - kHitDrop);
    chain_ = 0;
    chainTimer_ = 0.0f;
    lossFlash_ = 1.0f;
}

PulseTier Pulse::tier() const {
    if (meter_ >= kOver) return PulseTier::Overpulse;
    if (meter_ >= kBurning) return PulseTier::Burning;
    if (meter_ >= kSurging) return PulseTier::Surging;
    if (meter_ >= kCharged) return PulseTier::Charged;
    return PulseTier::Dormant;
}

PulseDir Pulse::dir() const {
    if (overpulse()) return PulseDir::Locked;
    if (velocity_ > kDirEps) return PulseDir::Rising;
    if (velocity_ < -kDirEps) return PulseDir::Falling;
    return PulseDir::Stable;
}

const char* pulseTierName(PulseTier t) {
    switch (t) {
        case PulseTier::Dormant:   return "DORMANT";
        case PulseTier::Charged:   return "CHARGED";
        case PulseTier::Surging:   return "SURGING";
        case PulseTier::Burning:   return "BURNING";
        case PulseTier::Overpulse: return "OVERPULSE";
    }
    return "?";
}

const char* pulseDirName(PulseDir d) {
    switch (d) {
        case PulseDir::Rising:  return "RISING";
        case PulseDir::Stable:  return "STABLE";
        case PulseDir::Falling: return "FALLING";
        case PulseDir::Locked:  return "LOCKED";
    }
    return "?";
}

} // namespace pulse
