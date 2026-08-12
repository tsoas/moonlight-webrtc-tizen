#include "session/StreamSettings.h"

#include <algorithm>
#include <cctype>

namespace gateway {

namespace {

inline constexpr std::array AllVideoCodecs{
    VideoCodec::H264,
    VideoCodec::HEVC,
};

} // namespace

const VideoMode* findVideoMode(int width, int height, int fps)
{
    const auto mode = std::ranges::find_if(
        SupportedVideoModes, [=](const VideoMode& candidate) {
            return candidate.width == width && candidate.height == height
                && candidate.fps == fps;
        });
    return mode == SupportedVideoModes.end() ? nullptr : &*mode;
}

bool videoModeSupportsCodec(const VideoMode& mode, VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::H264:
        return mode.supportsH264;
    case VideoCodec::HEVC:
        return mode.supportsHevc;
    }
    return false;
}

std::span<const VideoCodec> supportedVideoCodecs()
{
    return AllVideoCodecs;
}

StreamSettings defaultStreamSettings(int width,
                                     int height,
                                     std::optional<VideoCodec> codec)
{
    StreamSettings settings;
    settings.width = width;
    settings.height = height;
    if (const auto* mode = findVideoMode(width, height)) {
        settings.fps = mode->fps;
        settings.codec = codec.value_or(mode->defaultCodec);
        settings.bitrateKbps = mode->defaultBitrateKbps;
    } else if (codec) {
        settings.codec = *codec;
    }
    return settings;
}

std::optional<std::string> validateStreamSettings(const StreamSettings& settings)
{
    const auto* mode = findVideoMode(settings.width, settings.height, settings.fps);
    if (!mode) {
        if (settings.fps != 60) {
            return "Unsupported frame rate";
        }
        return "Unsupported resolution";
    }
    if (!videoModeSupportsCodec(*mode, settings.codec)) {
        return "Unsupported resolution and codec combination";
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
    case VideoCodec::HEVC:
        return "hevc";
    }
    return "unknown";
}

std::string_view videoCodecDisplayName(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::H264:
        return "H.264";
    case VideoCodec::HEVC:
        return "HEVC (H.265)";
    }
    return "Unknown";
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
    if (lowercase == "hevc") {
        return VideoCodec::HEVC;
    }
    return std::nullopt;
}

} // namespace gateway
