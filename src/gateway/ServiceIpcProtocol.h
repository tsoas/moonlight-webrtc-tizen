#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gateway::serviceipc {

inline constexpr std::uint32_t ProtocolVersion = 1;

struct StatusSnapshot {
    bool serviceRunning = true;
    std::optional<bool> sunshineConnected;
    std::optional<bool> sunshinePaired;
    std::optional<std::string> sunshineHost;
    std::optional<std::string> runningApplicationId;
    std::optional<std::string> runningApplicationName;
    std::optional<bool> sessionActive;
    std::optional<std::uint32_t> connectedTvClients;
};

enum class RequestType {
    Status,
};

RequestType parseRequest(std::string_view payload);
std::string makeStatusResponse(const StatusSnapshot& snapshot);
std::string makeErrorResponse(std::string_view code, std::string_view message);

} // namespace gateway::serviceipc
