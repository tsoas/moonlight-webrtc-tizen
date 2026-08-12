#include "webrtc/SamsungSdp.h"

namespace gateway {

namespace {

std::string_view videoSection(std::string_view sdp)
{
    const auto begin = sdp.find("m=video ");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = sdp.find("\r\nm=", begin + 1);
    return sdp.substr(begin, end == std::string_view::npos ? end : end - begin);
}

} // namespace

std::string samsungGameModeImageAttribute(const StreamSettings& settings,
                                          int payloadType)
{
    return "imageattr:" + std::to_string(payloadType) + " send [x=["
        + std::to_string(settings.width) + ":"
        + std::to_string(settings.width) + "],y=[" + std::to_string(settings.height)
        + ":" + std::to_string(settings.height) + "],fps=["
        + std::to_string(settings.fps) + ":" + std::to_string(settings.fps) + "]]";
}

std::string samsungGameModeSdpLine(const StreamSettings& settings, int payloadType)
{
    return "a=" + samsungGameModeImageAttribute(settings, payloadType);
}

bool hasValidSamsungGameModeImageAttribute(std::string_view sdp,
                                           const StreamSettings& settings,
                                           int payloadType)
{
    for (std::size_t index = 0; index < sdp.size(); ++index) {
        if ((sdp[index] == '\n' && (index == 0 || sdp[index - 1] != '\r'))
            || (sdp[index] == '\r'
                && (index + 1 == sdp.size() || sdp[index + 1] != '\n'))) {
            return false;
        }
    }

    const auto expected = samsungGameModeSdpLine(settings, payloadType);
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

bool hasExpectedVideoCodec(std::string_view sdp,
                           VideoCodec codec,
                           int payloadType)
{
    const auto section = videoSection(sdp);
    if (section.empty()) {
        return false;
    }
    const auto payload = std::to_string(payloadType);
    const auto expected = "a=rtpmap:" + payload + " "
        + (codec == VideoCodec::HEVC ? "H265/90000" : "H264/90000");
    if (section.find(expected) == std::string_view::npos) {
        return false;
    }
    if (codec == VideoCodec::HEVC) {
        return section.find("H264/90000") == std::string_view::npos
            && section.find("a=fmtp:" + payload + " ") == std::string_view::npos;
    }
    return section.find("H265/90000") == std::string_view::npos;
}

} // namespace gateway
