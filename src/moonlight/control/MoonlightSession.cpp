#include "moonlight/control/MoonlightSession.h"

#include "moonlight/control/SunshineHttpClient.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <openssl/rand.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace gateway::moonlight {
namespace {

constexpr auto DetectionTimeout = std::chrono::seconds(2);
constexpr auto RequestTimeout = std::chrono::seconds(5);

std::vector<std::string> localIpv4Addresses()
{
    std::vector<std::string> addresses;
#ifdef _WIN32
    WSADATA winsockData{};
    if (WSAStartup(MAKEWORD(2, 2), &winsockData) != 0) {
        return addresses;
    }

    std::array<char, 256> hostname{};
    if (gethostname(hostname.data(), static_cast<int>(hostname.size())) == 0) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* results = nullptr;
        if (getaddrinfo(hostname.data(), nullptr, &hints, &results) == 0) {
            for (auto* result = results; result; result = result->ai_next) {
                const auto* address = reinterpret_cast<const sockaddr_in*>(result->ai_addr);
                std::array<char, INET_ADDRSTRLEN> text{};
                if (inet_ntop(AF_INET, &address->sin_addr, text.data(), text.size())
                    && std::string_view(text.data()) != "127.0.0.1") {
                    addresses.emplace_back(text.data());
                }
            }
            freeaddrinfo(results);
        }
    }
    WSACleanup();
#endif
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    return addresses;
}

std::unique_ptr<SunshineHttpClient> configuredClient(const MoonlightIdentity& identity,
                                                      const DetectedSunshine& detected)
{
    auto client = std::make_unique<SunshineHttpClient>(identity, detected.address);
    client->setHttpsPort(detected.serverInfo.httpsPort);
    if (detected.pairedHost) {
        client->setPinnedServerCertificate(detected.pairedHost->serverCertificatePem);
    }
    return client;
}

std::string stageName(int stage)
{
    const char* name = LiGetStageName(stage);
    return name ? name : std::to_string(stage);
}

} // namespace

std::atomic<MoonlightSession*> MoonlightSession::activeSession_ = nullptr;

ServerInformationStorage::ServerInformationStorage(const SunshineServerInfo& serverInfo,
                                                   std::string address,
                                                   std::string rtspSessionUrl)
    : address_(std::move(address))
    , appVersion_(serverInfo.appVersion)
    , gfeVersion_(serverInfo.gfeVersion)
    , rtspSessionUrl_(std::move(rtspSessionUrl))
{
    LiInitializeServerInformation(&value_);
    value_.serverCodecModeSupport = serverInfo.serverCodecModeSupport;
    refreshPointers();
}

SERVER_INFORMATION* ServerInformationStorage::get()
{
    refreshPointers();
    return &value_;
}

void ServerInformationStorage::refreshPointers()
{
    value_.address = address_.c_str();
    value_.serverInfoAppVersion = appVersion_.c_str();
    value_.serverInfoGfeVersion = gfeVersion_.empty() ? nullptr : gfeVersion_.c_str();
    value_.rtspSessionUrl = rtspSessionUrl_.empty() ? nullptr : rtspSessionUrl_.c_str();
}

MoonlightSession::MoonlightSession(MediaSender& sender,
                                   MoonlightIdentity& identity,
                                   MoonlightSessionOptions options,
                                   Logger logger,
                                   TerminationHandler terminationHandler)
    : sender_(sender)
    , identity_(identity)
    , options_(std::move(options))
    , logger_(std::move(logger))
    , terminationHandler_(std::move(terminationHandler))
{
    configureConnectionCallbacks();
}

MoonlightSession::~MoonlightSession()
{
    stop();
    auto* expected = this;
    activeSession_.compare_exchange_strong(expected, nullptr);
}

