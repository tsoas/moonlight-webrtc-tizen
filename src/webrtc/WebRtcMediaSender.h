#pragma once

#include "media/MediaSender.h"

#include <memory>

namespace rtc {
class Track;
}

namespace gateway {

class WebRtcMediaSender final : public MediaSender {
public:
    WebRtcMediaSender(std::shared_ptr<rtc::Track> videoTrack,
                      std::shared_ptr<rtc::Track> audioTrack);

    void sendH264AccessUnit(std::span<const std::uint8_t> accessUnit,
                            std::uint32_t rtpTimestamp) override;
    void sendOpusPacket(std::span<const std::uint8_t> packet,
                        std::uint32_t rtpTimestamp) override;

private:
    std::shared_ptr<rtc::Track> videoTrack_;
    std::shared_ptr<rtc::Track> audioTrack_;
};

} // namespace gateway
