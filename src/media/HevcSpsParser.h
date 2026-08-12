#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace gateway {

struct HevcSpsInfo {
    int profileIdc;
    bool main10CompatibleProfile;
    int chromaFormatIdc;
    int bitDepthLuma;
    int bitDepthChroma;

    [[nodiscard]] bool isMain10_420() const;
};

std::optional<HevcSpsInfo> parseHevcSps(std::span<const std::uint8_t> annexB);

} // namespace gateway
