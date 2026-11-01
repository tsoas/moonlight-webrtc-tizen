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

// Version 2 made TV authentication mandatory before any other request.
inline constexpr int Version = 2;

class ProtocolError : public std::runtime_error {
public:
    ProtocolError(std::string code, std::string message);

    const std::string& code() const noexcept;

private:
    std::string code_;
};

struct AuthenticateRequest {
    std::string clientId;
    std::string proof;
};

struct PairClientRequest {
    std::string pin;
    std::string clientName;
};

struct GetAppsRequest {};
struct GetAppArtworkRequest {
    std::string appId;
};
struct StopSessionRequest {};
struct StopHostSessionRequest {};

struct StartSessionRequest {
    std::string appId;
    StreamSettings settings;
};

struct SwitchSessionRequest {
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

using ClientPayload = std::variant<AuthenticateRequest,
                                   PairClientRequest,
                                   GetAppsRequest,
                                   GetAppArtworkRequest,
                                   StartSessionRequest,
                                   StopSessionRequest,
                                   StopHostSessionRequest,
                                   SwitchSessionRequest,
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
    std::optional<std::string> runningAppId;
    // The Gateway adapter the TV reaches it through, so the TV can wake this PC later.
    std::optional<std::string> macAddress;
};

struct Application {
    std::string id;
    std::string title;
    bool artworkAvailable = false;
    bool running = false;
};

ClientMessage parseClientMessage(std::string_view text);
// The Wake-on-LAN address is visible to the LAN through ARP anyway, so it is offered
// before authentication: it lets a TV's reachability probe learn how to wake the PC.
// Whether the Gateway can reach a paired Sunshine is offered for the same probe, so that a
// TV can tell a Gateway that is up but cannot stream from one that is fully available.
nlohmann::json makeAuthRequired(std::string_view nonce,
                                const std::optional<std::string>& macAddress = std::nullopt,
                                std::optional<bool> sunshineAvailable = std::nullopt);
// Pushed to every connection, authenticated or not, when Sunshine starts or stops, so a
// TV watching the Gateway from its home screen updates without reconnecting.
nlohmann::json makeSunshineAvailability(bool sunshineAvailable);
nlohmann::json makeAuthenticated();
nlohmann::json makePaired(std::string_view clientId, std::string_view clientSecret);
nlohmann::json makeGatewayStatus(const GatewayStatus& status);
// What Sunshine can encode beyond H.264 and HEVC. AV1 needs a GPU that can encode it, so it
// is listed, with or without HDR, only when Sunshine advertises it.
struct EncoderSupport {
    bool av1 = false;
    bool av1Hdr = false;
};

nlohmann::json makeCapabilities(const EncoderSupport& encoders = {});
nlohmann::json makeApps(const std::vector<Application>& applications);
nlohmann::json makeAppArtwork(std::string_view appId,
                              bool available,
                              std::string_view mimeType = {},
                              std::string_view base64Data = {});
nlohmann::json makeSessionStatus(std::string_view state,
                                 std::optional<std::uint64_t> sessionId = std::nullopt,
                                 std::optional<StreamSettings> settings = std::nullopt,
                                 std::optional<std::string> message = std::nullopt);
nlohmann::json makeHostSessionStatus(std::string_view state,
                                     std::optional<std::string> runningAppId = std::nullopt,
                                     std::optional<std::string> targetAppId = std::nullopt,
                                     std::optional<std::string> message = std::nullopt);
nlohmann::json makeError(std::string_view requestType,
                         std::string_view code,
                         std::string_view message);
nlohmann::json makeOffer(std::uint64_t sessionId, std::string_view sdp);
nlohmann::json makeCandidate(std::uint64_t sessionId,
                             std::string_view candidate,
                             std::string_view mid);

} // namespace gateway::protocol
