#include "moonlight/HdrMetadata.h"

#include <iomanip>
#include <sstream>

namespace gateway::moonlight {
namespace {

std::optional<HdrChromaticity> chromaticity(std::uint16_t x, std::uint16_t y)
{
    if (x == 0 && y == 0) {
        return std::nullopt;
    }
    return HdrChromaticity{x / 50000.0, y / 50000.0};
}

std::optional<double> positiveNits(std::uint16_t value, double scale = 1.0)
{
    return value == 0 ? std::nullopt : std::optional<double>(value / scale);
}

void appendValue(std::ostringstream& output,
                 std::string_view name,
                 const std::optional<double>& value)
{
    output << ", " << name << '=';
    if (value) {
        output << *value << " nits";
    } else {
        output << "unavailable";
    }
}

} // namespace

HdrMetadata convertHdrMetadata(const SS_HDR_METADATA& metadata)
{
    HdrMetadata converted;
    for (std::size_t index = 0; index < converted.displayPrimaries.size(); ++index) {
        converted.displayPrimaries[index] = chromaticity(
            metadata.displayPrimaries[index].x, metadata.displayPrimaries[index].y);
    }
    converted.whitePoint = chromaticity(metadata.whitePoint.x, metadata.whitePoint.y);
    converted.maxDisplayLuminanceNits = positiveNits(metadata.maxDisplayLuminance);
    converted.minDisplayLuminanceNits = positiveNits(metadata.minDisplayLuminance, 10000.0);
    converted.maxContentLightLevelNits = positiveNits(metadata.maxContentLightLevel);
    converted.maxFrameAverageLightLevelNits =
        positiveNits(metadata.maxFrameAverageLightLevel);
    converted.maxFullFrameLuminanceNits =
        positiveNits(metadata.maxFullFrameLuminance);
    return converted;
}

std::string formatHdrMetadata(const HdrMetadata& metadata)
{
    std::ostringstream output;
    output << std::fixed << std::setprecision(5) << "HDR metadata: primaries=";
    constexpr std::array names{'R', 'G', 'B'};
    for (std::size_t index = 0; index < metadata.displayPrimaries.size(); ++index) {
        if (index != 0) {
            output << '/';
        }
        output << names[index] << '(';
        if (const auto& primary = metadata.displayPrimaries[index]) {
            output << primary->x << ',' << primary->y;
        } else {
            output << "unavailable";
        }
        output << ')';
    }
    output << ", whitePoint=";
    if (metadata.whitePoint) {
        output << '(' << metadata.whitePoint->x << ',' << metadata.whitePoint->y << ')';
    } else {
        output << "unavailable";
    }
    output << std::setprecision(4);
    appendValue(output, "maxDisplay", metadata.maxDisplayLuminanceNits);
    appendValue(output, "minDisplay", metadata.minDisplayLuminanceNits);
    appendValue(output, "MaxCLL", metadata.maxContentLightLevelNits);
    appendValue(output, "MaxFALL", metadata.maxFrameAverageLightLevelNits);
    appendValue(output, "maxFullFrame", metadata.maxFullFrameLuminanceNits);
    return output.str();
}

} // namespace gateway::moonlight
