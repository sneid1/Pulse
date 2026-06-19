#pragma once

#include <string>

#include "Game/Tunables.hpp"

namespace pulse {

struct ConfigLoadResult {
    bool loaded = false;
    std::string path;
    std::string message;
};

ConfigLoadResult loadTunablesFromDisk(Tunables& tunables);

} // namespace pulse
