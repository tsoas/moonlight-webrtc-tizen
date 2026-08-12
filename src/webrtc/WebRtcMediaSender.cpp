#include "webrtc/WebRtcMediaSender.h"

#include <stdexcept>
#include <utility>

#include <rtc/rtc.hpp>

namespace gateway {

WebRtcMediaSender::WebRtcMediaSender(std::shared_ptr<rtc::Track> videoTrack,
                                     std::shared_ptr<rtc::Track> audioTrack,
                                     VideoCodec videoCodec)
    : videoTrack_(std::move(videoTrack))
    , audioTrack_(std::move(audioTrack))
    , videoCodec_(videoCodec)
{
    if (!videoTrack_ || !audioTrack_) {
        throw std::invalid_argument("WebRTC media tracks must not be null");
    }
}

void WebRtcMediaSender::sendVideoAccessUnit(VideoCodec codec,
                                            std::span<const std::uint8_t> accessUnit,
                                            std::uint32_t rtpTimestamp)
{
    if (codec != videoCodec_) {
        throw std::invalid_argument("Video access unit codec does not match the WebRTC track");
    }
    videoTrack_->sendFrame(reinterpret_cast<const rtc::byte*>(accessUnit.data()),
                           accessUnit.size(),
                           rtc::FrameInfo(rtpTimestamp));
}

std::shared_ptr<rtc::RtpPacketizer> makeVideoRtpPacketizer(
    VideoCodec codec,
    const std::shared_ptr<rtc::RtpPacketizationConfig>& rtpConfiguration)
{
    if (!rtpConfiguration) {
        throw std::invalid_argument("Video RTP configuration must not be null");
    }
    switch (codec) {
    case VideoCodec::H264:
        return std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence, rtpConfiguration);
    case VideoCodec::HEVC:
        return std::make_shared<rtc::H265RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence, rtpConfiguration);
    }
    throw std::invalid_argument("Unsupported WebRTC video codec");
}

void WebRtcMediaSender::sendOpusPacket(std::span<const std::uint8_t> packet,
                                       std::uint32_t rtpTimestamp)
{
    audioTrack_->sendFrame(reinterpret_cast<const rtc::byte*>(packet.data()),
                           packet.size(),
                           rtc::FrameInfo(rtpTimestamp));
}

} // namespace gateway
