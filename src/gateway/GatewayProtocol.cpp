#include "gateway/GatewayProtocol.h"

#include <limits>

namespace gateway::protocol {
namespace {

using Json = nlohmann::json;

Json envelope(std::string_view type)
{
    return {{"version", Version}, {"type", type}};
}

void requireVersion(const Json& message)
{
    if (!message.contains("version") || !message.at("version").is_number_integer()
        || message.at("version").get<int>() != Version) {
        throw ProtocolError("unsupported-version", "Protocol version 1 is required");
    }
}

std::uint64_t sessionId(const Json& message)
{
    if (!message.contains("sessionId")
        || !message.at("sessionId").is_number_unsigned()) {
        throw ProtocolError("invalid-message", "A positive sessionId is required");
    }
    const auto value = message.at("sessionId").get<std::uint64_t>();
    if (value == 0) {
        throw ProtocolError("invalid-message", "A positive sessionId is required");
    }
    return value;
}

StreamSettings parseSettings(const Json& message)
{
    try {
        const auto& video = message.at("video");
        const auto& audio = message.at("audio");
        const auto codecName = video.at("codec").get<std::string>();
        const auto codec = parseVideoCodec(codecName);
        if (!codec) {
            throw ProtocolError("unsupported-settings", "Unsupported video codec");
        }

        StreamSettings settings;
        settings.width = video.at("width").get<int>();
        settings.height = video.at("height").get<int>();
        settings.fps = video.at("fps").get<int>();
        settings.codec = *codec;
        settings.bitrateKbps = video.at("bitrateKbps").get<int>();
        settings.hdr = video.at("hdr").get<bool>();
        settings.audioChannels = audio.at("channels").get<int>();
        if (const auto error = validateStreamSettings(settings)) {
            throw ProtocolError("unsupported-settings", *error);
        }
        return settings;
    } catch (const ProtocolError&) {
        throw;
    } catch (const std::exception&) {
        throw ProtocolError("invalid-message", "Invalid start-session settings");
    }
}

Json settingsJson(const StreamSettings& settings)
{
    return {
        {"video",
         {{"width", settings.width},
          {"height", settings.height},
          {"fps", settings.fps},
          {"codec", videoCodecName(settings.codec)},
          {"bitrateKbps", settings.bitrateKbps},
          {"hdr", settings.hdr}}},
        {"audio", {{"channels", settings.audioChannels}, {"sampleRate", 48000}}},
    };
}

Json videoModeJson(const VideoMode& mode)
{
    Json codecs = Json::array();
    for (const auto codec : supportedVideoCodecs()) {
        if (videoModeSupportsCodec(mode, codec)) {
            codecs.push_back(videoCodecName(codec));
        }
    }
    return {
        {"width", mode.width},
        {"height", mode.height},
        {"fps", mode.fps},
        {"codecs", std::move(codecs)},
        {"defaultCodec", videoCodecName(mode.defaultCodec)},
        {"defaultBitrateKbps", mode.defaultBitrateKbps},
        {"experimental", mode.experimental},
    };
}

} // namespace

ProtocolError::ProtocolError(std::string code, std::string message)
    : std::runtime_error(std::move(message))
    , code_(std::move(code))
{
}

const std::string& ProtocolError::code() const noexcept
{
    return code_;
}

ClientMessage parseClientMessage(std::string_view text)
{
    Json message;
    try {
        message = Json::parse(text);
    } catch (const std::exception&) {
        throw ProtocolError("invalid-json", "Message is not valid JSON");
    }

    try {
        requireVersion(message);
        const auto type = message.at("type").get<std::string>();
        if (type == "get-apps") {
            return {type, GetAppsRequest{}};
        }
        if (type == "stop-session") {
            return {type, StopSessionRequest{}};
        }
        if (type == "start-session") {
            if (!message.at("appId").is_string()) {
                throw ProtocolError("invalid-message", "appId must be a string");
            }
            const auto appId = message.at("appId").get<std::string>();
            if (appId.empty()) {
                throw ProtocolError("invalid-message", "appId must not be empty");
            }
            return {type, StartSessionRequest{appId, parseSettings(message)}};
        }
        if (type == "answer") {
            return {type,
                    AnswerMessage{sessionId(message),
                                  message.at("sdp").get<std::string>()}};
        }
        if (type == "candidate") {
            return {type,
                    CandidateMessage{sessionId(message),
                                     message.at("candidate").get<std::string>(),
                                     message.at("mid").get<std::string>()}};
        }
        throw ProtocolError("unsupported-message", "Unsupported message type: " + type);
    } catch (const ProtocolError&) {
        throw;
    } catch (const std::exception&) {
        throw ProtocolError("invalid-message", "Message fields are invalid");
    }
}

Json makeGatewayStatus(const GatewayStatus& status)
{
    auto message = envelope("gateway-status");
    message.update({{"gatewayName", status.gatewayName},
                    {"sunshineDetected", status.sunshineDetected},
                    {"sunshinePaired", status.sunshinePaired},
                    {"sessionActive", status.sessionActive}});
    return message;
}

Json makeCapabilities()
{
    auto message = envelope("capabilities");
    message["videoModes"] = Json::array();
    message["resolutions"] = Json::array();
    for (const auto& mode : SupportedVideoModes) {
        message["videoModes"].push_back(videoModeJson(mode));
        message["resolutions"].push_back({
            {"width", mode.width},
            {"height", mode.height},
            {"experimental", mode.experimental},
        });
    }
    message.update({
        {"frameRates", {60}},
        {"codecs", {"h264", "hevc"}},
        {"hdr", false},
        {"audio", "stereo"},
        {"audioSampleRate", 48000},
        {"bitratesKbps", SupportedBitratesKbps},
        {"defaults",
         {{"720p60", 12000},
          {"1080p60", 20000},
          {"1440p60", 30000},
          {"2160p60", 50000}}},
    });
    return message;
}

Json makeApps(const std::vector<Application>& applications)
{
    auto message = envelope("apps");
    message["apps"] = Json::array();
    for (const auto& application : applications) {
        message["apps"].push_back(
            {{"id", application.id}, {"title", application.title}});
    }
    return message;
}

Json makeSessionStatus(std::string_view state,
                       std::optional<std::uint64_t> sessionIdValue,
                       std::optional<StreamSettings> settings,
                       std::optional<std::string> detail)
{
    auto message = envelope("session-status");
    message["state"] = state;
    if (sessionIdValue) {
        message["sessionId"] = *sessionIdValue;
    }
    if (settings) {
        message.update(settingsJson(*settings));
    }
    if (detail) {
        message["message"] = *detail;
    }
    return message;
}

Json makeError(std::string_view requestType,
               std::string_view code,
               std::string_view detail)
{
    auto message = envelope("error");
    message.update({{"requestType", requestType}, {"code", code}, {"message", detail}});
    return message;
}

Json makeOffer(std::uint64_t sessionIdValue, std::string_view sdp)
{
    auto message = envelope("offer");
    message.update({{"sessionId", sessionIdValue}, {"sdp", sdp}});
    return message;
}

Json makeCandidate(std::uint64_t sessionIdValue,
                   std::string_view candidate,
                   std::string_view mid)
{
    auto message = envelope("candidate");
    message.update({{"sessionId", sessionIdValue}, {"candidate", candidate}, {"mid", mid}});
    return message;
}

} // namespace gateway::protocol
