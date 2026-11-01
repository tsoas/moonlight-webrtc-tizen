#pragma once

#include <cstdint>
#include <string>

namespace gateway::moonlight {

// Sunshine derives its whole port map from a configurable base port, so a host that
// moved off the default is only reachable once the Gateway probes the matching port.
inline constexpr std::uint16_t DefaultSunshineHttpPort = 47989;
inline constexpr std::uint16_t DefaultSunshineHttpsPortOffset = 5;

struct SunshineEndpoint {
    std::string host;
    std::uint16_t httpPort = DefaultSunshineHttpPort;
};

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
    std::uint16_t httpPort = DefaultSunshineHttpPort;
};

} // namespace gateway::moonlight
