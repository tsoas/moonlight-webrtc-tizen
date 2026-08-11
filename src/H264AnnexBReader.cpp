#include "H264AnnexBReader.h"

#include <cstdint>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

namespace moonlight {
namespace {

struct StartCode {
    std::size_t offset;
    std::size_t size;
};

std::optional<StartCode> findStartCode(std::span<const std::uint8_t> data, std::size_t from)
{
    for (std::size_t index = from; index + 2 < data.size(); ++index) {
        if (data[index] != 0 || data[index + 1] != 0) {
            continue;
        }

        if (index + 3 < data.size() && data[index + 2] == 0 && data[index + 3] == 1) {
            return StartCode{index, 4};
        }

        if (data[index + 2] == 1) {
            return StartCode{index, 3};
        }
    }

    return std::nullopt;
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw std::runtime_error("Unable to open H.264 sample: " + path.string());
    }

    const auto end = stream.tellg();
    if (end <= 0) {
        throw std::runtime_error("H.264 sample is empty: " + path.string());
    }

    std::vector<std::uint8_t> data(static_cast<std::size_t>(end));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!stream) {
        throw std::runtime_error("Unable to read H.264 sample: " + path.string());
    }

    return data;
}

} // namespace

H264AnnexBReader::H264AnnexBReader(const std::filesystem::path& path)
{
    const std::vector<std::uint8_t> data = readFile(path);
    auto startCode = findStartCode(data, 0);

    if (!startCode || startCode->offset != 0) {
        throw std::runtime_error("H.264 sample does not start with an Annex-B start code");
    }

    AccessUnit currentAccessUnit;
    bool foundAud = false;

    while (startCode) {
        const std::size_t nalHeaderOffset = startCode->offset + startCode->size;
        const auto nextStartCode = findStartCode(data, nalHeaderOffset + 1);
        const std::size_t nalEnd = nextStartCode ? nextStartCode->offset : data.size();

        if (nalHeaderOffset >= nalEnd) {
            throw std::runtime_error("H.264 sample contains an empty NAL unit");
        }

        hasLongStartCodes_ |= startCode->size == 4;
        hasShortStartCodes_ |= startCode->size == 3;

        const auto nalType = data[nalHeaderOffset] & 0x1F;
        hasSps_ |= nalType == 7;
        hasPps_ |= nalType == 8;
        hasIdr_ |= nalType == 5;
        if (nalType == 5 && !firstIdrAccessUnitIndex_) {
            firstIdrAccessUnitIndex_ = accessUnits_.size();
        }

        if (nalType == 9) {
            if (foundAud && !currentAccessUnit.empty()) {
                accessUnits_.push_back(std::move(currentAccessUnit));
                currentAccessUnit.clear();
            }
            foundAud = true;
        }

        currentAccessUnit.insert(currentAccessUnit.end(),
                                 data.begin() + static_cast<std::ptrdiff_t>(startCode->offset),
                                 data.begin() + static_cast<std::ptrdiff_t>(nalEnd));
        startCode = nextStartCode;
    }

    if (!foundAud) {
        throw std::runtime_error("H.264 sample does not contain Access Unit Delimiters");
    }

    if (!currentAccessUnit.empty()) {
        accessUnits_.push_back(std::move(currentAccessUnit));
    }
}

const std::vector<H264AnnexBReader::AccessUnit>& H264AnnexBReader::accessUnits() const
{
    return accessUnits_;
}

std::optional<std::size_t> H264AnnexBReader::firstIdrAccessUnitIndex() const
{
    return firstIdrAccessUnitIndex_;
}

bool H264AnnexBReader::hasSps() const
{
    return hasSps_;
}

bool H264AnnexBReader::hasPps() const
{
    return hasPps_;
}

bool H264AnnexBReader::hasIdr() const
{
    return hasIdr_;
}

bool H264AnnexBReader::hasLongStartCodes() const
{
    return hasLongStartCodes_;
}

bool H264AnnexBReader::hasShortStartCodes() const
{
    return hasShortStartCodes_;
}

} // namespace moonlight
