#include "OpusFileReader.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>

namespace moonlight {
namespace {

constexpr std::uint32_t OpusSampleRate = 48000;

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw std::runtime_error("Unable to open Ogg Opus sample: " + path.string());
    }

    const auto end = stream.tellg();
    if (end <= 0) {
        throw std::runtime_error("Ogg Opus sample is empty: " + path.string());
    }

    std::vector<std::uint8_t> data(static_cast<std::size_t>(end));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!stream) {
        throw std::runtime_error("Unable to read Ogg Opus sample: " + path.string());
    }

    return data;
}

std::uint16_t readLittleEndian16(std::span<const std::uint8_t> data, std::size_t offset)
{
    if (offset + 2 > data.size()) {
        throw std::runtime_error("Truncated Ogg Opus header");
    }

    return static_cast<std::uint16_t>(data[offset])
        | static_cast<std::uint16_t>(data[offset + 1] << 8);
}

std::uint32_t readLittleEndian32(std::span<const std::uint8_t> data, std::size_t offset)
{
    if (offset + 4 > data.size()) {
        throw std::runtime_error("Truncated Ogg page header");
    }

    return static_cast<std::uint32_t>(data[offset])
        | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

bool startsWith(std::span<const std::uint8_t> data, const std::string& prefix)
{
    return data.size() >= prefix.size()
        && std::equal(prefix.begin(), prefix.end(), data.begin());
}

std::uint32_t opusPacketSampleCount(std::span<const std::uint8_t> packet)
{
    if (packet.empty()) {
        throw std::runtime_error("Ogg stream contains an empty Opus packet");
    }

    const std::uint8_t toc = packet[0];
    const std::uint8_t configuration = toc >> 3;

    std::uint32_t samplesPerFrame;
    if (configuration >= 16) {
        samplesPerFrame = 120U << (configuration & 0x03U);
    } else if (configuration >= 12) {
        samplesPerFrame = 480U << (configuration & 0x01U);
    } else if ((configuration & 0x03U) == 0x03U) {
        samplesPerFrame = 2880;
    } else {
        samplesPerFrame = 480U << (configuration & 0x03U);
    }

    std::uint32_t frameCount;
    switch (toc & 0x03U) {
    case 0:
        frameCount = 1;
        break;
    case 1:
    case 2:
        frameCount = 2;
        break;
    case 3:
        if (packet.size() < 2) {
            throw std::runtime_error("Truncated multi-frame Opus packet");
        }
        frameCount = packet[1] & 0x3FU;
        if (frameCount == 0) {
            throw std::runtime_error("Opus packet declares zero frames");
        }
        break;
    default:
        throw std::runtime_error("Invalid Opus frame count code");
    }

    const std::uint32_t sampleCount = samplesPerFrame * frameCount;
    if (sampleCount > 5760) {
        throw std::runtime_error("Opus packet duration exceeds 120 ms");
    }

    return sampleCount;
}

std::vector<OpusFileReader::Packet> extractOggPackets(std::span<const std::uint8_t> data)
{
    std::vector<OpusFileReader::Packet> packets;
    OpusFileReader::Packet currentPacket;
    std::size_t offset = 0;
    std::uint32_t streamSerial = 0;
    std::uint32_t expectedSequence = 0;
    bool foundStream = false;

    while (offset < data.size()) {
        if (offset + 27 > data.size() || !startsWith(data.subspan(offset), "OggS")) {
            throw std::runtime_error("Invalid Ogg page capture pattern");
        }
        if (data[offset + 4] != 0) {
            throw std::runtime_error("Unsupported Ogg bitstream version");
        }

        const std::uint8_t headerType = data[offset + 5];
        const std::uint32_t pageSerial = readLittleEndian32(data, offset + 14);
        const std::uint32_t pageSequence = readLittleEndian32(data, offset + 18);
        if (!foundStream) {
            streamSerial = pageSerial;
            expectedSequence = pageSequence;
            foundStream = true;
        }
        if (pageSerial != streamSerial || pageSequence != expectedSequence++) {
            throw std::runtime_error("Unexpected Ogg logical stream or page sequence");
        }

        const bool continuedPacket = (headerType & 0x01U) != 0;
        if (continuedPacket != !currentPacket.empty()) {
            throw std::runtime_error("Invalid Ogg continued-packet flag");
        }

        const std::size_t segmentCount = data[offset + 26];
        const std::size_t segmentTableOffset = offset + 27;
        const std::size_t payloadOffset = segmentTableOffset + segmentCount;
        if (payloadOffset > data.size()) {
            throw std::runtime_error("Truncated Ogg segment table");
        }

        std::size_t pagePayloadSize = 0;
        for (std::size_t index = 0; index < segmentCount; ++index) {
            pagePayloadSize += data[segmentTableOffset + index];
        }
        if (payloadOffset + pagePayloadSize > data.size()) {
            throw std::runtime_error("Truncated Ogg page payload");
        }

        std::size_t packetOffset = payloadOffset;
        for (std::size_t index = 0; index < segmentCount; ++index) {
            const std::size_t segmentSize = data[segmentTableOffset + index];
            currentPacket.insert(currentPacket.end(),
                                 data.begin() + static_cast<std::ptrdiff_t>(packetOffset),
                                 data.begin()
                                     + static_cast<std::ptrdiff_t>(packetOffset + segmentSize));
            packetOffset += segmentSize;

            if (segmentSize < 255) {
                packets.push_back(std::move(currentPacket));
                currentPacket.clear();
            }
        }

        offset = payloadOffset + pagePayloadSize;
    }

    if (!currentPacket.empty()) {
        throw std::runtime_error("Ogg stream ends with an incomplete packet");
    }

    return packets;
}

} // namespace

