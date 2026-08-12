#pragma once

#include "session/StreamSettings.h"

#include <string>
#include <string_view>
#include <optional>

namespace gateway {

std::string samsungGameModeImageAttribute(const StreamSettings& settings,
                                          int payloadType = 96);
std::string samsungGameModeSdpLine(const StreamSettings& settings,
                                   int payloadType = 96);
bool hasValidSamsungGameModeImageAttribute(std::string_view sdp,
                                           const StreamSettings& settings,
                                           int payloadType = 96);
bool hasExpectedVideoCodec(std::string_view sdp,
                           VideoCodec codec,
                           int payloadType = 96);
std::optional<std::string> hevcFormatParameters(const StreamSettings& settings);
bool hasExpectedHevcFormatParameters(std::string_view sdp,
                                     const StreamSettings& settings,
                                     int payloadType = 96);
std::optional<int> hevcLevelId(std::string_view sdp, int payloadType = 96);
std::optional<int> videoExtensionId(std::string_view sdp, std::string_view uri);

} // namespace gateway
