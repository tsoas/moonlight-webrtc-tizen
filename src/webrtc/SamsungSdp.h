#pragma once

#include "session/StreamSettings.h"

#include <string>
#include <string_view>

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

} // namespace gateway
