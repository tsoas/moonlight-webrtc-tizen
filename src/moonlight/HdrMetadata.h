#pragma once

#include <Limelight.h>

#include <array>
#include <optional>
#include <string>

namespace gateway::moonlight {

struct HdrChromaticity {
    double x;
    double y;
};

struct HdrMetadata {
    std::array<std::optional<HdrChromaticity>, 3> displayPrimaries;
    std::optional<HdrChromaticity> whitePoint;
    std::optional<double> maxDisplayLuminanceNits;
    std::optional<double> minDisplayLuminanceNits;
    std::optional<double> maxContentLightLevelNits;
    std::optional<double> maxFrameAverageLightLevelNits;
    std::optional<double> maxFullFrameLuminanceNits;
};

HdrMetadata convertHdrMetadata(const SS_HDR_METADATA& metadata);
std::string formatHdrMetadata(const HdrMetadata& metadata);

} // namespace gateway::moonlight
