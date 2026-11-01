#include "media/Av1SequenceHeaderParser.h"

namespace gateway {
namespace {

constexpr int ObuSequenceHeader = 1;
constexpr int ColorPrimariesBt709 = 1;
constexpr int TransferSrgb = 13;
constexpr int MatrixIdentity = 0;

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> bytes)
        : bytes_(bytes)
    {
    }

    std::optional<std::uint32_t> readBits(int count)
    {
        if (count < 0 || count > 32
            || bitOffset_ + static_cast<std::size_t>(count) > bytes_.size() * 8) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (int index = 0; index < count; ++index) {
            value = (value << 1) | ((bytes_[bitOffset_ / 8] >> (7 - (bitOffset_ % 8))) & 1U);
            ++bitOffset_;
        }
        return value;
    }

    std::optional<bool> readFlag()
    {
        const auto bit = readBits(1);
        return bit ? std::optional<bool>(*bit != 0) : std::nullopt;
    }

    // uvlc() from the AV1 spec, 4.10.3.
    bool skipUvlc()
    {
        int leadingZeros = 0;
        while (true) {
            const auto bit = readBits(1);
            if (!bit) {
                return false;
            }
            if (*bit != 0) {
                break;
            }
            ++leadingZeros;
        }
        return leadingZeros >= 32 || readBits(leadingZeros).has_value();
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t bitOffset_ = 0;
};

// Reads only as far as color_config(), which is all the HDR check needs.
std::optional<Av1SequenceHeaderInfo> parseSequenceHeaderPayload(
    std::span<const std::uint8_t> payload)
{
    BitReader reader(payload);
    const auto seqProfile = reader.readBits(3);
    const auto stillPicture = reader.readFlag();
    const auto reducedStillPictureHeader = reader.readFlag();
    if (!seqProfile || !stillPicture || !reducedStillPictureHeader) {
        return std::nullopt;
    }

    if (*reducedStillPictureHeader) {
        if (!reader.readBits(5)) {
            return std::nullopt;
        }
    } else {
        const auto timingInfoPresent = reader.readFlag();
        if (!timingInfoPresent) {
            return std::nullopt;
        }
        bool decoderModelInfoPresent = false;
        int bufferDelayLength = 0;
        if (*timingInfoPresent) {
            if (!reader.readBits(32) || !reader.readBits(32)) {
                return std::nullopt;
            }
            const auto equalPictureInterval = reader.readFlag();
            if (!equalPictureInterval || (*equalPictureInterval && !reader.skipUvlc())) {
                return std::nullopt;
            }
            const auto modelInfoPresent = reader.readFlag();
            if (!modelInfoPresent) {
                return std::nullopt;
            }
            decoderModelInfoPresent = *modelInfoPresent;
            if (decoderModelInfoPresent) {
                const auto bufferDelayLengthMinus1 = reader.readBits(5);
                if (!bufferDelayLengthMinus1 || !reader.readBits(32) || !reader.readBits(5)
                    || !reader.readBits(5)) {
                    return std::nullopt;
                }
                bufferDelayLength = static_cast<int>(*bufferDelayLengthMinus1) + 1;
            }
        }
        const auto initialDisplayDelayPresent = reader.readFlag();
        const auto operatingPointsMinus1 = reader.readBits(5);
        if (!initialDisplayDelayPresent || !operatingPointsMinus1) {
            return std::nullopt;
        }
        for (std::uint32_t point = 0; point <= *operatingPointsMinus1; ++point) {
            const auto idc = reader.readBits(12);
            const auto levelIdx = reader.readBits(5);
            if (!idc || !levelIdx || (*levelIdx > 7 && !reader.readBits(1))) {
                return std::nullopt;
            }
            if (decoderModelInfoPresent) {
                const auto decoderModelPresent = reader.readFlag();
                if (!decoderModelPresent) {
                    return std::nullopt;
                }
                if (*decoderModelPresent
                    && (!reader.readBits(bufferDelayLength) || !reader.readBits(bufferDelayLength)
                        || !reader.readBits(1))) {
                    return std::nullopt;
                }
            }
            if (*initialDisplayDelayPresent) {
                const auto delayPresent = reader.readFlag();
                if (!delayPresent || (*delayPresent && !reader.readBits(4))) {
                    return std::nullopt;
                }
            }
        }
    }

    const auto frameWidthBitsMinus1 = reader.readBits(4);
    const auto frameHeightBitsMinus1 = reader.readBits(4);
    if (!frameWidthBitsMinus1 || !frameHeightBitsMinus1
        || !reader.readBits(static_cast<int>(*frameWidthBitsMinus1) + 1)
        || !reader.readBits(static_cast<int>(*frameHeightBitsMinus1) + 1)) {
        return std::nullopt;
    }
    if (!*reducedStillPictureHeader) {
        const auto frameIdNumbersPresent = reader.readFlag();
        if (!frameIdNumbersPresent || (*frameIdNumbersPresent && !reader.readBits(7))) {
            return std::nullopt;
        }
    }
    // use_128x128_superblock, enable_filter_intra, enable_intra_edge_filter.
    if (!reader.readBits(3)) {
        return std::nullopt;
    }
    if (!*reducedStillPictureHeader) {
        // enable_interintra_compound, enable_masked_compound, enable_warped_motion,
        // enable_dual_filter.
        if (!reader.readBits(4)) {
            return std::nullopt;
        }
        const auto enableOrderHint = reader.readFlag();
        if (!enableOrderHint || (*enableOrderHint && !reader.readBits(2))) {
            return std::nullopt;
        }
        const auto chooseScreenContentTools = reader.readFlag();
        if (!chooseScreenContentTools) {
            return std::nullopt;
        }
        // SELECT_SCREEN_CONTENT_TOOLS is 2, which is non-zero like an explicit 1.
        bool forceScreenContentTools = true;
        if (!*chooseScreenContentTools) {
            const auto force = reader.readFlag();
            if (!force) {
                return std::nullopt;
            }
            forceScreenContentTools = *force;
        }
        if (forceScreenContentTools) {
            const auto chooseIntegerMv = reader.readFlag();
            if (!chooseIntegerMv || (!*chooseIntegerMv && !reader.readBits(1))) {
                return std::nullopt;
            }
        }
        if (*enableOrderHint && !reader.readBits(3)) {
            return std::nullopt;
        }
    }
    // enable_superres, enable_cdef, enable_restoration.
    if (!reader.readBits(3)) {
        return std::nullopt;
    }

    // color_config(), AV1 spec 5.5.2.
    Av1SequenceHeaderInfo info{};
    info.seqProfile = static_cast<int>(*seqProfile);
    const auto highBitdepth = reader.readFlag();
    if (!highBitdepth) {
        return std::nullopt;
    }
    info.bitDepth = *highBitdepth ? 10 : 8;
    if (info.seqProfile == 2 && *highBitdepth) {
        const auto twelveBit = reader.readFlag();
        if (!twelveBit) {
            return std::nullopt;
        }
        info.bitDepth = *twelveBit ? 12 : 10;
    }
    if (info.seqProfile != 1) {
        const auto monochrome = reader.readFlag();
        if (!monochrome) {
            return std::nullopt;
        }
        info.monochrome = *monochrome;
    }
    const auto colorDescriptionPresent = reader.readFlag();
    if (!colorDescriptionPresent) {
        return std::nullopt;
    }
    if (*colorDescriptionPresent) {
        const auto primaries = reader.readBits(8);
        const auto transfer = reader.readBits(8);
        const auto matrix = reader.readBits(8);
        if (!primaries || !transfer || !matrix) {
            return std::nullopt;
        }
        info.colorPrimaries = static_cast<int>(*primaries);
        info.transferCharacteristics = static_cast<int>(*transfer);
        info.matrixCoefficients = static_cast<int>(*matrix);
    }
    if (info.monochrome) {
        info.subsamplingX = 1;
        info.subsamplingY = 1;
    } else if (info.colorPrimaries == ColorPrimariesBt709
               && info.transferCharacteristics == TransferSrgb
               && info.matrixCoefficients == MatrixIdentity) {
        info.subsamplingX = 0;
        info.subsamplingY = 0;
    } else if (info.seqProfile == 0) {
        info.subsamplingX = 1;
        info.subsamplingY = 1;
    } else if (info.seqProfile == 1) {
        info.subsamplingX = 0;
        info.subsamplingY = 0;
    } else {
        // color_range comes first in this branch.
        if (!reader.readBits(1)) {
            return std::nullopt;
        }
        info.subsamplingX = 1;
        info.subsamplingY = 0;
        if (info.bitDepth == 12) {
            const auto subsamplingX = reader.readBits(1);
            if (!subsamplingX) {
                return std::nullopt;
            }
            info.subsamplingX = static_cast<int>(*subsamplingX);
            if (info.subsamplingX != 0) {
                const auto subsamplingY = reader.readBits(1);
                if (!subsamplingY) {
                    return std::nullopt;
                }
                info.subsamplingY = static_cast<int>(*subsamplingY);
            }
        }
    }
    return info;
}

} // namespace