OpusFileReader::OpusFileReader(const std::filesystem::path& path)
{
    const std::vector<std::uint8_t> data = readFile(path);
    std::vector<Packet> oggPackets = extractOggPackets(data);
    if (oggPackets.size() < 3 || !startsWith(oggPackets[0], "OpusHead")
        || !startsWith(oggPackets[1], "OpusTags")) {
        throw std::runtime_error("Ogg stream does not contain Opus headers and audio packets");
    }
    if (oggPackets[0].size() < 19 || oggPackets[0][8] > 15) {
        throw std::runtime_error("Unsupported Opus identification header");
    }

    channels_ = oggPackets[0][9];
    preSkip_ = readLittleEndian16(oggPackets[0], 10);
    sampleRate_ = readLittleEndian32(oggPackets[0], 12);
    if (channels_ == 0 || sampleRate_ != OpusSampleRate) {
        throw std::runtime_error("Opus sample must use at least one channel at 48000 Hz");
    }

    packets_.assign(std::make_move_iterator(oggPackets.begin() + 2),
                    std::make_move_iterator(oggPackets.end()));
    packetSampleCounts_.reserve(packets_.size());
    for (const Packet& packet : packets_) {
        packetSampleCounts_.push_back(opusPacketSampleCount(packet));
    }
}

const std::vector<OpusFileReader::Packet>& OpusFileReader::packets() const
{
    return packets_;
}

const std::vector<std::uint32_t>& OpusFileReader::packetSampleCounts() const
{
    return packetSampleCounts_;
}

std::uint32_t OpusFileReader::sampleRate() const
{
    return sampleRate_;
}

std::uint8_t OpusFileReader::channels() const
{
    return channels_;
}

std::uint16_t OpusFileReader::preSkip() const
{
    return preSkip_;
}

std::chrono::microseconds OpusFileReader::packetDuration(std::size_t index) const
{
    if (index >= packetSampleCounts_.size()) {
        throw std::out_of_range("Opus packet index is out of range");
    }

    return std::chrono::microseconds(
        static_cast<std::int64_t>(packetSampleCounts_[index]) * 1000000 / OpusSampleRate);
}

std::chrono::microseconds OpusFileReader::totalPacketDuration() const
{
    const std::uint64_t totalSamples = std::accumulate(packetSampleCounts_.begin(),
                                                       packetSampleCounts_.end(),
                                                       std::uint64_t{0});
    return std::chrono::microseconds(
        static_cast<std::int64_t>(totalSamples * 1000000 / OpusSampleRate));
}

} // namespace moonlight
