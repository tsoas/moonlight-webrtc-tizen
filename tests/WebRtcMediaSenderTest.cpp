#include "webrtc/WebRtcMediaSender.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <rtc/rtc.hpp>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main()
{
    try {
        auto h264Configuration = std::make_shared<rtc::RtpPacketizationConfig>(
            42, "test", 96, 90000);
        auto h264Packetizer = gateway::makeVideoRtpPacketizer(
            gateway::VideoCodec::H264, h264Configuration);
        require(std::dynamic_pointer_cast<rtc::H264RtpPacketizer>(h264Packetizer)
                    != nullptr,
                "H.264 did not select rtc::H264RtpPacketizer");
        require(std::dynamic_pointer_cast<rtc::H265RtpPacketizer>(h264Packetizer)
                    == nullptr,
                "H.264 selected the H.265 packetizer");

        auto hevcConfiguration = std::make_shared<rtc::RtpPacketizationConfig>(
            43, "test", 96, 90000);
        auto hevcPacketizer = gateway::makeVideoRtpPacketizer(
            gateway::VideoCodec::HEVC, hevcConfiguration);
        require(std::dynamic_pointer_cast<rtc::H265RtpPacketizer>(hevcPacketizer)
                    != nullptr,
                "HEVC did not select rtc::H265RtpPacketizer");
        require(std::dynamic_pointer_cast<rtc::H264RtpPacketizer>(hevcPacketizer)
                    == nullptr,
                "HEVC selected the H.264 packetizer");
        require(h264Configuration->clockRate == 90000
                    && hevcConfiguration->clockRate == 90000,
                "Video RTP clock is not 90000 Hz");

        rtc::Description::Video h264Description(
            "video", rtc::Description::Direction::SendOnly);
        h264Description.addH264Codec(96);
        const std::string h264Sdp = h264Description.generateSdp();
        require(h264Sdp.find("a=rtpmap:96 H264/90000") != std::string::npos,
                "libdatachannel H.264 SDP description is incorrect");

        rtc::Description::Video hevcDescription(
            "video", rtc::Description::Direction::SendOnly);
        hevcDescription.addH265Codec(96);
        const std::string hevcSdp = hevcDescription.generateSdp();
        require(hevcSdp.find("a=rtpmap:96 H265/90000") != std::string::npos
                    && hevcSdp.find("a=fmtp:96") == std::string::npos
                    && hevcSdp.find("H264/90000") == std::string::npos,
                "libdatachannel HEVC SDP description is incorrect");

        std::cout << "WebRTC media sender tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WebRTC media sender test failed: " << error.what() << '\n';
        return 1;
    }
}
