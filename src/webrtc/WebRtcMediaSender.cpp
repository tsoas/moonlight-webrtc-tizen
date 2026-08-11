#include "webrtc/WebRtcMediaSender.h"

#include <stdexcept>
#include <utility>

#include <rtc/rtc.hpp>

namespace gateway {

WebRtcMediaSender::WebRtcMediaSender(std::shared_ptr<rtc::Track> videoTrack,
                                     std::shared_ptr<rtc::Track> audioTrack)
    : videoTrack_(std::move(videoTrack))
    , audioTrack_(std::move(audioTrack))
{
    if (!videoTrack_ || !audioTrack_) {
        throw std::invalid_argument("WebRTC media tracks must not be null");
    }
}

void WebRtcMediaSender::sendH264AccessUnit(std::span<const std::uint8_t> accessUnit,
                                           std::uint32_t rtpTimestamp)
{
    videoTrack_->sendFrame(reinterpret_cast<const rtc::byte*>(accessUnit.data()),
                           accessUnit.size(),
                           rtc::FrameInfo(rtpTimestamp));
}

void WebRtcMediaSender::sendOpusPacket(std::span<const std::uint8_t> packet,
                                       std::uint32_t rtpTimestamp)
{
    audioTrack_->sendFrame(reinterpret_cast<const rtc::byte*>(packet.data()),
                           packet.size(),
                           rtc::FrameInfo(rtpTimestamp));
}

} // namespace gateway
