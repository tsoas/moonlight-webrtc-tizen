#pragma once

#include <cstdint>
#include <span>

namespace gateway {

class MediaSender {
public:
    virtual ~MediaSender() = default;

    virtual void sendH264AccessUnit(std::span<const std::uint8_t> accessUnit,
                                    std::uint32_t rtpTimestamp) = 0;
    virtual void sendOpusPacket(std::span<const std::uint8_t> packet,
                                std::uint32_t rtpTimestamp) = 0;
};

} // namespace gateway
