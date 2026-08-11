#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace moonlight {

class H264AnnexBReader {
public:
    using AccessUnit = std::vector<std::uint8_t>;

    explicit H264AnnexBReader(const std::filesystem::path& path);

    [[nodiscard]] const std::vector<AccessUnit>& accessUnits() const;
    [[nodiscard]] bool hasSps() const;
    [[nodiscard]] bool hasPps() const;
    [[nodiscard]] bool hasIdr() const;
    [[nodiscard]] bool hasLongStartCodes() const;
    [[nodiscard]] bool hasShortStartCodes() const;

private:
    std::vector<AccessUnit> accessUnits_;
    bool hasSps_ = false;
    bool hasPps_ = false;
    bool hasIdr_ = false;
    bool hasLongStartCodes_ = false;
    bool hasShortStartCodes_ = false;
};

} // namespace moonlight
