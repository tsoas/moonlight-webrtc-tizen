#include "webrtc/SamsungSdp.h"

namespace gateway {

std::string samsungGameModeImageAttribute(const StreamSettings& settings)
{
    return "imageattr:96 send [x=[" + std::to_string(settings.width) + ":"
        + std::to_string(settings.width) + "],y=[" + std::to_string(settings.height)
        + ":" + std::to_string(settings.height) + "],fps=["
        + std::to_string(settings.fps) + ":" + std::to_string(settings.fps) + "]]";
}

std::string samsungGameModeSdpLine(const StreamSettings& settings)
{
    return "a=" + samsungGameModeImageAttribute(settings);
}

bool hasValidSamsungGameModeImageAttribute(std::string_view sdp,
                                           const StreamSettings& settings)
{
    for (std::size_t index = 0; index < sdp.size(); ++index) {
        if ((sdp[index] == '\n' && (index == 0 || sdp[index - 1] != '\r'))
            || (sdp[index] == '\r'
                && (index + 1 == sdp.size() || sdp[index + 1] != '\n'))) {
            return false;
        }
    }

    const auto expected = samsungGameModeSdpLine(settings);
    const auto attributePosition = sdp.find(expected);
    if (attributePosition == std::string_view::npos
        || sdp.find(expected, attributePosition + expected.size()) != std::string_view::npos
        || sdp.find("a=imageattr:") != attributePosition
        || sdp.find("a=imageattr:", attributePosition + 1) != std::string_view::npos) {
        return false;
    }

    const bool completeLine =
        (attributePosition == 0 || sdp.substr(attributePosition - 2, 2) == "\r\n")
        && sdp.substr(attributePosition + expected.size(), 2) == "\r\n";
    const auto videoPosition = sdp.find("m=video ");
    const auto nextMediaPosition = videoPosition == std::string_view::npos
        ? std::string_view::npos
        : sdp.find("\r\nm=", videoPosition + 1);

    return completeLine && videoPosition != std::string_view::npos
        && attributePosition > videoPosition
        && (nextMediaPosition == std::string_view::npos
            || attributePosition < nextMediaPosition);
}

} // namespace gateway
