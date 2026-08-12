#include "webrtc/SamsungSdp.h"

#include <charconv>
#include <stdexcept>

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

std::optional<std::string_view> formatParameter(std::string_view section,
                                                int payloadType,
                                                std::string_view name)
{
    const auto prefix = "a=fmtp:" + std::to_string(payloadType) + " ";
    const auto position = section.find(prefix);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    const auto end = section.find("\r\n", position);
    auto parameters = section.substr(position + prefix.size(), end - position - prefix.size());
    while (!parameters.empty()) {
        const auto separator = parameters.find(';');
        auto parameter = parameters.substr(0, separator);
        while (!parameter.empty() && parameter.front() == ' ') {
            parameter.remove_prefix(1);
        }
        while (!parameter.empty() && parameter.back() == ' ') {
            parameter.remove_suffix(1);
        }
        const auto equals = parameter.find('=');
        if (equals != std::string_view::npos && parameter.substr(0, equals) == name) {
            return parameter.substr(equals + 1);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        parameters.remove_prefix(separator + 1);
    }
    return std::nullopt;
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
        return section.find("H264/90000") == std::string_view::npos;
    }
    return section.find("H265/90000") == std::string_view::npos;
}

std::optional<std::string> hevcFormatParameters(const StreamSettings& settings)
{
    if (settings.codec != VideoCodec::HEVC || !settings.hdr) {
        return std::nullopt;
    }
    if (const auto error = validateStreamSettings(settings)) {
        throw std::invalid_argument(*error);
    }
    int levelId = 123;
    if (settings.width == 2560) {
        levelId = 150;
    } else if (settings.width == 3840) {
        levelId = 153;
    }
    return "profile-id=2;tier-flag=0;level-id=" + std::to_string(levelId);
}

bool hasExpectedHevcFormatParameters(std::string_view sdp,
                                     const StreamSettings& settings,
                                     int payloadType)
{
    if (!hevcFormatParameters(settings)) {
        return true;
    }
    const auto section = videoSection(sdp);
    if (section.empty()) {
        return false;
    }
    return formatParameter(section, payloadType, "profile-id") == "2"
        && formatParameter(section, payloadType, "tier-flag") == "0";
}

std::optional<int> hevcLevelId(std::string_view sdp, int payloadType)
{
    const auto value = formatParameter(videoSection(sdp), payloadType, "level-id");
    if (!value) {
        return std::nullopt;
    }
    int level = 0;
    const auto parsed = std::from_chars(
        value->data(), value->data() + value->size(), level);
    if (parsed.ec != std::errc{}
        || parsed.ptr != value->data() + value->size()) {
        return std::nullopt;
    }
    return level;
}

std::optional<int> videoExtensionId(std::string_view sdp, std::string_view uri)
{
    auto section = videoSection(sdp);
    while (!section.empty()) {
        const auto lineEnd = section.find("\r\n");
        const auto line = section.substr(0, lineEnd);
        constexpr std::string_view prefix = "a=extmap:";
        if (line.starts_with(prefix)) {
            auto value = line.substr(prefix.size());
            const auto separator = value.find(' ');
            if (separator != std::string_view::npos) {
                const auto idAndDirection = value.substr(0, separator);
                const auto slash = idAndDirection.find('/');
                const auto idText = idAndDirection.substr(0, slash);
                auto extension = value.substr(separator + 1);
                const auto attributes = extension.find(' ');
                extension = extension.substr(0, attributes);
                int id = 0;
                const auto parsed = std::from_chars(
                    idText.data(), idText.data() + idText.size(), id);
                if (parsed.ec == std::errc{}
                    && parsed.ptr == idText.data() + idText.size()
                    && extension == uri) {
                    return id;
                }
            }
        }
        if (lineEnd == std::string_view::npos) {
            break;
        }
        section.remove_prefix(lineEnd + 2);
    }
    return std::nullopt;
}

} // namespace gateway