DetectedSunshine MoonlightSession::detectSunshine(
    MoonlightIdentity& identity,
    const std::optional<std::string>& requestedHost,
    const Logger& logger)
{
    std::vector<std::string> candidates;
    if (requestedHost) {
        candidates.push_back(*requestedHost);
    } else {
        candidates.push_back("127.0.0.1");
        const auto lanAddresses = localIpv4Addresses();
        candidates.insert(candidates.end(), lanAddresses.begin(), lanAddresses.end());
    }

    std::string lastError;
    for (const auto& candidate : candidates) {
        try {
            SunshineHttpClient client(identity, candidate);
            auto serverInfo = client.getServerInfo(false, DetectionTimeout);
            if (!serverInfo.state.starts_with("SUNSHINE_")) {
                throw std::runtime_error("server did not identify itself as Sunshine");
            }

            auto pairedHost = identity.pairedHost(serverInfo.uniqueId);
            if (pairedHost) {
                client.setHttpsPort(serverInfo.httpsPort);
                client.setPinnedServerCertificate(pairedHost->serverCertificatePem);
                try {
                    auto authenticatedInfo = client.getServerInfo(true, RequestTimeout);
                    if (authenticatedInfo.uniqueId != serverInfo.uniqueId) {
                        throw std::runtime_error("Sunshine identity changed after TLS connection");
                    }
                    serverInfo = std::move(authenticatedInfo);
                } catch (const SunshineHttpError& error) {
                    if (error.statusCode() != 401) {
                        throw;
                    }
                    serverInfo.pairStatus = 0;
                }
            }

            if (logger) {
                logger("Sunshine detected: " + candidate);
                logger("Sunshine server name: " + serverInfo.hostname);
                logger("Sunshine app version: " + serverInfo.appVersion);
                logger("Sunshine state: " + serverInfo.state);
            }
            return {candidate, std::move(serverInfo), std::move(pairedHost)};
        } catch (const std::exception& error) {
            lastError = candidate + ": " + error.what();
            if (requestedHost
                || std::string_view(error.what()).find("certificate mismatch")
                    != std::string_view::npos) {
                throw;
            }
        }
    }

    throw std::runtime_error("Sunshine was not detected locally (" + lastError + ")");
}

STREAM_CONFIGURATION MoonlightSession::createStreamConfiguration()
{
    STREAM_CONFIGURATION configuration;
    LiInitializeStreamConfiguration(&configuration);
    configuration.width = 1280;
    configuration.height = 720;
    configuration.fps = 60;
    configuration.bitrate = 12000;
    configuration.packetSize = 1392;
    configuration.streamingRemotely = STREAM_CFG_LOCAL;
    configuration.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    configuration.supportedVideoFormats = VIDEO_FORMAT_H264;
    configuration.clientRefreshRateX100 = 6000;
    configuration.colorSpace = COLORSPACE_REC_709;
    configuration.colorRange = COLOR_RANGE_LIMITED;
    configuration.encryptionFlags = ENCFLG_ALL;

    if (RAND_bytes(
            reinterpret_cast<unsigned char*>(configuration.remoteInputAesKey),
            sizeof(configuration.remoteInputAesKey))
            != 1
        || RAND_bytes(
               reinterpret_cast<unsigned char*>(configuration.remoteInputAesIv), 4)
            != 1) {
        throw std::runtime_error("OpenSSL failed to generate Moonlight session keys");
    }
    return configuration;
}

void MoonlightSession::start()
{
    State expected = State::Idle;
    if (!state_.compare_exchange_strong(expected, State::Starting)) {
        throw std::runtime_error("Moonlight session was already started");
    }

    try {
        const auto detected = detectSunshine(identity_, options_.host, logger_);
        if (!detected.pairedHost || detected.serverInfo.pairStatus != 1) {
            throw std::runtime_error(
                "Moonlight client is not paired; run with --source=moonlight --pair first");
        }

        auto httpClient = configuredClient(identity_, detected);
        auto applications = httpClient->getAppList();
        for (const auto& application : applications) {
            log("Sunshine application: " + application.title + " (ID "
                + std::to_string(application.id) + ")");
        }

        const SunshineApp* application = SunshineHttpClient::findApplication(
            applications, options_.application);
        if (!application) {
            throw std::runtime_error(
                "Requested Sunshine application was not found: " + options_.application);
        }

        std::string verb = "launch";
        if (detected.serverInfo.currentGame != 0) {
            if (detected.serverInfo.currentGame != application->id) {
                throw std::runtime_error(
                    "Sunshine is already streaming a different application");
            }
            verb = "resume";
        }

        streamConfiguration_ = createStreamConfiguration();
        const std::string rtspSessionUrl = httpClient->launchOrResume(
            verb, application->id, streamConfiguration_);
        log(verb == "resume" ? "Sunshine Desktop resumed" : "Sunshine Desktop launched");
        log("RTSP session URL obtained");

        serverInformation_ = std::make_unique<ServerInformationStorage>(
            detected.serverInfo, detected.address, rtspSessionUrl);
        mediaBridge_ = std::make_unique<MoonlightMediaBridge>(sender_, logger_);

        activeSession_.store(this, std::memory_order_release);
        const int result = LiStartConnection(serverInformation_->get(),
                                             &streamConfiguration_,
                                             &connectionCallbacks_,
                                             mediaBridge_->videoCallbacks(),
                                             mediaBridge_->audioCallbacks(),
                                             mediaBridge_.get(),
                                             0,
                                             mediaBridge_.get(),
                                             0);
        if (result != 0) {
            state_.store(State::Stopped, std::memory_order_release);
            throw std::runtime_error("LiStartConnection failed with error "
                                     + std::to_string(result));
        }
        State starting = State::Starting;
        if (!state_.compare_exchange_strong(
                starting, State::Connected, std::memory_order_acq_rel)) {
            LiStopConnection();
            throw std::runtime_error("Moonlight connection startup was interrupted");
        }
        log("LiStartConnection succeeded");
    } catch (...) {
        State starting = State::Starting;
        state_.compare_exchange_strong(starting, State::Stopped);
        throw;
    }
}

