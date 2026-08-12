#include "session/StreamSettings.h"

#include <algorithm>
#include <cctype>

namespace gateway {

StreamSettings defaultStreamSettings(int width, int height)
{
    StreamSettings settings;
    settings.width = width;
    settings.height = height;
    settings.bitrateKbps = width == 1920 && height == 1080 ? 20000 : 12000;
    return settings;
}

std::optional<std::string> validateStreamSettings(const StreamSettings& settings)
{
    const bool supportedResolution = std::ranges::any_of(
        SupportedResolutions, [&settings](const auto& resolution) {
            return resolution.first == settings.width
                && resolution.second == settings.height;
        });
    if (!supportedResolution) {
        return "Unsupported resolution";
    }
    if (settings.fps != 60) {
        return "Unsupported frame rate";
    }
    if (settings.codec != VideoCodec::H264) {
        return "Unsupported video codec";
    }
    if (settings.hdr) {
        return "HDR streaming is not supported";
    }
    if (settings.audioChannels != 2) {
        return "Only stereo audio is supported";
    }
    if (std::ranges::find(SupportedBitratesKbps, settings.bitrateKbps)
        == SupportedBitratesKbps.end()) {
        return "Unsupported bitrate";
    }
    return std::nullopt;
}

std::string_view videoCodecName(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::H264:
        return "h264";
    }
    return "unknown";
}

std::optional<VideoCodec> parseVideoCodec(std::string_view name)
{
    std::string lowercase(name);
    std::ranges::transform(lowercase, lowercase.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (lowercase == "h264") {
        return VideoCodec::H264;
    }
    return std::nullopt;
}

} // namespace gateway
