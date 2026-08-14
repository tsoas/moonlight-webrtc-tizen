#pragma once

#include <cstdint>
#include <string>

namespace gateway::moonlight {

struct SunshineServerInfo {
    std::string hostname;
    std::string appVersion;
    std::string gfeVersion;
    std::string uniqueId;
    std::uint16_t httpsPort = 0;
    int serverCodecModeSupport = 0;
    int pairStatus = 0;
    int currentGame = 0;
    std::string state;
};

struct SunshineApp {
    std::string title;
    // Sunshine application IDs are opaque GameStream identifiers. Keep their XML text
    // unchanged for appasset lookups and Gateway protocol messages.
    std::string id;
};

struct PairedSunshineHost {
    std::string serverUniqueId;
    std::string hostname;
    std::string lastAddress;
    std::uint16_t httpsPort = 0;
    std::string serverCertificatePem;
};

} // namespace gateway::moonlight
