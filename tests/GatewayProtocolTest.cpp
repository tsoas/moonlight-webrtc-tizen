#include "gateway/GatewayProtocol.h"
#include "session/StreamSettings.h"
#include "webrtc/SamsungSdp.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<typename Callback>
void requireProtocolError(Callback&& callback, const std::string& expectedCode)
{
    try {
        callback();
    } catch (const gateway::protocol::ProtocolError& error) {
        require(error.code() == expectedCode,
                "Unexpected protocol error code: " + error.code());
        return;
    }
    throw std::runtime_error("Expected protocol error was not thrown");
}

std::string startMessage(int width,
                         int height,
                         int fps,
                         const std::string& codec,
                         int bitrate,
                         bool hdr,
                         int channels)
{
    return nlohmann::json({
        {"version", 1},
        {"type", "start-session"},
        {"appId", "7"},
        {"video",
         {{"width", width},
          {"height", height},
          {"fps", fps},
          {"codec", codec},
          {"bitrateKbps", bitrate},
          {"hdr", hdr}}},
        {"audio", {{"channels", channels}}},
    }).dump();
}

} // namespace

int main()
{
    try {
        const auto settings720 = gateway::defaultStreamSettings();
        const auto settings1080 = gateway::defaultStreamSettings(1920, 1080);
        const auto settingsHevc1080 = gateway::defaultStreamSettings(
            1920, 1080, gateway::VideoCodec::HEVC);
        const auto settings1440 = gateway::defaultStreamSettings(2560, 1440);
        const auto settings4k = gateway::defaultStreamSettings(3840, 2160);
        require(!gateway::validateStreamSettings(settings720)
                    && settings720.bitrateKbps == 12000,
                "Valid 720p60 settings were rejected");
        require(!gateway::validateStreamSettings(settings1080)
                    && settings1080.bitrateKbps == 20000,
                "Valid 1080p60 settings were rejected");
        require(!gateway::validateStreamSettings(settingsHevc1080),
                "Valid HEVC 1080p60 settings were rejected");
        require(!gateway::validateStreamSettings(settings1440)
                    && settings1440.codec == gateway::VideoCodec::HEVC
                    && settings1440.bitrateKbps == 30000,
                "Valid/default HEVC 1440p60 settings are incorrect");
        require(!gateway::validateStreamSettings(settings4k)
                    && settings4k.codec == gateway::VideoCodec::HEVC
                    && settings4k.bitrateKbps == 50000,
                "Valid/default HEVC 4K60 settings are incorrect");

        auto invalid = settings720;
        invalid.width = 1366;
        require(gateway::validateStreamSettings(invalid).has_value(),
                "Invalid resolution was accepted");
        invalid = settings720;
        invalid.fps = 30;
        require(gateway::validateStreamSettings(invalid).has_value(),
                "Invalid frame rate was accepted");
        invalid = settings720;
        invalid.hdr = true;
        require(gateway::validateStreamSettings(invalid).has_value(),
                "HDR streaming was accepted");
        invalid = settings720;
        invalid.bitrateKbps = 999999;
        require(gateway::validateStreamSettings(invalid).has_value(),
                "Invalid bitrate was accepted");
        invalid = settings4k;
        invalid.codec = gateway::VideoCodec::H264;
        require(gateway::validateStreamSettings(invalid).has_value(),
                "Unsupported H.264 4K combination was accepted");
        invalid = settings720;
        invalid.audioChannels = 6;
        require(gateway::validateStreamSettings(invalid).has_value(),
                "Unsupported audio channel count was accepted");

        const auto parsed720 = gateway::protocol::parseClientMessage(
            startMessage(1280, 720, 60, "h264", 12000, false, 2));
        const auto& start720 =
            std::get<gateway::protocol::StartSessionRequest>(parsed720.payload);
        require(start720.appId == "7" && start720.settings == settings720,
                "720p start-session parsing failed");

        const auto parsed1080 = gateway::protocol::parseClientMessage(
            startMessage(1920, 1080, 60, "H264", 20000, false, 2));
        const auto& start1080 =
            std::get<gateway::protocol::StartSessionRequest>(parsed1080.payload);
        require(start1080.settings == settings1080,
                "1080p start-session parsing failed");

        const auto parsedHevc1080 = gateway::protocol::parseClientMessage(
            startMessage(1920, 1080, 60, "hevc", 20000, false, 2));
        require(std::get<gateway::protocol::StartSessionRequest>(
                    parsedHevc1080.payload).settings
                    == settingsHevc1080,
                "HEVC 1080p start-session parsing failed");
        const auto parsedHevc1440 = gateway::protocol::parseClientMessage(
            startMessage(2560, 1440, 60, "HEVC", 30000, false, 2));
        require(std::get<gateway::protocol::StartSessionRequest>(
                    parsedHevc1440.payload).settings
                    == settings1440,
                "HEVC 1440p start-session parsing failed");
        const auto parsedHevc4k = gateway::protocol::parseClientMessage(
            startMessage(3840, 2160, 60, "hevc", 50000, false, 2));
        require(std::get<gateway::protocol::StartSessionRequest>(
                    parsedHevc4k.payload).settings
                    == settings4k,
                "HEVC 4K start-session parsing failed");

        requireProtocolError(
            [] {
                gateway::protocol::parseClientMessage(
                    startMessage(1366, 768, 60, "h264", 12000, false, 2));
            },
            "unsupported-settings");
        requireProtocolError(
            [] {
                gateway::protocol::parseClientMessage(
                    startMessage(1280, 720, 30, "h264", 12000, false, 2));
            },
            "unsupported-settings");
        requireProtocolError(
            [] {
                gateway::protocol::parseClientMessage(
                    startMessage(3840, 2160, 60, "h264", 50000, false, 2));
            },
            "unsupported-settings");
        requireProtocolError(
            [] {
                gateway::protocol::parseClientMessage(
                    startMessage(1920, 1080, 60, "vp9", 20000, false, 2));
            },
            "unsupported-settings");
        requireProtocolError(
            [] {
                gateway::protocol::parseClientMessage(
                    startMessage(1920, 1080, 60, "hevc", 20000, false, 6));
            },
            "unsupported-settings");
        requireProtocolError(
            [] {
                gateway::protocol::parseClientMessage(
                    startMessage(1280, 720, 60, "h264", 12000, true, 2));
            },
            "unsupported-settings");
        requireProtocolError(
            [] {
                gateway::protocol::parseClientMessage(
                    startMessage(1280, 720, 60, "h264", 12345, false, 2));
            },
            "unsupported-settings");

        const auto getApps = gateway::protocol::parseClientMessage(
            R"({"version":1,"type":"get-apps"})");
        require(std::holds_alternative<gateway::protocol::GetAppsRequest>(getApps.payload),
                "get-apps parsing failed");
        const auto stop = gateway::protocol::parseClientMessage(
            R"({"version":1,"type":"stop-session"})");
        require(std::holds_alternative<gateway::protocol::StopSessionRequest>(stop.payload),
                "stop-session parsing failed");
        requireProtocolError(
            [] {
                gateway::protocol::parseClientMessage(
                    R"({"version":2,"type":"stop-session"})");
            },
            "unsupported-version");

        const auto imageAttribute720 = gateway::samsungGameModeImageAttribute(settings720);
        const auto imageAttribute1080 = gateway::samsungGameModeImageAttribute(settings1080);
        const auto imageAttribute1440 = gateway::samsungGameModeImageAttribute(settings1440);
        const auto imageAttribute4k = gateway::samsungGameModeImageAttribute(settings4k);
        require(imageAttribute720
                    == "imageattr:96 send [x=[1280:1280],y=[720:720],fps=[60:60]]",
                "720p imageattr generation failed");
        require(imageAttribute1080
                    == "imageattr:96 send [x=[1920:1920],y=[1080:1080],fps=[60:60]]",
                "1080p imageattr generation failed");
        require(imageAttribute1440
                    == "imageattr:96 send [x=[2560:2560],y=[1440:1440],fps=[60:60]]",
                "1440p imageattr generation failed");
        require(imageAttribute4k
                    == "imageattr:96 send [x=[3840:3840],y=[2160:2160],fps=[60:60]]",
                "4K imageattr generation failed");

        const std::string sdp720 = "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na="
            + imageAttribute720 + "\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n";
        const std::string sdp1080 = "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na="
            + imageAttribute1080 + "\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n";
        const std::string sdp1440 = "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na="
            + imageAttribute1440 + "\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n";
        const std::string sdp4k = "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na="
            + imageAttribute4k + "\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n";
        require(gateway::hasValidSamsungGameModeImageAttribute(sdp720, settings720),
                "720p imageattr validation failed");
        require(gateway::hasValidSamsungGameModeImageAttribute(sdp1080, settings1080),
                "1080p imageattr validation failed");
        require(gateway::hasValidSamsungGameModeImageAttribute(sdp1440, settings1440),
                "1440p imageattr validation failed");
        require(gateway::hasValidSamsungGameModeImageAttribute(sdp4k, settings4k),
                "4K imageattr validation failed");
        require(!gateway::hasValidSamsungGameModeImageAttribute(sdp720, settings1080),
                "Mismatched imageattr was accepted");

        const std::string hevcSdp =
            "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
            "a=rtpmap:96 H265/90000\r\n"
            "a=" + imageAttribute4k + "\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n";
        const std::string h264Sdp =
            "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
            "a=rtpmap:96 H264/90000\r\n"
            "a=fmtp:96 packetization-mode=1\r\n"
            "a=" + imageAttribute1080 + "\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n";
        require(gateway::hasExpectedVideoCodec(
                    hevcSdp, gateway::VideoCodec::HEVC),
                "HEVC SDP codec validation failed");
        require(gateway::hasExpectedVideoCodec(
                    h264Sdp, gateway::VideoCodec::H264),
                "H.264 SDP codec validation failed");
        require(!gateway::hasExpectedVideoCodec(
                    h264Sdp, gateway::VideoCodec::HEVC),
                "H.264 SDP was accepted for HEVC");

        const auto capabilities = gateway::protocol::makeCapabilities();
        require(capabilities.at("resolutions").size() == 4
                    && capabilities.at("videoModes").size() == 4
                    && capabilities.at("frameRates") == nlohmann::json::array({60})
                    && capabilities.at("codecs")
                        == nlohmann::json::array({"h264", "hevc"})
                    && !capabilities.at("hdr").get<bool>()
                    && capabilities.at("audio") == "stereo",
                "Advertised capabilities are incorrect");
        require(capabilities.at("videoModes").at(2).at("experimental") == true
                    && capabilities.at("videoModes").at(2).at("defaultCodec")
                        == "hevc"
                    && capabilities.at("videoModes").at(2).at("defaultBitrateKbps")
                        == 30000
                    && capabilities.at("videoModes").at(3).at("defaultBitrateKbps")
                        == 50000,
                "High-resolution capability defaults are incorrect");

        std::cout << "Gateway protocol tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Gateway protocol test failed: " << error.what() << '\n';
        return 1;
    }
}
