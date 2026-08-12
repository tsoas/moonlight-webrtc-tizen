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
        require(!gateway::validateStreamSettings(settings720)
                    && settings720.bitrateKbps == 12000,
                "Valid 720p60 settings were rejected");
        require(!gateway::validateStreamSettings(settings1080)
                    && settings1080.bitrateKbps == 20000,
                "Valid 1080p60 settings were rejected");

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
                    startMessage(1280, 720, 60, "hevc", 12000, false, 2));
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
        require(imageAttribute720
                    == "imageattr:96 send [x=[1280:1280],y=[720:720],fps=[60:60]]",
                "720p imageattr generation failed");
        require(imageAttribute1080
                    == "imageattr:96 send [x=[1920:1920],y=[1080:1080],fps=[60:60]]",
                "1080p imageattr generation failed");

        const std::string sdp720 = "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na="
            + imageAttribute720 + "\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n";
        const std::string sdp1080 = "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na="
            + imageAttribute1080 + "\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n";
        require(gateway::hasValidSamsungGameModeImageAttribute(sdp720, settings720),
                "720p imageattr validation failed");
        require(gateway::hasValidSamsungGameModeImageAttribute(sdp1080, settings1080),
                "1080p imageattr validation failed");
        require(!gateway::hasValidSamsungGameModeImageAttribute(sdp720, settings1080),
                "Mismatched imageattr was accepted");

        const auto capabilities = gateway::protocol::makeCapabilities();
        require(capabilities.at("resolutions").size() == 2
                    && capabilities.at("frameRates") == nlohmann::json::array({60})
                    && capabilities.at("codecs") == nlohmann::json::array({"h264"})
                    && !capabilities.at("hdr").get<bool>()
                    && capabilities.at("audio") == "stereo",
                "Advertised capabilities are incorrect");

        std::cout << "Gateway protocol tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Gateway protocol test failed: " << error.what() << '\n';
        return 1;
    }
}
