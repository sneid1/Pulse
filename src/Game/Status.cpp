#include "Game/Status.hpp"

#include <algorithm>

namespace pulse {

const char* elementName(Element e) {
    switch (e) {
        case Element::Burn: return "BURN";
        case Element::Shock: return "SHOCK";
        case Element::Cryo: return "CRYO";
        case Element::Corrode: return "CORRODE";
        case Element::None:
        case Element::Count: break;
    }
    return "";
}

Vec3f elementTint(Element e) {
    switch (e) {
        case Element::Burn:    return { 1.70f, 0.55f, 0.18f };  // ember orange
        case Element::Shock:   return { 0.55f, 0.85f, 1.80f };  // electric cyan-white
        case Element::Cryo:    return { 0.55f, 0.95f, 1.55f };  // frost blue
        case Element::Corrode: return { 0.75f, 1.55f, 0.40f };  // acid green
        case Element::None:
        case Element::Count: break;
    }
    return { 1.0f, 1.0f, 1.0f };
}

void statusDecay(StatusState& s, float dt) {
    if (s.frozen > 0.0f) {
        s.frozen = std::max(0.0f, s.frozen - dt);
        s.chill = 1.0f;                 // held at full while frozen so it does not melt mid-CC
        if (s.frozen <= 0.0f) {
            s.chill = 0.4f;             // thaws to a partial chill, not a hard reset
        }
    } else if (s.chill > 0.0f) {
        s.chill = std::max(0.0f, s.chill - stat::kChillDecay * dt);
    }
    if (s.burn > 0.0f) s.burn = std::max(0.0f, s.burn - stat::kBurnDecay * dt);
    if (s.shock > 0.0f) s.shock = std::max(0.0f, s.shock - stat::kShockDecay * dt);
    if (s.corrode > 0.0f) s.corrode = std::max(0.0f, s.corrode - stat::kCorrodeDecay * dt);
}

} // namespace pulse