int Av1SequenceHeaderInfo::chromaFormatIdc() const
{
    if (monochrome) {
        return 0;
    }
    if (subsamplingX != 0 && subsamplingY != 0) {
        return 1;
    }
    return subsamplingX != 0 ? 2 : 3;
}

bool Av1SequenceHeaderInfo::isMain10_420() const
{
    return seqProfile == 0 && bitDepth == 10 && chromaFormatIdc() == 1;
}

std::optional<Av1SequenceHeaderInfo> parseAv1SequenceHeader(
    std::span<const std::uint8_t> temporalUnit)
{
    std::size_t offset = 0;
    while (offset < temporalUnit.size()) {
        const auto header = temporalUnit[offset];
        if ((header & 0x80U) != 0) {
            return std::nullopt;
        }
        const int type = (header >> 3) & 0x0F;
        const bool hasExtension = (header & 0x04U) != 0;
        const bool hasSizeField = (header & 0x02U) != 0;
        std::size_t position = offset + 1 + (hasExtension ? 1 : 0);
        if (position > temporalUnit.size()) {
            return std::nullopt;
        }
        std::size_t payloadSize = temporalUnit.size() - position;
        if (hasSizeField) {
            // leb128(), at most eight bytes.
            payloadSize = 0;
            bool complete = false;
            for (int index = 0; index < 8 && position < temporalUnit.size(); ++index) {
                const auto byte = temporalUnit[position++];
                payloadSize |= static_cast<std::size_t>(byte & 0x7FU) << (index * 7);
                if ((byte & 0x80U) == 0) {
                    complete = true;
                    break;
                }
            }
            if (!complete || payloadSize > temporalUnit.size() - position) {
                return std::nullopt;
            }
        }
        if (type == ObuSequenceHeader) {
            return parseSequenceHeaderPayload(temporalUnit.subspan(position, payloadSize));
        }
        offset = position + payloadSize;
    }
    return std::nullopt;
}

} // namespace gateway
