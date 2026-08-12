#pragma once

#include "session/StreamSettings.h"

#include <string_view>

namespace gateway::moonlight {

struct MoonlightVideoProfile {
    int videoFormat;
    int colorSpace;
    int colorRange;
    int bitDepth;
    bool hdr;
    std::string_view name;
};

MoonlightVideoProfile moonlightVideoProfile(const StreamSettings& settings);

} // namespace gateway::moonlight
