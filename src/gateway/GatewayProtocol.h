#pragma once

#include "session/StreamSettings.h"

#include <optional>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace gateway::protocol {

inline constexpr int Version = 1;

class ProtocolError : public std::runtime_error {
public:
    ProtocolError(std::string code, std::string message);

    const std::string& code() const noexcept;

private:
    std::string code_;
};

struct GetAppsRequest {};
struct StopSessionRequest {};

struct StartSessionRequest {
    std::string appId;
    StreamSettings settings;
};

struct AnswerMessage {
    std::uint64_t sessionId = 0;
    std::string sdp;
};

struct CandidateMessage {
    std::uint64_t sessionId = 0;
    std::string candidate;
    std::string mid;
};

using ClientPayload = std::variant<GetAppsRequest,
                                   StartSessionRequest,
                                   StopSessionRequest,
                                   AnswerMessage,
                                   CandidateMessage>;

struct ClientMessage {
    std::string type;
    ClientPayload payload;
};

struct GatewayStatus {
    std::string gatewayName = "Moonlight WebRTC Gateway";
    bool sunshineDetected = false;
    bool sunshinePaired = false;
    bool sessionActive = false;
};

struct Application {
    std::string id;
    std::string title;
};

ClientMessage parseClientMessage(std::string_view text);
nlohmann::json makeGatewayStatus(const GatewayStatus& status);
nlohmann::json makeCapabilities();
nlohmann::json makeApps(const std::vector<Application>& applications);
nlohmann::json makeSessionStatus(std::string_view state,
                                 std::optional<std::uint64_t> sessionId = std::nullopt,
                                 std::optional<StreamSettings> settings = std::nullopt,
                                 std::optional<std::string> message = std::nullopt);
nlohmann::json makeError(std::string_view requestType,
                         std::string_view code,
                         std::string_view message);
nlohmann::json makeOffer(std::uint64_t sessionId, std::string_view sdp);
nlohmann::json makeCandidate(std::uint64_t sessionId,
                             std::string_view candidate,
                             std::string_view mid);

} // namespace gateway::protocol