void MoonlightSession::stop()
{
    const State previous = state_.exchange(State::Stopped, std::memory_order_acq_rel);
    if (previous == State::Starting) {
        LiInterruptConnection();
    } else if (previous == State::Connected) {
        LiStopConnection();
    }
}

void MoonlightSession::onWebRtcKeyframeRequest()
{
    if (mediaBridge_) {
        mediaBridge_->onWebRtcKeyframeRequest();
    }
}

void MoonlightSession::stageStartingCallback(int stage)
{
    if (auto* session = activeSession_.load(std::memory_order_acquire)) {
        session->log("Moonlight stage starting: " + stageName(stage));
    }
}

void MoonlightSession::stageCompleteCallback(int stage)
{
    if (auto* session = activeSession_.load(std::memory_order_acquire)) {
        session->log("Moonlight stage complete: " + stageName(stage));
    }
}

void MoonlightSession::stageFailedCallback(int stage, int errorCode)
{
    if (auto* session = activeSession_.load(std::memory_order_acquire)) {
        session->log("Moonlight stage failed: " + stageName(stage) + ", error="
                     + std::to_string(errorCode));
    }
}

void MoonlightSession::connectionStartedCallback()
{
    if (auto* session = activeSession_.load(std::memory_order_acquire)) {
        session->log("Moonlight connection started");
    }
}

void MoonlightSession::connectionTerminatedCallback(int errorCode)
{
    if (auto* session = activeSession_.load(std::memory_order_acquire)) {
        session->log("Moonlight connection terminated: " + std::to_string(errorCode));
        if (session->terminationHandler_) {
            session->terminationHandler_();
        }
    }
}

void MoonlightSession::logMessageCallback(const char* format, ...)
{
    if (auto* session = activeSession_.load(std::memory_order_acquire)) {
        std::array<char, 2048> message{};
        va_list arguments;
        va_start(arguments, format);
        std::vsnprintf(message.data(), message.size(), format, arguments);
        va_end(arguments);
        session->log(message.data());
    }
}

void MoonlightSession::connectionStatusCallback(int status)
{
    if (auto* session = activeSession_.load(std::memory_order_acquire)) {
        session->log("Moonlight connection status: " + std::to_string(status));
    }
}

void MoonlightSession::configureConnectionCallbacks()
{
    LiInitializeConnectionCallbacks(&connectionCallbacks_);
    connectionCallbacks_.stageStarting = &MoonlightSession::stageStartingCallback;
    connectionCallbacks_.stageComplete = &MoonlightSession::stageCompleteCallback;
    connectionCallbacks_.stageFailed = &MoonlightSession::stageFailedCallback;
    connectionCallbacks_.connectionStarted = &MoonlightSession::connectionStartedCallback;
    connectionCallbacks_.connectionTerminated =
        &MoonlightSession::connectionTerminatedCallback;
    connectionCallbacks_.logMessage = &MoonlightSession::logMessageCallback;
    connectionCallbacks_.connectionStatusUpdate = &MoonlightSession::connectionStatusCallback;
}

void MoonlightSession::log(const std::string& message) const
{
    if (logger_) {
        logger_(message);
    }
}

} // namespace gateway::moonlight
