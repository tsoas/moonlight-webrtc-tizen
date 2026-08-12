#include "media/HevcSpsParser.h"

#include <array>
#include <limits>
#include <vector>

namespace gateway {
namespace {

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> bytes)
        : bytes_(bytes)
    {
    }

    std::optional<std::uint32_t> readBits(int count)
    {
        if (count < 0 || count > 32 || bitOffset_ + static_cast<std::size_t>(count)
                > bytes_.size() * 8) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (int index = 0; index < count; ++index) {
            value = (value << 1)
                | ((bytes_[bitOffset_ / 8] >> (7 - (bitOffset_ % 8))) & 1U);
            ++bitOffset_;
        }
        return value;
    }

    bool skip(int count)
    {
        if (count < 0 || bitOffset_ + static_cast<std::size_t>(count)
                > bytes_.size() * 8) {
            return false;
        }
        bitOffset_ += static_cast<std::size_t>(count);
        return true;
    }

    std::optional<std::uint32_t> readUnsignedExpGolomb()
    {
        int leadingZeros = 0;
        while (true) {
            const auto bit = readBits(1);
            if (!bit) {
                return std::nullopt;
            }
            if (*bit != 0) {
                break;
            }
            if (++leadingZeros > 31) {
                return std::nullopt;
            }
        }
        const auto suffix = readBits(leadingZeros);
        if (!suffix) {
            return std::nullopt;
        }
        return ((std::uint32_t{1} << leadingZeros) - 1U) + *suffix;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t bitOffset_ = 0;
};

std::size_t startCodeLength(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset + 3 <= bytes.size() && bytes[offset] == 0 && bytes[offset + 1] == 0
        && bytes[offset + 2] == 1) {
        return 3;
    }
    if (offset + 4 <= bytes.size() && bytes[offset] == 0 && bytes[offset + 1] == 0
        && bytes[offset + 2] == 0 && bytes[offset + 3] == 1) {
        return 4;
    }
    return 0;
}

std::vector<std::uint8_t> rbsp(std::span<const std::uint8_t> payload)
{
    std::vector<std::uint8_t> result;
    result.reserve(payload.size());
    int zeroCount = 0;
    for (const auto byte : payload) {
        if (zeroCount >= 2 && byte == 0x03) {
            zeroCount = 0;
            continue;
        }
        result.push_back(byte);
        zeroCount = byte == 0 ? zeroCount + 1 : 0;
    }
    return result;
}

std::optional<HevcSpsInfo> parseSpsNal(std::span<const std::uint8_t> nal)
{
    if (nal.size() < 3 || ((nal[0] >> 1) & 0x3F) != 33) {
        return std::nullopt;
    }
    const auto bytes = rbsp(nal.subspan(2));
    BitReader reader(bytes);
    if (!reader.skip(4)) {
        return std::nullopt;
    }
    const auto maxSubLayersMinus1 = reader.readBits(3);
    if (!maxSubLayersMinus1 || !reader.skip(1) || !reader.skip(3)) {
        return std::nullopt;
    }
    const auto profileIdc = reader.readBits(5);
    if (!profileIdc) {
        return std::nullopt;
    }
    std::array<bool, 32> profileCompatibility{};
    for (auto& compatible : profileCompatibility) {
        const auto bit = reader.readBits(1);
        if (!bit) {
            return std::nullopt;
        }
        compatible = *bit != 0;
    }
    if (!reader.skip(48) || !reader.skip(8)) {
        return std::nullopt;
    }

    std::array<bool, 7> subLayerProfilePresent{};
    std::array<bool, 7> subLayerLevelPresent{};
    for (std::uint32_t index = 0; index < *maxSubLayersMinus1; ++index) {
        const auto profilePresent = reader.readBits(1);
        const auto levelPresent = reader.readBits(1);
        if (!profilePresent || !levelPresent) {
            return std::nullopt;
        }
        subLayerProfilePresent[index] = *profilePresent != 0;
        subLayerLevelPresent[index] = *levelPresent != 0;
    }
    if (*maxSubLayersMinus1 > 0
        && !reader.skip(static_cast<int>((8 - *maxSubLayersMinus1) * 2))) {
        return std::nullopt;
    }
    for (std::uint32_t index = 0; index < *maxSubLayersMinus1; ++index) {
        if (subLayerProfilePresent[index] && !reader.skip(88)) {
            return std::nullopt;
        }
        if (subLayerLevelPresent[index] && !reader.skip(8)) {
            return std::nullopt;
        }
    }

    const auto sequenceId = reader.readUnsignedExpGolomb();
    const auto chromaFormat = reader.readUnsignedExpGolomb();
    if (!sequenceId || !chromaFormat || *chromaFormat > 3) {
        return std::nullopt;
    }
    if (*chromaFormat == 3 && !reader.skip(1)) {
        return std::nullopt;
    }
    const auto width = reader.readUnsignedExpGolomb();
    const auto height = reader.readUnsignedExpGolomb();
    const auto conformanceWindow = reader.readBits(1);
    if (!width || !height || !conformanceWindow) {
        return std::nullopt;
    }
    if (*conformanceWindow != 0) {
        for (int index = 0; index < 4; ++index) {
            if (!reader.readUnsignedExpGolomb()) {
                return std::nullopt;
            }
        }
    }
    const auto bitDepthLumaMinus8 = reader.readUnsignedExpGolomb();
    const auto bitDepthChromaMinus8 = reader.readUnsignedExpGolomb();
    if (!bitDepthLumaMinus8 || !bitDepthChromaMinus8
        || *bitDepthLumaMinus8 > 8 || *bitDepthChromaMinus8 > 8) {
        return std::nullopt;
    }
    return HevcSpsInfo{
        static_cast<int>(*profileIdc),
        *profileIdc == 2 || profileCompatibility[2],
        static_cast<int>(*chromaFormat),
        static_cast<int>(*bitDepthLumaMinus8 + 8),
        static_cast<int>(*bitDepthChromaMinus8 + 8),
    };
}

} // namespace

bool HevcSpsInfo::isMain10_420() const
{
    return main10CompatibleProfile && chromaFormatIdc == 1
        && bitDepthLuma == 10 && bitDepthChroma == 10;
}

std::optional<HevcSpsInfo> parseHevcSps(std::span<const std::uint8_t> annexB)
{
    for (std::size_t offset = 0; offset < annexB.size();) {
        const auto prefix = startCodeLength(annexB, offset);
        if (prefix == 0) {
            ++offset;
            continue;
        }
        const auto nalStart = offset + prefix;
        auto nalEnd = nalStart;
        while (nalEnd < annexB.size() && startCodeLength(annexB, nalEnd) == 0) {
            ++nalEnd;
        }
        if (const auto parsed = parseSpsNal(annexB.subspan(nalStart, nalEnd - nalStart))) {
            return parsed;
        }
        offset = nalEnd;
    }
    return std::nullopt;
}

} // namespace gateway
