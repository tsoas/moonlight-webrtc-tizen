#include "moonlight/MoonlightVideoProfile.h"

#include <Limelight.h>

#include <stdexcept>

namespace gateway::moonlight {

MoonlightVideoProfile moonlightVideoProfile(const StreamSettings& settings)
{
    if (const auto error = validateStreamSettings(settings)) {
        throw std::invalid_argument(*error);
    }

    if (settings.codec == VideoCodec::H264) {
        return {
            VIDEO_FORMAT_H264,
            COLORSPACE_REC_709,
            COLOR_RANGE_LIMITED,
            8,
            false,
            "H.264 High 8-bit",
        };
    }
    if (settings.codec == VideoCodec::AV1 && settings.hdr) {
        return {
            VIDEO_FORMAT_AV1_MAIN10,
            COLORSPACE_REC_2020,
            COLOR_RANGE_LIMITED,
            10,
            true,
            "AV1 Main 10-bit",
        };
    }
    if (settings.codec == VideoCodec::AV1) {
        return {
            VIDEO_FORMAT_AV1_MAIN8,
            COLORSPACE_REC_709,
            COLOR_RANGE_LIMITED,
            8,
            false,
            "AV1 Main 8-bit",
        };
    }
    if (settings.hdr) {
        return {
            VIDEO_FORMAT_H265_MAIN10,
            COLORSPACE_REC_2020,
            COLOR_RANGE_LIMITED,
            10,
            true,
            "HEVC Main10",
        };
    }
    return {
        VIDEO_FORMAT_H265,
        COLORSPACE_REC_709,
        COLOR_RANGE_LIMITED,
        8,
        false,
        "HEVC Main 8-bit",
    };
}

} // namespace gateway::moonlight
