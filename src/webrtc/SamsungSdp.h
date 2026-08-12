#pragma once

#include "session/StreamSettings.h"

#include <string>
#include <string_view>

namespace gateway {

std::string samsungGameModeImageAttribute(const StreamSettings& settings);
std::string samsungGameModeSdpLine(const StreamSettings& settings);
bool hasValidSamsungGameModeImageAttribute(std::string_view sdp,
                                           const StreamSettings& settings);

} // namespace gateway
