#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace gateway {

// The parts of an AV1 sequence header (AV1 spec 5.5) that decide whether a stream is the
// Main 10-bit 4:2:0 that HDR needs. Colour fields are absent when the header omits them.
struct Av1SequenceHeaderInfo {
    int seqProfile;
    int bitDepth;
    bool monochrome;
    int subsamplingX;
    int subsamplingY;
    std::optional<int> colorPrimaries;
    std::optional<int> transferCharacteristics;
    std::optional<int> matrixCoefficients;

    // The HEVC chroma_format_idc for the same layout, so both codecs report alike.
    [[nodiscard]] int chromaFormatIdc() const;
    [[nodiscard]] bool isMain10_420() const;
};

// Finds the sequence header OBU in a temporal unit of size-delimited OBUs, as Sunshine
// sends them, and parses it. Returns nothing when the unit carries none (non-key frames).
std::optional<Av1SequenceHeaderInfo> parseAv1SequenceHeader(
    std::span<const std::uint8_t> temporalUnit);

} // namespace gateway
