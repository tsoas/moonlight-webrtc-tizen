#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace moonlight {

class OpusFileReader {
public:
    using Packet = std::vector<std::uint8_t>;

    explicit OpusFileReader(const std::filesystem::path& path);

    const std::vector<Packet>& packets() const;
    const std::vector<std::uint32_t>& packetSampleCounts() const;
    std::uint32_t sampleRate() const;
    std::uint8_t channels() const;
    std::uint16_t preSkip() const;
    std::chrono::microseconds packetDuration(std::size_t index) const;
    std::chrono::microseconds totalPacketDuration() const;

private:
    std::vector<Packet> packets_;
    std::vector<std::uint32_t> packetSampleCounts_;
    std::uint32_t sampleRate_ = 0;
    std::uint8_t channels_ = 0;
    std::uint16_t preSkip_ = 0;
};

} // namespace moonlight
