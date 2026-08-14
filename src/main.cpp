#include "H264AnnexBReader.h"
#include "OpusFileReader.h"
#include "gateway/ApplicationArtworkCache.h"
#include "gateway/GatewayLifecycle.h"
#include "gateway/GatewayProtocol.h"
#include "gateway/ManagementIpcClient.h"
#include "gateway/ServiceIpcServer.h"
#include "gateway/WindowsServiceHost.h"
#include "media/MediaSender.h"
#include "moonlight/MoonlightMediaBridge.h"
#include "moonlight/control/MoonlightIdentity.h"
#include "moonlight/control/MoonlightPairing.h"
#include "moonlight/control/MoonlightSession.h"
#include "moonlight/control/SunshineHttpClient.h"
#include "moonlight/input/MoonlightInputBridge.h"
#include "session/StreamSettings.h"
#include "webrtc/SamsungSdp.h"
#include "webrtc/WebRtcMediaSender.h"

#include <atomic>
#include <chrono>
#include <charconv>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <rtc/rtc.hpp>

namespace {

using Json = nlohmann::json;

constexpr std::uint8_t VideoPayloadType = 96;
constexpr std::uint8_t AudioPayloadType = 111;
constexpr rtc::SSRC VideoSsrc = 42;
constexpr rtc::SSRC AudioSsrc = 43;
constexpr double VideoFrameRate = 60.0;
constexpr std::uint32_t VideoRtpClockRate = 90000;
constexpr std::uint32_t AudioRtpClockRate = 48000;
constexpr auto VideoSamplePath = "samples/test-720p60.h264";
constexpr auto AudioSamplePath = "samples/test-tone-48k-stereo.opus";
constexpr auto RtpCname = "moonlight-webrtc";
constexpr auto MediaStreamId = "stream1";
// Artwork is capped at 8 MiB before Base64 and JSON framing expand it for WebSocket transport.
constexpr std::size_t MaxGatewayWebSocketMessageSize = 12 * 1024 * 1024;
volatile std::sig_atomic_t ConsoleShutdownRequested = 0;

std::string base64Encode(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty()) {
        return {};
    }

    std::string encoded(4 * ((bytes.size() + 2) / 3), '\0');
    const int size = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()), bytes.data(), static_cast<int>(bytes.size()));
    if (size <= 0) {
        throw std::runtime_error("Unable to encode Sunshine application artwork");
    }
    encoded.resize(static_cast<std::size_t>(size));
    return encoded;
}

int runningSunshineApplicationId(const gateway::moonlight::SunshineServerInfo& serverInfo)
{
    return gateway::moonlight::SunshineHttpClient::runningApplicationId(serverInfo);
}

void requestShutdown(int)
{
    ConsoleShutdownRequested = 1;
}

std::string_view videoMediaSection(std::string_view sdp)
{
    const auto videoPosition = sdp.find("m=video ");
    if (videoPosition == std::string_view::npos) {
        return {};
    }

    const auto nextMediaPosition = sdp.find("\r\nm=", videoPosition + 1);
    return sdp.substr(videoPosition,
                      nextMediaPosition == std::string_view::npos
                          ? std::string_view::npos
                          : nextMediaPosition - videoPosition);
}

bool hasDataChannelApplicationSection(std::string_view sdp)
{
    const auto application = sdp.find("m=application ");
    return application != std::string_view::npos
        && sdp.find("webrtc-datachannel", application) != std::string_view::npos
        && sdp.find("a=sctp-port:", application) != std::string_view::npos;
}

enum class MediaSourceMode {
    Test,
    Moonlight,
};

enum class ProgramHostMode {
    Console,
    Service,
};

struct ProgramOptions {
    MediaSourceMode sourceMode = MediaSourceMode::Test;
    std::optional<std::string> host;
    std::string application = "Desktop";
    bool pair = false;
    ProgramHostMode hostMode = ProgramHostMode::Console;
    std::optional<std::filesystem::path> dataDirectory;
    bool dataDirectoryExplicit = false;
    std::optional<std::filesystem::path> migrationSourceDirectory;
};

ProgramOptions parseProgramOptions(int argc, char** argv)
{
    ProgramOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--source=test") {
            options.sourceMode = MediaSourceMode::Test;
        } else if (argument == "--source=moonlight") {
            options.sourceMode = MediaSourceMode::Moonlight;
        } else if (argument.starts_with("--host=") && argument.size() > 7) {
            options.host = std::string(argument.substr(7));
        } else if (argument.starts_with("--app=") && argument.size() > 6) {
            options.application = std::string(argument.substr(6));
        } else if (argument == "--pair") {
            options.pair = true;
        } else if (argument == "--console") {
            options.hostMode = ProgramHostMode::Console;
        } else if (argument == "--service") {
            options.hostMode = ProgramHostMode::Service;
        } else if (argument.starts_with("--data-dir=") && argument.size() > 11) {
            options.dataDirectory = std::filesystem::path(argument.substr(11));
            options.dataDirectoryExplicit = true;
        } else if (argument.starts_with("--migrate-data-from=") && argument.size() > 20) {
            options.migrationSourceDirectory = std::filesystem::path(argument.substr(20));
        } else {
            throw std::invalid_argument(
                "Usage: moonlight_webrtc [--source=test|--source=moonlight] "
                "[--host=<host>] [--app=<name>] [--pair] [--console|--service] "
                "[--data-dir=<path>] [--migrate-data-from=<path>]");
        }
    }

    if (options.sourceMode != MediaSourceMode::Moonlight
        && (options.host || options.application != "Desktop" || options.pair || options.dataDirectory
            || options.migrationSourceDirectory)) {
        throw std::invalid_argument(
            "--host, --app, --pair, and --data-dir require --source=moonlight");
    }
    if (options.pair && options.hostMode == ProgramHostMode::Service) {
        throw std::invalid_argument("--pair cannot run as a Windows service");
    }
    if (options.migrationSourceDirectory) {
        if (options.sourceMode != MediaSourceMode::Moonlight || !options.dataDirectory) {
            throw std::invalid_argument(
                "--migrate-data-from requires --source=moonlight and an explicit --data-dir");
        }
        if (options.hostMode == ProgramHostMode::Service || options.pair || options.host) {
            throw std::invalid_argument(
                "--migrate-data-from cannot be combined with --service, --pair, or --host");
        }
    }
    if (options.hostMode == ProgramHostMode::Service) {
        if (options.sourceMode != MediaSourceMode::Moonlight) {
            throw std::invalid_argument("--service requires --source=moonlight");
        }
        options.dataDirectory = gateway::moonlight::MoonlightIdentity::resolveStorageDirectory(
            options.dataDirectory, gateway::moonlight::MoonlightDataDirectoryMode::Service);
    }
    return options;
}

struct PendingCandidate {
    std::string candidate;
    std::string mid;
};

struct Session {
    ~Session()
    {
        if (inputBridge) {
            inputBridge->shutdown();
        }
        stopStreaming();
    }

    void requestStreamingStop()
    {
        {
            const std::lock_guard lock(streamMutex);
            stopRequested = true;
        }
        streamCondition.notify_all();
    }

    void stopStreaming()
    {
        requestStreamingStop();
        if (streamingThread.joinable()) {
            streamingThread.join();
        }
    }

    std::shared_ptr<rtc::WebSocket> socket;
    std::shared_ptr<rtc::PeerConnection> peerConnection;
    std::shared_ptr<rtc::Track> videoTrack;
    std::shared_ptr<rtc::Track> audioTrack;
    std::shared_ptr<rtc::DataChannel> controlChannel;
    std::shared_ptr<rtc::DataChannel> gamepadChannel;
    std::shared_ptr<gateway::WebRtcMediaSender> mediaSender;
    std::shared_ptr<rtc::RtpPacketizationConfig> videoRtpConfiguration;
    std::shared_ptr<gateway::moonlight::MoonlightSession> moonlightSession;
    std::shared_ptr<gateway::moonlight::MoonlightInputBridge> inputBridge;
    std::uint32_t videoRtpStartTimestamp = 0;
    std::uint32_t audioRtpStartTimestamp = 0;
    std::uint64_t sessionId = 0;
    gateway::StreamSettings settings = gateway::defaultStreamSettings();
    std::optional<int> applicationId;
    bool streamingActive = false;

    std::mutex sendMutex;
    std::mutex candidateMutex;
    bool hasRemoteDescription = false;
    std::vector<PendingCandidate> pendingCandidates;

    std::mutex streamMutex;
    std::condition_variable streamCondition;
    bool peerConnected = false;
    bool videoTrackOpen = false;
    bool audioTrackOpen = false;
    bool stopRequested = false;
    bool hostOperationActive = false;
    std::atomic<bool> videoKeyframeRequested = false;
    std::atomic<std::uint64_t> keyframeRequestCount = 0;
    std::atomic<bool> malformedGamepadMessageReported = false;
    std::thread streamingThread;
};

using Logger = std::function<void(const std::string&)>;

class SignalingServer {
public:
    explicit SignalingServer(const ProgramOptions& options, Logger logger)
        : sourceMode_(options.sourceMode)
        , moonlightOptions_{options.host, options.application}
        , explicitMoonlightHost_(options.host.has_value())
        , identity_(sourceMode_ == MediaSourceMode::Moonlight
                        ? std::make_unique<gateway::moonlight::MoonlightIdentity>(
                            gateway::moonlight::MoonlightIdentity::resolveStorageDirectory(
                                options.dataDirectory,
                                options.hostMode == ProgramHostMode::Service
                                    ? gateway::moonlight::MoonlightDataDirectoryMode::Service
                                    : gateway::moonlight::MoonlightDataDirectoryMode::Console))
                        : nullptr)
        , logger_(std::move(logger))
        , server_(makeServerConfiguration())
    {
        moonlightOptions_.allowUnavailableHostDisplayForHdr =
            options.hostMode == ProgramHostMode::Service;
        log("Moonlight WebRTC Gateway");
        server_.onClient([this](std::shared_ptr<rtc::WebSocket> socket) {
            acceptClient(std::move(socket));
        });

        if (sourceMode_ == MediaSourceMode::Test) {
            videoSource_ = std::make_unique<moonlight::H264AnnexBReader>(VideoSamplePath);
            audioSource_ = std::make_unique<moonlight::OpusFileReader>(AudioSamplePath);

            std::ostringstream message;
            message << "Loaded H.264 sample: " << videoSource_->accessUnits().size()
                    << " access units";
            log(message.str());
            message.str("");
            message.clear();
            message << "Loaded Opus sample: " << audioSource_->packets().size()
                    << " encoded packets";
            log(message.str());
        } else {
            if (!moonlightOptions_.host) {
                moonlightOptions_.host = identity_->configuredSunshineHost();
            }
            log("Moonlight source selected");
            log("Moonlight identity path: " + identity_->storageDirectory().string());
        }
        log("WebSocket signaling server listening on 0.0.0.0:8000");
    }

    ~SignalingServer()
    {
        if (pairingThread_.joinable()) {
            pairingThread_.join();
        }
        std::shared_ptr<Session> session;
        {
            const std::lock_guard lock(sessionMutex_);
            session = std::exchange(activeSession_, nullptr);
        }
        if (session) {
            stopActiveSession(session, false);
        }
    }

    void wait(gateway::GatewayShutdownSignal& shutdown)
    {
        while (!shutdown.requested()) {
            if (ConsoleShutdownRequested) {
                shutdown.request();
                break;
            }
            shutdown.waitFor(std::chrono::milliseconds(200));
        }
    }

private:
    static rtc::WebSocketServer::Configuration makeServerConfiguration()
    {
        rtc::WebSocketServer::Configuration configuration;
        configuration.port = 8000;
        configuration.enableTls = false;
        configuration.bindAddress = "0.0.0.0";
        configuration.maxMessageSize = MaxGatewayWebSocketMessageSize;
        return configuration;
    }

    void log(const std::string& message)
    {
        const std::lock_guard lock(logMutex_);
        logger_(message);
    }

    void acceptClient(std::shared_ptr<rtc::WebSocket> socket)
    {
        auto session = std::make_shared<Session>();
        session->socket = std::move(socket);

        std::shared_ptr<Session> previousSession;
        {
            const std::lock_guard lock(sessionMutex_);
            previousSession = std::exchange(activeSession_, session);
        }

        if (previousSession && previousSession->socket) {
            previousSession->socket->close();
        }

        const std::weak_ptr<Session> weakSession = session;

        session->socket->onOpen([this, weakSession] {
            if (const auto currentSession = weakSession.lock()) {
                log("WebSocket client connected");
                sendInitialState(currentSession);
            }
        });

        session->socket->onClosed([this, weakSession] {
            if (const auto currentSession = weakSession.lock()) {
                log("WebSocket client disconnected");
                closeSession(currentSession);
            }
        });

        session->socket->onError([this](const std::string& error) {
            log("WebSocket error: " + error);
        });

        session->socket->onMessage(
            [this, weakSession](rtc::message_variant data) {
                if (!std::holds_alternative<std::string>(data)) {
                    return;
                }

                if (const auto currentSession = weakSession.lock()) {
                    handleMessage(currentSession, std::get<std::string>(std::move(data)));
                }
            });
    }

    bool isCurrentSession(const std::shared_ptr<Session>& session,
                          std::uint64_t sessionId)
    {
        const std::lock_guard lock(session->streamMutex);
        return session->sessionId == sessionId && sessionId != 0;
    }

    void createPeerConnection(const std::shared_ptr<Session>& session,
                              std::uint64_t sessionId,
                              const gateway::StreamSettings& settings)
    {
        if (session->peerConnection) {
            throw std::runtime_error("A WebRTC session is already active");
        }

        rtc::Configuration configuration;
        configuration.disableAutoNegotiation = true;
        auto peerConnection = std::make_shared<rtc::PeerConnection>(configuration);
        session->peerConnection = peerConnection;

        const std::weak_ptr<Session> weakSession = session;

        peerConnection->onStateChange(
            [this, weakSession, sessionId](rtc::PeerConnection::State state) {
                std::ostringstream message;
                message << "PeerConnection state: " << state;
                log(message.str());

                if (const auto currentSession = weakSession.lock();
                    currentSession && isCurrentSession(currentSession, sessionId)) {
                    {
                        const std::lock_guard lock(currentSession->streamMutex);
                        currentSession->peerConnected =
                            state == rtc::PeerConnection::State::Connected;

                        if (state == rtc::PeerConnection::State::Disconnected
                            || state == rtc::PeerConnection::State::Failed
                            || state == rtc::PeerConnection::State::Closed) {
                            if (currentSession->inputBridge) {
                                currentSession->inputBridge->onTransportClosed();
                            }
                            currentSession->stopRequested = true;
                        }
                    }
                    currentSession->streamCondition.notify_all();
                }
            });

        peerConnection->onIceStateChange([this](rtc::PeerConnection::IceState state) {
            std::ostringstream message;
            message << "ICE state: " << state;
            log(message.str());
        });

        peerConnection->onLocalDescription(
            [this, weakSession, sessionId, settings](rtc::Description description) {
                if (description.type() != rtc::Description::Type::Offer) {
                    return;
                }

                if (const auto currentSession = weakSession.lock();
                    currentSession && isCurrentSession(currentSession, sessionId)) {
                    const std::string sdp = std::string(description);
                    if (!gateway::hasValidSamsungGameModeImageAttribute(sdp, settings)) {
                        log("Samsung Game Mode SDP imageattr validation failed");
                        sendSessionStatus(currentSession,
                                          "error",
                                          "Invalid Samsung Game Mode SDP imageattr");
                        currentSession->requestStreamingStop();
                        return;
                    }
                    if (!gateway::hasExpectedVideoCodec(
                            sdp, settings.codec, VideoPayloadType)) {
                        log("WebRTC SDP video codec validation failed");
                        sendSessionStatus(currentSession,
                                          "error",
                                          "WebRTC SDP does not contain the requested video codec");
                        currentSession->requestStreamingStop();
                        return;
                    }
                    if (!gateway::hasExpectedHevcFormatParameters(
                            sdp, settings, VideoPayloadType)) {
                        log("WebRTC SDP HEVC Main10 fmtp validation failed");
                        sendSessionStatus(currentSession,
                                          "error",
                                          "WebRTC SDP does not contain the requested HEVC Main10 profile");
                        currentSession->requestStreamingStop();
                        return;
                    }
                    if (!hasDataChannelApplicationSection(sdp)) {
                        log("WebRTC data-channel SDP validation failed");
                        sendSessionStatus(currentSession,
                                          "error",
                                          "Missing WebRTC data-channel SDP section");
                        currentSession->requestStreamingStop();
                        return;
                    }

                    log("Samsung Game Mode SDP imageattr enabled");
                    log("WebRTC data-channel application section enabled");
                    log("Outgoing SDP m=video section:\n"
                        + std::string(videoMediaSection(sdp)));
                    sendJson(currentSession,
                             gateway::protocol::makeOffer(sessionId, sdp));
                    log("Sent SDP offer");
                }
            });

        peerConnection->onLocalCandidate(
            [this, weakSession, sessionId](rtc::Candidate candidate) {
            if (const auto currentSession = weakSession.lock();
                currentSession && isCurrentSession(currentSession, sessionId)) {
                const std::string candidateText = candidate.candidate();
                sendJson(currentSession,
                         gateway::protocol::makeCandidate(
                             sessionId, candidateText, candidate.mid()));
                log("Sent ICE candidate: " + candidateText);
            }
        });

        auto inputBridge = std::make_shared<gateway::moonlight::MoonlightInputBridge>(
            [this](const std::string& message) { log(message); },
            [this, weakSession, sessionId](const std::string& message) {
                const auto currentSession = weakSession.lock();
                if (!currentSession || !isCurrentSession(currentSession, sessionId)) {
                    return;
                }
                const auto controlChannel = currentSession->controlChannel;
                if (!controlChannel || !controlChannel->isOpen()) {
                    return;
                }
                try {
                    controlChannel->send(message);
                } catch (const std::exception&) {
                    // The peer can close while a queued haptic message is dispatched.
                }
            });
        session->inputBridge = inputBridge;

        rtc::DataChannelInit controlChannelOptions;
        controlChannelOptions.reliability.unordered = false;
        session->controlChannel =
            peerConnection->createDataChannel("control", controlChannelOptions);
        log("Control DataChannel configured: ordered, reliable");

        session->controlChannel->onOpen([this] {
            log("Control DataChannel opened");
        });
        session->controlChannel->onClosed([this, weakSession, inputBridge, sessionId] {
            log("Control DataChannel closed");
            if (const auto currentSession = weakSession.lock();
                currentSession && isCurrentSession(currentSession, sessionId)) {
                inputBridge->onTransportClosed();
            }
        });
        session->controlChannel->onError(
            [this, weakSession, inputBridge, sessionId](const std::string& error) {
            log("Control DataChannel error: " + error);
            if (const auto currentSession = weakSession.lock();
                currentSession && isCurrentSession(currentSession, sessionId)) {
                inputBridge->onTransportClosed();
            }
        });
        session->controlChannel->onMessage(
            [this, weakSession, inputBridge, sessionId](rtc::message_variant data) {
            const auto currentSession = weakSession.lock();
            if (!currentSession || !isCurrentSession(currentSession, sessionId)
                || !std::holds_alternative<std::string>(data)) {
                return;
            }
            if (!inputBridge->handleControlMessage(
                    std::get<std::string>(std::move(data)))) {
                log("Rejected malformed gamepad control message");
            }
        });

        rtc::DataChannelInit gamepadChannelOptions;
        gamepadChannelOptions.reliability.unordered = true;
        gamepadChannelOptions.reliability.maxRetransmits = 0;
        session->gamepadChannel =
            peerConnection->createDataChannel("gamepad", gamepadChannelOptions);
        log("Gamepad DataChannel configured: unordered, maxRetransmits=0");

        session->gamepadChannel->onOpen([this] {
            log("Gamepad DataChannel opened");
        });
        session->gamepadChannel->onClosed([this, weakSession, inputBridge, sessionId] {
            log("Gamepad DataChannel closed");
            if (const auto currentSession = weakSession.lock();
                currentSession && isCurrentSession(currentSession, sessionId)) {
                inputBridge->onTransportClosed();
            }
        });
        session->gamepadChannel->onError(
            [this, weakSession, inputBridge, sessionId](const std::string& error) {
            log("Gamepad DataChannel error: " + error);
            if (const auto currentSession = weakSession.lock();
                currentSession && isCurrentSession(currentSession, sessionId)) {
                inputBridge->onTransportClosed();
            }
        });
        session->gamepadChannel->onMessage(
            [this, weakSession, inputBridge, sessionId](rtc::message_variant data) {
            const auto currentSession = weakSession.lock();
            if (!currentSession || !isCurrentSession(currentSession, sessionId)
                || !std::holds_alternative<std::string>(data)) {
                return;
            }
            if (!inputBridge->handleGamepadMessage(
                    std::get<std::string>(std::move(data)))
                && !currentSession->malformedGamepadMessageReported.exchange(true)) {
                log("Rejected malformed or stale gamepad state message");
            }
        });

        rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        if (settings.codec == gateway::VideoCodec::HEVC) {
            video.addH265Codec(
                VideoPayloadType, gateway::hevcFormatParameters(settings));
        } else {
            video.addH264Codec(VideoPayloadType);
        }
        video.addAttribute(
            gateway::samsungGameModeImageAttribute(settings, VideoPayloadType));
        if (settings.hdr) {
            video.addExtMap(rtc::Description::Entry::ExtMap(
                gateway::PreferredWebRtcColorSpaceExtensionId,
                std::string(gateway::WebRtcColorSpaceExtensionUri)));
        }
        video.addSSRC(VideoSsrc, RtpCname, MediaStreamId, "video");

        session->videoTrack = peerConnection->addTrack(video);

        auto rtpConfiguration = std::make_shared<rtc::RtpPacketizationConfig>(
            VideoSsrc,
            RtpCname,
            VideoPayloadType,
            VideoRtpClockRate);
        session->videoRtpConfiguration = rtpConfiguration;
        session->videoRtpStartTimestamp = rtpConfiguration->startTimestamp;
        auto packetizer = gateway::makeVideoRtpPacketizer(
            settings.codec, rtpConfiguration);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtpConfiguration));
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        packetizer->addToChain(std::make_shared<rtc::PliHandler>(
            [this, weakSession, sessionId] {
            if (const auto currentSession = weakSession.lock();
                currentSession && isCurrentSession(currentSession, sessionId)) {
                const auto requestNumber = currentSession->keyframeRequestCount.fetch_add(
                                               1, std::memory_order_relaxed)
                    + 1;
                log("RTCP PLI/FIR received: keyframe requested (#"
                    + std::to_string(requestNumber) + ")");

                if (sourceMode_ == MediaSourceMode::Moonlight) {
                    std::shared_ptr<gateway::moonlight::MoonlightSession> moonlightSession;
                    {
                        const std::lock_guard lock(currentSession->streamMutex);
                        moonlightSession = currentSession->moonlightSession;
                    }
                    if (moonlightSession) {
                        moonlightSession->onWebRtcKeyframeRequest();
                    }
                } else {
                    currentSession->videoKeyframeRequested.store(
                        true, std::memory_order_relaxed);
                }
            }
        }));
        session->videoTrack->setMediaHandler(packetizer);

        session->videoTrack->onOpen([this, weakSession, sessionId] {
            if (const auto currentSession = weakSession.lock();
                currentSession && isCurrentSession(currentSession, sessionId)) {
                {
                    const std::lock_guard lock(currentSession->streamMutex);
                    currentSession->videoTrackOpen = true;
                }
                log("Video track opened");
                currentSession->streamCondition.notify_all();
            }
        });

        session->videoTrack->onClosed([this, weakSession, sessionId] {
            if (const auto currentSession = weakSession.lock();
                currentSession && isCurrentSession(currentSession, sessionId)) {
                currentSession->requestStreamingStop();
            }
        });

        rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
        audio.addOpusCodec(
            AudioPayloadType,
            "minptime=20;maxaveragebitrate=128000;stereo=1;sprop-stereo=1;useinbandfec=0");
        audio.addSSRC(AudioSsrc, RtpCname, MediaStreamId, "audio");

        session->audioTrack = peerConnection->addTrack(audio);

        auto audioRtpConfiguration = std::make_shared<rtc::RtpPacketizationConfig>(
            AudioSsrc,
            RtpCname,
            AudioPayloadType,
            rtc::OpusRtpPacketizer::DefaultClockRate);
        session->audioRtpStartTimestamp = audioRtpConfiguration->startTimestamp;
        auto audioPacketizer =
            std::make_shared<rtc::OpusRtpPacketizer>(audioRtpConfiguration);
        audioPacketizer->addToChain(
            std::make_shared<rtc::RtcpSrReporter>(audioRtpConfiguration));
        audioPacketizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        session->audioTrack->setMediaHandler(audioPacketizer);

        session->audioTrack->onOpen([this, weakSession, sessionId] {
            if (const auto currentSession = weakSession.lock();
                currentSession && isCurrentSession(currentSession, sessionId)) {
                {
                    const std::lock_guard lock(currentSession->streamMutex);
                    currentSession->audioTrackOpen = true;
                }
                log("Audio track opened");
                currentSession->streamCondition.notify_all();
            }
        });

        session->audioTrack->onClosed([this, weakSession, sessionId] {
            if (const auto currentSession = weakSession.lock();
                currentSession && isCurrentSession(currentSession, sessionId)) {
                currentSession->requestStreamingStop();
            }
        });

        session->mediaSender = std::make_shared<gateway::WebRtcMediaSender>(
            session->videoTrack, session->audioTrack, settings.codec);

        session->streamingThread = std::thread([this, weakSession, sessionId] {
            if (const auto currentSession = weakSession.lock();
                currentSession && isCurrentSession(currentSession, sessionId)) {
                if (sourceMode_ == MediaSourceMode::Moonlight) {
                    streamMoonlightMedia(currentSession);
                } else {
                    streamTestMedia(*currentSession);
                }
            }
        });

        peerConnection->setLocalDescription();
    }

    bool waitForMediaTracks(Session& session)
    {
        std::unique_lock lock(session.streamMutex);
        session.streamCondition.wait(lock, [&session] {
            return session.stopRequested
                || (session.peerConnected && session.videoTrackOpen && session.audioTrackOpen);
        });
        return !session.stopRequested;
    }

    void streamMoonlightMedia(const std::shared_ptr<Session>& session)
    {
        if (!waitForMediaTracks(*session)) {
            return;
        }

        sendSessionStatus(session, "connecting-sunshine");
        auto options = configuredMoonlightOptions();
        options.applicationId = session->applicationId;
        options.settings = session->settings;

        auto moonlightSession = std::make_shared<gateway::moonlight::MoonlightSession>(
            *session->mediaSender,
            *identity_,
            std::move(options),
            [this](const std::string& message) { log(message); },
            [weakSession = std::weak_ptr<Session>(session)] {
                if (const auto currentSession = weakSession.lock()) {
                    currentSession->requestStreamingStop();
                }
            },
            [weakInputBridge = std::weak_ptr<gateway::moonlight::MoonlightInputBridge>(
                 session->inputBridge)](std::uint16_t controllerNumber,
                                        std::uint16_t lowFrequencyMotor,
                                        std::uint16_t highFrequencyMotor) {
                if (const auto inputBridge = weakInputBridge.lock()) {
                    inputBridge->handleRumble(controllerNumber,
                                               lowFrequencyMotor,
                                               highFrequencyMotor);
                }
            },
            [weakInputBridge = std::weak_ptr<gateway::moonlight::MoonlightInputBridge>(
                 session->inputBridge)](std::uint16_t controllerNumber,
                                        std::uint16_t leftTriggerMotor,
                                        std::uint16_t rightTriggerMotor) {
                if (const auto inputBridge = weakInputBridge.lock()) {
                    inputBridge->handleTriggerRumble(controllerNumber,
                                                      leftTriggerMotor,
                                                      rightTriggerMotor);
                }
            });
        {
            const std::lock_guard lock(session->streamMutex);
            session->moonlightSession = moonlightSession;
        }

        try {
            sendSessionStatus(session, "starting-moonlight");
            moonlightSession->start();
            session->inputBridge->setMoonlightSessionActive(true);
            {
                const std::lock_guard lock(session->streamMutex);
                session->streamingActive = true;
            }
            sendSessionStatus(session, "streaming");
            std::unique_lock lock(session->streamMutex);
            session->streamCondition.wait(
                lock, [&session] { return session->stopRequested; });
        } catch (const std::exception& error) {
            log("Moonlight streaming error: " + std::string(error.what()));
            sendSessionStatus(session, "error", error.what());
            session->requestStreamingStop();
        }

        session->inputBridge->setMoonlightSessionActive(false);
        moonlightSession->stop();
        {
            const std::lock_guard lock(session->streamMutex);
            session->streamingActive = false;
            session->moonlightSession.reset();
        }
        log("Moonlight streaming stopped");
    }

    void streamTestMedia(Session& session)
    {
        if (!waitForMediaTracks(session)) {
            return;
        }

        log("Video streaming started");
        log("Audio streaming started");

        using Clock = std::chrono::steady_clock;
        const auto streamStart = Clock::now();
        auto nextVideoReport = streamStart + std::chrono::seconds(1);
        auto nextAudioReport = streamStart + std::chrono::seconds(1);
        std::uint64_t videoFramesSent = 0;
        std::uint64_t videoBytesSent = 0;
        std::uint64_t audioPacketsSent = 0;
        std::uint64_t audioBytesSent = 0;
        std::size_t videoFrameIndex = 0;
        std::size_t audioPacketIndex = 0;
        std::uint64_t audioSamplesSent = 0;
        std::chrono::microseconds audioElapsed{0};

        while (true) {
            {
                const std::lock_guard lock(session.streamMutex);
                if (session.stopRequested || !session.peerConnected || !session.videoTrackOpen
                    || !session.audioTrackOpen) {
                    break;
                }
            }

            const std::chrono::duration<double> videoTimestamp(
                static_cast<double>(videoFramesSent) / VideoFrameRate);
            const std::chrono::duration<double> audioTimestamp = audioElapsed;
            const bool sendVideo = videoTimestamp <= audioTimestamp;
            const std::chrono::duration<double> mediaTimestamp =
                sendVideo ? videoTimestamp : audioTimestamp;
            const auto sendTime = streamStart
                + std::chrono::duration_cast<Clock::duration>(mediaTimestamp);

            {
                std::unique_lock lock(session.streamMutex);
                if (session.streamCondition.wait_until(lock, sendTime, [&session] {
                        return session.stopRequested || !session.peerConnected
                            || !session.videoTrackOpen || !session.audioTrackOpen;
                    })) {
                    break;
                }
            }

            try {
                if (sendVideo) {
                    const bool keyframeRequested = session.videoKeyframeRequested.exchange(
                        false, std::memory_order_relaxed);
                    bool idrScheduled = false;
                    if (keyframeRequested) {
                        const auto idrIndex = videoSource_->firstIdrAccessUnitIndex();
                        if (idrIndex) {
                            videoFrameIndex = *idrIndex;
                            idrScheduled = true;
                        }
                    }

                    const auto& accessUnit = videoSource_->accessUnits()[videoFrameIndex];
                    const auto rtpTimestamp = session.videoRtpStartTimestamp
                        + static_cast<std::uint32_t>(
                            (videoFramesSent * VideoRtpClockRate)
                            / static_cast<std::uint64_t>(VideoFrameRate));
                    session.mediaSender->sendVideoAccessUnit(
                        gateway::VideoCodec::H264, accessUnit, rtpTimestamp);

                    ++videoFramesSent;
                    videoBytesSent += accessUnit.size();
                    ++videoFrameIndex;

                    if (idrScheduled) {
                        log("H.264 IDR sent in response to RTCP PLI/FIR");
                    } else if (keyframeRequested) {
                        log("RTCP PLI/FIR response unavailable: sample has no H.264 IDR");
                    }

                    if (videoFrameIndex == videoSource_->accessUnits().size()) {
                        videoFrameIndex = 0;
                        log("Video loop restarted");
                    }
                } else {
                    const auto& packet = audioSource_->packets()[audioPacketIndex];
                    const auto rtpTimestamp = session.audioRtpStartTimestamp
                        + static_cast<std::uint32_t>(audioSamplesSent);
                    session.mediaSender->sendOpusPacket(packet, rtpTimestamp);

                    ++audioPacketsSent;
                    audioBytesSent += packet.size();
                    const auto packetDuration = audioSource_->packetDuration(audioPacketIndex);
                    const auto packetSampleCount =
                        audioSource_->packetSampleCounts()[audioPacketIndex];
                    audioElapsed += packetDuration;
                    audioSamplesSent += packetSampleCount;
                    ++audioPacketIndex;

                    if (audioPacketIndex == audioSource_->packets().size()) {
                        audioPacketIndex = 0;
                        log("Audio loop restarted");
                    }
                }
            } catch (const std::exception& error) {
                log("Media streaming error: " + std::string(error.what()));
                session.requestStreamingStop();
                break;
            }

            const auto now = Clock::now();
            if (now >= nextVideoReport) {
                std::ostringstream message;
                message << "Video streaming: " << videoFramesSent << " frames sent, "
                        << videoBytesSent << " bytes";
                log(message.str());

                do {
                    nextVideoReport += std::chrono::seconds(1);
                } while (nextVideoReport <= now);
            }
            if (now >= nextAudioReport) {
                std::ostringstream message;
                message << "Audio streaming: " << audioPacketsSent << " packets sent, "
                        << audioBytesSent << " bytes";
                log(message.str());

                do {
                    nextAudioReport += std::chrono::seconds(1);
                } while (nextAudioReport <= now);
            }
        }

        log("Video streaming stopped");
        log("Audio streaming stopped");
    }

    gateway::protocol::GatewayStatus gatewayStatus(
        const std::shared_ptr<Session>& session)
    {
        gateway::protocol::GatewayStatus status;
        {
            const std::lock_guard lock(session->streamMutex);
            status.sessionActive = session->sessionId != 0;
        }

        if (sourceMode_ == MediaSourceMode::Test) {
            return status;
        }

        try {
            const auto detected = gateway::moonlight::MoonlightSession::detectSunshine(
                *identity_, configuredMoonlightOptions().host, [this](const std::string& message) {
                    log(message);
                });
            status.sunshineDetected = true;
            status.sunshinePaired = detected.pairedHost.has_value()
                && detected.serverInfo.pairStatus == 1;
            if (!detected.serverInfo.hostname.empty()) {
                status.gatewayName = detected.serverInfo.hostname;
            }
            const int runningAppId = runningSunshineApplicationId(detected.serverInfo);
            if (runningAppId != 0) {
                status.runningAppId = std::to_string(runningAppId);
            }
        } catch (const std::exception& error) {
            log("Sunshine status unavailable: " + std::string(error.what()));
        }
        return status;
    }

    std::vector<gateway::protocol::Application> loadApplications()
    {
        if (sourceMode_ == MediaSourceMode::Test) {
            return {{"test", "Test Media"}};
        }

        const auto detected = gateway::moonlight::MoonlightSession::detectSunshine(
            *identity_, configuredMoonlightOptions().host, [this](const std::string& message) {
                log(message);
            });
        if (!detected.pairedHost || detected.serverInfo.pairStatus != 1) {
            throw std::runtime_error("Moonlight WebRTC Gateway is not paired with Sunshine");
        }

        gateway::moonlight::SunshineHttpClient client(*identity_, detected.address);
        client.setHttpsPort(detected.serverInfo.httpsPort);
        client.setPinnedServerCertificate(
            detected.pairedHost->serverCertificatePem);

        const int runningAppId = runningSunshineApplicationId(detected.serverInfo);
        const auto sunshineApplications = client.getAppList();
        std::vector<gateway::protocol::Application> result;
        std::unordered_map<std::string, std::string> titles;
        for (const auto& application : sunshineApplications) {
            const std::string& appId = application.id;
            const auto cached = artworkCache_.find(detected.serverInfo.uniqueId, appId);
            result.push_back({appId,
                              application.title,
                              cached && cached->available,
                              application.id == std::to_string(runningAppId)});
            titles[detected.serverInfo.uniqueId + '\n' + appId] = application.title;
        }
        {
            const std::lock_guard lock(applicationTitleMutex_);
            applicationTitles_.insert(titles.begin(), titles.end());
        }
        return result;
    }

    void sendApplicationArtwork(
        const std::shared_ptr<Session>& session,
        const gateway::protocol::GetAppArtworkRequest& request)
    {
        if (sourceMode_ == MediaSourceMode::Test) {
            sendJson(session, gateway::protocol::makeAppArtwork(request.appId, false));
            return;
        }

        std::string hostId;
        try {
            const auto detected = gateway::moonlight::MoonlightSession::detectSunshine(
                *identity_, configuredMoonlightOptions().host, [this](const std::string& message) {
                    log(message);
                });
            if (!detected.pairedHost || detected.serverInfo.pairStatus != 1) {
                throw std::runtime_error("Moonlight WebRTC Gateway is not paired with Sunshine");
            }
            hostId = detected.serverInfo.uniqueId;

            gateway::moonlight::SunshineHttpClient client(*identity_, detected.address);
            client.setHttpsPort(detected.serverInfo.httpsPort);
            client.setPinnedServerCertificate(detected.pairedHost->serverCertificatePem);
            auto applications = client.getAppList();
            const auto application = std::ranges::find_if(
                applications, [&request](const auto& candidate) {
                    return candidate.id == request.appId;
                });
            if (application == applications.end()) {
                throw std::runtime_error("Sunshine /applist no longer contains the requested app ID");
            }

            const std::string target =
                gateway::moonlight::SunshineHttpClient::appArtworkRequestTarget(request.appId);
            log("[Artwork] App: " + application->title);
            log("[Artwork] Sunshine applist ID: " + application->id);
            log("[Artwork] Requested appasset ID: " + request.appId);
            log("[Artwork] URL: " + target);

            auto artwork = artworkCache_.find(hostId, request.appId);
            if (!artwork) {
                const auto sunshineArtwork = client.getAppArtwork(request.appId);
                artwork = gateway::ApplicationArtwork{
                    true, sunshineArtwork.mimeType, sunshineArtwork.bytes};
                artworkCache_.store(hostId, request.appId, *artwork);
                log("[Artwork] HTTP status: " + std::to_string(sunshineArtwork.httpStatus));
                log("[Artwork] Content-Type: " + sunshineArtwork.mimeType);
                log("[Artwork] Bytes received: "
                    + std::to_string(sunshineArtwork.bytes.size()));
            } else {
                log("[Artwork] Cache hit: " + request.appId);
                log("[Artwork] Content-Type: " + artwork->mimeType);
                log("[Artwork] Bytes received: " + std::to_string(artwork->bytes.size()));
            }

            if (artwork->available) {
                sendJson(session,
                         gateway::protocol::makeAppArtwork(
                             request.appId,
                             true,
                             artwork->mimeType,
                             base64Encode(artwork->bytes)));
            } else {
                sendJson(session, gateway::protocol::makeAppArtwork(request.appId, false));
            }
        } catch (const std::exception& error) {
            log("Sunshine artwork unavailable for application " + request.appId + ": "
                + error.what());
            if (!hostId.empty()) {
                artworkCache_.store(hostId, request.appId, {});
            }
            sendJson(session, gateway::protocol::makeAppArtwork(request.appId, false));
        }
    }

public:
    gateway::managementipc::Result handleManagementCommand(
        const gateway::managementipc::Command& command)
    {
        if (sourceMode_ != MediaSourceMode::Moonlight) {
            return {false, "unsupported-source", "Sunshine management requires the Moonlight source"};
        }
        if (command.type == gateway::managementipc::CommandType::SetHost) {
            if (!gateway::moonlight::MoonlightIdentity::isValidSunshineHost(command.host)) {
                return {false, "invalid-host", "Enter a hostname or IPv4 address without a URL or port"};
            }
            try {
                identity_->saveConfiguredSunshineHost(command.host);
                if (!explicitMoonlightHost_) {
                    std::lock_guard lock(moonlightOptionsMutex_);
                    moonlightOptions_.host = command.host;
                }
                return {true, "saved", explicitMoonlightHost_
                    ? "Host saved; the explicit --host value remains active until restart"
                    : "Sunshine host saved"};
            } catch (const std::exception&) {
                return {false, "persistence-failed", "Unable to save the Sunshine host"};
            }
        }

        if (command.type == gateway::managementipc::CommandType::PairStatus) {
            std::lock_guard lock(pairingMutex_);
            return pairingResult_;
        }

        if (command.type == gateway::managementipc::CommandType::Pair) {
            return startPairing();
        }

        if (command.type == gateway::managementipc::CommandType::Unpair) {
            return unpairConfiguredHost();
        }

        if (command.type != gateway::managementipc::CommandType::Test) {
            return {false, "unsupported-command", "Unsupported Sunshine management command"};
        }
        try {
            const auto detected = gateway::moonlight::MoonlightSession::detectSunshine(
                *identity_, configuredMoonlightOptions().host, [](const std::string&) {});
            if (!detected.pairedHost || detected.serverInfo.pairStatus != 1) {
                return {false, "not-paired", "Sunshine is reachable but this Gateway is not paired"};
            }
            gateway::moonlight::SunshineHttpClient client(*identity_, detected.address);
            client.setHttpsPort(detected.serverInfo.httpsPort);
            client.setPinnedServerCertificate(detected.pairedHost->serverCertificatePem);
            (void)client.getServerInfo(true, std::chrono::seconds(5));
            return {true, "reachable", "Sunshine is reachable and its pinned TLS connection succeeded"};
        } catch (const std::exception& error) {
            const std::string detail = error.what();
            const bool tls = detail.find("certificate") != std::string::npos
                || detail.find("SSL") != std::string::npos || detail.find("TLS") != std::string::npos;
            const bool protocol = detail.find("XML") != std::string::npos
                || detail.find("status") != std::string::npos;
            return {false, tls ? "tls-pinning-failed" : protocol ? "protocol-failed" : "unreachable",
                    tls ? "Sunshine TLS or certificate pinning failed" : protocol
                        ? "Sunshine responded with an invalid protocol response"
                        : "Sunshine is unreachable"};
        }
    }

    gateway::serviceipc::StatusSnapshot localServiceStatus()
    {
        gateway::serviceipc::StatusSnapshot snapshot;
        std::shared_ptr<Session> session;
        {
            const std::lock_guard lock(sessionMutex_);
            session = activeSession_;
        }
        if (session) {
            const std::lock_guard lock(session->streamMutex);
            snapshot.sessionActive = session->sessionId != 0;
            snapshot.connectedTvClients = session->socket && session->socket->isOpen() ? 1U : 0U;
        } else {
            snapshot.sessionActive = false;
            snapshot.connectedTvClients = 0;
        }

        if (sourceMode_ == MediaSourceMode::Test) {
            return snapshot;
        }

        const auto moonlightOptions = configuredMoonlightOptions();
        if (moonlightOptions.host) {
            snapshot.sunshineHost = *moonlightOptions.host;
        }

        try {
            const auto detected = gateway::moonlight::MoonlightSession::detectSunshine(
                *identity_, moonlightOptions.host, [](const std::string&) {});
            snapshot.sunshineConnected = true;
            snapshot.sunshinePaired = detected.pairedHost.has_value()
                && detected.serverInfo.pairStatus == 1;
            if (!detected.serverInfo.hostname.empty()) {
                snapshot.sunshineHost = detected.serverInfo.hostname;
            }
            const int runningAppId = runningSunshineApplicationId(detected.serverInfo);
            if (runningAppId != 0) {
                snapshot.runningApplicationId = std::to_string(runningAppId);
                const std::string cacheKey = detected.serverInfo.uniqueId + '\n' + *snapshot.runningApplicationId;
                {
                    const std::lock_guard lock(applicationTitleMutex_);
                    if (const auto cached = applicationTitles_.find(cacheKey); cached != applicationTitles_.end()) {
                        snapshot.runningApplicationName = cached->second;
                    }
                }
            }
        } catch (const std::exception&) {
            snapshot.sunshineConnected = false;
        }
        return snapshot;
    }

private:
    gateway::managementipc::Result startPairing()
    {
        bool joinFinishedPairing = false;
        {
            std::lock_guard lock(pairingMutex_);
            if (pairingInProgress_) {
                return {false, "pairing-in-progress", "Pairing is already in progress"};
            }
            joinFinishedPairing = pairingThread_.joinable();
        }
        if (joinFinishedPairing) pairingThread_.join();

        try {
            const auto detected = gateway::moonlight::MoonlightSession::detectSunshine(
                *identity_, configuredMoonlightOptions().host, [](const std::string&) {});
            if (detected.pairedHost && detected.serverInfo.pairStatus == 1) {
                return {false, "already-paired", "This Gateway is already paired with Sunshine"};
            }
            if (detected.serverInfo.pairStatus == 1) {
                return {false, "pairing-state-unknown", "Sunshine reports pairing but local trust is unavailable; pairing was not replaced"};
            }

            const std::string pin = gateway::moonlight::MoonlightPairing::generatePin();
            {
                std::lock_guard lock(pairingMutex_);
                pairingInProgress_ = true;
                pairingResult_ = {false, "pairing-in-progress", "Pairing is in progress"};
            }
            pairingThread_ = std::thread([this, detected, pin] {
                gateway::managementipc::Result result;
                try {
                    gateway::moonlight::SunshineHttpClient client(*identity_, detected.address);
                    client.setHttpsPort(detected.serverInfo.httpsPort);
                    gateway::moonlight::MoonlightPairing pairing(*identity_, client);
                    const auto outcome = pairing.pair(detected.serverInfo.appVersion, pin);
                    if (outcome.result == gateway::moonlight::MoonlightPairing::Result::IncorrectPin) {
                        result = {false, "incorrect-pin", "Sunshine rejected the pairing PIN"};
                    } else if (outcome.result == gateway::moonlight::MoonlightPairing::Result::AlreadyInProgress) {
                        result = {false, "pairing-busy", "Sunshine is already handling another pairing request"};
                    } else if (outcome.result != gateway::moonlight::MoonlightPairing::Result::Paired) {
                        result = {false, "pairing-failed", "Sunshine pairing did not complete"};
                    } else {
                        gateway::moonlight::PairedSunshineHost host{
                            detected.serverInfo.uniqueId, detected.serverInfo.hostname, detected.address,
                            detected.serverInfo.httpsPort, outcome.serverCertificatePem};
                        identity_->savePairedHost(host);
                        client.setPinnedServerCertificate(outcome.serverCertificatePem);
                        const auto verified = client.getServerInfo(true, std::chrono::seconds(5));
                        result = verified.pairStatus == 1
                            ? gateway::managementipc::Result{true, "paired", "Sunshine pairing completed"}
                            : gateway::managementipc::Result{false, "pairing-unverified", "Sunshine did not confirm the new pairing"};
                    }
                } catch (const std::exception&) {
                    result = {false, "pairing-failed", "Sunshine pairing failed"};
                }
                {
                    std::lock_guard lock(pairingMutex_);
                    pairingResult_ = std::move(result);
                    pairingInProgress_ = false;
                }
            });
            return {true, "pairing-started", "Enter this PIN in Sunshine to continue pairing", pin};
        } catch (const std::exception&) {
            return {false, "unreachable", "Sunshine is unreachable"};
        }
    }

    gateway::managementipc::Result unpairConfiguredHost()
    {
        {
            const std::lock_guard lock(sessionMutex_);
            if (activeSession_) {
                const std::lock_guard streamLock(activeSession_->streamMutex);
                if (activeSession_->sessionId != 0) {
                    return {false, "active-session", "Disconnect or stop the active stream before unpairing"};
                }
            }
        }
        try {
            const auto detected = gateway::moonlight::MoonlightSession::detectSunshine(
                *identity_, configuredMoonlightOptions().host, [](const std::string&) {});
            if (!detected.pairedHost || detected.serverInfo.pairStatus != 1) {
                return {false, "not-paired", "This Gateway is not paired with Sunshine"};
            }
            gateway::moonlight::SunshineHttpClient client(*identity_, detected.address);
            client.setHttpsPort(detected.serverInfo.httpsPort);
            client.setPinnedServerCertificate(detected.pairedHost->serverCertificatePem);
            if (!identity_->removePairedHost(detected.serverInfo.uniqueId)) {
                return {false, "local-trust-missing", "Local Sunshine trust was already unavailable"};
            }
            return {true, "local-unpaired", "Local Sunshine trust was removed; Sunshine has no remote unpair endpoint"};
        } catch (const std::exception& error) {
            log("Local Sunshine trust removal failed: " + std::string(error.what()));
            return {false, "unpair-failed", "Local Sunshine trust was preserved"};
        }
    }

    gateway::moonlight::MoonlightSessionOptions configuredMoonlightOptions() const
    {
        const std::lock_guard lock(moonlightOptionsMutex_);
        return moonlightOptions_;
    }

    void sendInitialState(const std::shared_ptr<Session>& session)
    {
        sendJson(session, gateway::protocol::makeGatewayStatus(gatewayStatus(session)));
        sendJson(session, gateway::protocol::makeCapabilities());
        sendJson(session, gateway::protocol::makeSessionStatus("idle"));
    }

    void sendApplications(const std::shared_ptr<Session>& session)
    {
        try {
            const auto applications = loadApplications();
            sendJson(session, gateway::protocol::makeApps(applications));
            log("Sent " + std::to_string(applications.size())
                + " Sunshine applications");
        } catch (const std::exception& error) {
            sendJson(session,
                     gateway::protocol::makeError(
                         "get-apps", "sunshine-unavailable", error.what()));
        }
    }

    void sendSessionStatus(const std::shared_ptr<Session>& session,
                           std::string_view state,
                           std::string_view detail = {})
    {
        std::optional<std::uint64_t> sessionId;
        std::optional<gateway::StreamSettings> settings;
        {
            const std::lock_guard lock(session->streamMutex);
            if (session->sessionId != 0) {
                sessionId = session->sessionId;
                settings = session->settings;
            }
        }
        sendJson(session,
                 gateway::protocol::makeSessionStatus(
                     state,
                     sessionId,
                     settings,
                     detail.empty()
                         ? std::nullopt
                         : std::optional<std::string>(detail)));
        log("Session status: " + std::string(state));
    }

    void sendHostSessionStatus(const std::shared_ptr<Session>& session,
                               std::string_view state,
                               std::optional<std::string> runningAppId = std::nullopt,
                               std::optional<std::string> targetAppId = std::nullopt,
                               std::string_view detail = {})
    {
        sendJson(session,
                 gateway::protocol::makeHostSessionStatus(
                     state,
                     std::move(runningAppId),
                     std::move(targetAppId),
                     detail.empty()
                         ? std::nullopt
                         : std::optional<std::string>(detail)));
    }

    void startSession(
        const std::shared_ptr<Session>& session,
        const gateway::protocol::StartSessionRequest& request)
    {
        if (sourceMode_ == MediaSourceMode::Test
            && request.settings.codec != gateway::VideoCodec::H264) {
            sendJson(session,
                     gateway::protocol::makeError(
                         "start-session",
                         "unsupported-test-source",
                         "The built-in test source contains H.264 media only"));
            return;
        }
        {
            const std::lock_guard lock(session->streamMutex);
            if (session->hostOperationActive) {
                sendJson(session,
                         gateway::protocol::makeError(
                             "start-session",
                             "host-operation-active",
                             "Wait for the current Sunshine operation to complete"));
                return;
            }
            if (session->sessionId != 0 || session->peerConnection) {
                sendJson(session,
                         gateway::protocol::makeError(
                             "start-session",
                             "session-active",
                             "Stop the active session before starting another"));
                return;
            }
        }

        std::optional<int> numericApplicationId;
        std::string applicationTitle;
        try {
            const auto applications = loadApplications();
            const auto application = std::ranges::find_if(
                applications, [&request](const auto& candidate) {
                    return candidate.id == request.appId;
                });
            if (application == applications.end()) {
                throw std::runtime_error("Selected Sunshine application no longer exists");
            }
            applicationTitle = application->title;

            if (sourceMode_ == MediaSourceMode::Moonlight) {
                int value = 0;
                const auto result = std::from_chars(
                    request.appId.data(),
                    request.appId.data() + request.appId.size(),
                    value);
                if (result.ec != std::errc()
                    || result.ptr != request.appId.data() + request.appId.size()
                    || value < 0) {
                    throw std::runtime_error("Invalid Sunshine application ID");
                }
                numericApplicationId = value;
            }
        } catch (const std::exception& error) {
            sendJson(session,
                     gateway::protocol::makeError(
                         "start-session", "invalid-application", error.what()));
            return;
        }

        const auto sessionId = nextSessionId_.fetch_add(1, std::memory_order_relaxed);
        log("Validated session settings: app=" + applicationTitle + ", video="
            + std::to_string(request.settings.width) + "x"
            + std::to_string(request.settings.height) + "@"
            + std::to_string(request.settings.fps) + ", codec="
            + std::string(gateway::videoCodecName(request.settings.codec))
            + ", bitrate=" + std::to_string(request.settings.bitrateKbps)
            + " kbps, HDR=" + (request.settings.hdr ? "ON" : "OFF")
            + ", audio=stereo 48000 Hz");
        {
            const std::lock_guard lock(session->streamMutex);
            session->sessionId = sessionId;
            session->settings = request.settings;
            session->applicationId = numericApplicationId;
            session->peerConnected = false;
            session->videoTrackOpen = false;
            session->audioTrackOpen = false;
            session->stopRequested = false;
            session->streamingActive = false;
        }
        {
            const std::lock_guard lock(session->candidateMutex);
            session->hasRemoteDescription = false;
            session->pendingCandidates.clear();
        }
        session->videoKeyframeRequested.store(false);
        session->keyframeRequestCount.store(0);
        session->malformedGamepadMessageReported.store(false);

        sendSessionStatus(session, "starting");
        sendSessionStatus(session, "starting-webrtc");
        try {
            createPeerConnection(session, sessionId, request.settings);
        } catch (const std::exception& error) {
            log("Session startup failed: " + std::string(error.what()));
            sendSessionStatus(session, "error", error.what());
            stopActiveSession(session, true);
        }
    }

    void stopActiveSession(const std::shared_ptr<Session>& session,
                           bool notifyClient)
    {
        std::uint64_t sessionId = 0;
        std::shared_ptr<gateway::moonlight::MoonlightInputBridge> inputBridge;
        std::shared_ptr<rtc::PeerConnection> peerConnection;
        {
            const std::lock_guard lock(session->streamMutex);
            sessionId = session->sessionId;
            inputBridge = session->inputBridge;
            peerConnection = session->peerConnection;
        }

        if (sessionId == 0) {
            if (notifyClient) {
                sendJson(session, gateway::protocol::makeSessionStatus("idle"));
            }
            return;
        }

        if (notifyClient) {
            sendSessionStatus(session, "stopping");
        }
        if (inputBridge) {
            inputBridge->onTransportClosed();
        }
        session->stopStreaming();
        if (peerConnection) {
            peerConnection->close();
        }

        {
            const std::lock_guard lock(session->streamMutex);
            session->sessionId = 0;
            session->peerConnection.reset();
            session->videoTrack.reset();
            session->audioTrack.reset();
            session->controlChannel.reset();
            session->gamepadChannel.reset();
            session->mediaSender.reset();
            session->moonlightSession.reset();
            session->inputBridge.reset();
            session->applicationId.reset();
            session->peerConnected = false;
            session->videoTrackOpen = false;
            session->audioTrackOpen = false;
            session->streamingActive = false;
        }
        {
            const std::lock_guard lock(session->candidateMutex);
            session->hasRemoteDescription = false;
            session->pendingCandidates.clear();
        }

        log("Session " + std::to_string(sessionId) + " stopped; Gateway remains idle");
        if (notifyClient) {
            sendJson(session, gateway::protocol::makeSessionStatus("idle"));
            sendJson(session,
                     gateway::protocol::makeGatewayStatus(gatewayStatus(session)));
        }
    }

    bool beginHostOperation(const std::shared_ptr<Session>& session,
                            std::string_view requestType)
    {
        const std::lock_guard lock(session->streamMutex);
        if (session->hostOperationActive) {
            sendJson(session,
                     gateway::protocol::makeError(
                         requestType,
                         "host-operation-active",
                         "A Sunshine session operation is already in progress"));
            return false;
        }
        session->hostOperationActive = true;
        return true;
    }

    void finishHostOperation(const std::shared_ptr<Session>& session)
    {
        const std::lock_guard lock(session->streamMutex);
        session->hostOperationActive = false;
    }

    bool waitForSunshineIdle(gateway::moonlight::SunshineHttpClient& client)
    {
        constexpr auto PollInterval = std::chrono::milliseconds(500);
        constexpr int PollAttempts = 20;
        for (int attempt = 0; attempt < PollAttempts; ++attempt) {
            const auto serverInfo = client.getServerInfo(true, std::chrono::seconds(5));
            if (runningSunshineApplicationId(serverInfo) == 0) {
                return true;
            }
            if (attempt + 1 < PollAttempts) {
                std::this_thread::sleep_for(PollInterval);
            }
        }
        return false;
    }

    void reconcileApplications(const std::shared_ptr<Session>& session)
    {
        sendApplications(session);
        sendJson(session, gateway::protocol::makeGatewayStatus(gatewayStatus(session)));
    }

    void stopOrSwitchHostSession(
        const std::shared_ptr<Session>& session,
        std::optional<gateway::protocol::StartSessionRequest> switchTarget)
    {
        const std::string requestType = switchTarget ? "switch-session" : "stop-host-session";
        if (sourceMode_ != MediaSourceMode::Moonlight) {
            sendJson(session,
                     gateway::protocol::makeError(
                         requestType,
                         "unsupported-test-source",
                         "Host session management requires the Moonlight source"));
            return;
        }
        if (!beginHostOperation(session, requestType)) {
            return;
        }

        try {
            const auto detected = gateway::moonlight::MoonlightSession::detectSunshine(
                *identity_, configuredMoonlightOptions().host, [this](const std::string& message) {
                    log(message);
                });
            if (!detected.pairedHost || detected.serverInfo.pairStatus != 1) {
                throw std::runtime_error("Moonlight WebRTC Gateway is not paired with Sunshine");
            }

            gateway::moonlight::SunshineHttpClient client(*identity_, detected.address);
            client.setHttpsPort(detected.serverInfo.httpsPort);
            client.setPinnedServerCertificate(detected.pairedHost->serverCertificatePem);

            const auto currentInfo = client.getServerInfo(true, std::chrono::seconds(5));
            const int runningAppId = runningSunshineApplicationId(currentInfo);
            const auto runningId = runningAppId == 0
                ? std::optional<std::string>{}
                : std::optional<std::string>(std::to_string(runningAppId));

            if (switchTarget) {
                const auto applications = client.getAppList();
                const auto target = std::ranges::find_if(
                    applications, [&switchTarget](const auto& application) {
                        return application.id == switchTarget->appId;
                    });
                if (target == applications.end()) {
                    throw std::runtime_error("Selected Sunshine application no longer exists");
                }
                if (runningId && *runningId == switchTarget->appId) {
                    sendHostSessionStatus(session,
                                          "resuming",
                                          runningId,
                                          switchTarget->appId);
                    finishHostOperation(session);
                    startSession(session, *switchTarget);
                    return;
                }
            }

            if (!runningId) {
                if (switchTarget) {
                    sendHostSessionStatus(session, "starting", std::nullopt, switchTarget->appId);
                    finishHostOperation(session);
                    startSession(session, *switchTarget);
                } else {
                    reconcileApplications(session);
                    sendHostSessionStatus(session, "stopped");
                    finishHostOperation(session);
                }
                return;
            }

            sendHostSessionStatus(session,
                                  switchTarget ? "switching" : "stopping",
                                  runningId,
                                  switchTarget
                                      ? std::optional<std::string>(switchTarget->appId)
                                      : std::nullopt);

            // A local WebRTC disconnect remains distinct from /cancel. Here it is only
            // performed because the explicit host-stop operation is about to cancel Sunshine.
            stopActiveSession(session, false);
            client.cancelRunningApplication();
            if (!waitForSunshineIdle(client)) {
                throw std::runtime_error(
                    "Sunshine still reports a running application after cancellation");
            }

            reconcileApplications(session);
            if (switchTarget) {
                sendHostSessionStatus(session, "starting", std::nullopt, switchTarget->appId);
                finishHostOperation(session);
                startSession(session, *switchTarget);
            } else {
                sendHostSessionStatus(session, "stopped");
                finishHostOperation(session);
            }
        } catch (const std::exception& error) {
            log("Sunshine host session operation failed: " + std::string(error.what()));
            reconcileApplications(session);
            sendHostSessionStatus(session, "failed", std::nullopt, std::nullopt, error.what());
            finishHostOperation(session);
        }
    }

    void handleMessage(const std::shared_ptr<Session>& session, const std::string& text)
    {
        try {
            const auto message = gateway::protocol::parseClientMessage(text);
            if (std::holds_alternative<gateway::protocol::GetAppsRequest>(
                    message.payload)) {
                sendApplications(session);
            } else if (const auto* artwork =
                           std::get_if<gateway::protocol::GetAppArtworkRequest>(
                               &message.payload)) {
                sendApplicationArtwork(session, *artwork);
            } else if (const auto* start =
                           std::get_if<gateway::protocol::StartSessionRequest>(
                               &message.payload)) {
                startSession(session, *start);
            } else if (std::holds_alternative<gateway::protocol::StopSessionRequest>(
                           message.payload)) {
                stopActiveSession(session, true);
            } else if (std::holds_alternative<gateway::protocol::StopHostSessionRequest>(
                           message.payload)) {
                stopOrSwitchHostSession(session, std::nullopt);
            } else if (const auto* switchRequest =
                           std::get_if<gateway::protocol::SwitchSessionRequest>(
                               &message.payload)) {
                stopOrSwitchHostSession(
                    session,
                    gateway::protocol::StartSessionRequest{
                        switchRequest->appId, switchRequest->settings});
            } else if (const auto* answer =
                           std::get_if<gateway::protocol::AnswerMessage>(
                               &message.payload)) {
                handleAnswer(session, answer->sessionId, answer->sdp);
            } else if (const auto* candidate =
                           std::get_if<gateway::protocol::CandidateMessage>(
                               &message.payload)) {
                handleCandidate(session,
                                candidate->sessionId,
                                candidate->candidate,
                                candidate->mid);
            }
        } catch (const gateway::protocol::ProtocolError& error) {
            log("Gateway protocol error: " + std::string(error.what()));
            sendJson(session,
                     gateway::protocol::makeError(
                         "unknown", error.code(), error.what()));
        } catch (const std::exception& error) {
            log("Gateway control error: " + std::string(error.what()));
            sendJson(session,
                     gateway::protocol::makeError(
                         "unknown", "internal-error", error.what()));
        }
    }

    void handleAnswer(const std::shared_ptr<Session>& session,
                      std::uint64_t sessionId,
                      const std::string& sdp)
    {
        if (!isCurrentSession(session, sessionId)) {
            log("Ignored SDP answer for inactive session " + std::to_string(sessionId));
            return;
        }
        const auto peerConnection = session->peerConnection;
        if (!peerConnection) {
            return;
        }

        log("Incoming SDP answer:\n" + sdp);
        if (!gateway::hasExpectedHevcFormatParameters(
                sdp, session->settings, VideoPayloadType)) {
            log("Tizen rejected the requested HEVC Main10 SDP profile");
            sendSessionStatus(session,
                              "error",
                              "Tizen did not negotiate HEVC Main10");
            session->requestStreamingStop();
            return;
        }
        if (session->settings.hdr) {
            if (const auto levelId = gateway::hevcLevelId(sdp, VideoPayloadType)) {
                log("Tizen HEVC Main10 answer level-id=" + std::to_string(*levelId));
                const int offeredLevel = session->settings.width == 3840 ? 153 : 123;
                if (*levelId < offeredLevel) {
                    log("Tizen lowered the HEVC level-id from "
                        + std::to_string(offeredLevel) + " to "
                        + std::to_string(*levelId)
                        + "; continuing with physical decoder validation");
                }
            } else {
                log("Tizen HEVC Main10 answer level-id: unavailable");
            }
            const auto colorSpaceId = gateway::videoExtensionId(
                sdp, gateway::WebRtcColorSpaceExtensionUri);
            if (colorSpaceId) {
                gateway::disableRtpColorSpace(session->videoRtpConfiguration);
                log("WebRTC RTP HDR color-space extension: NEGOTIATED BUT DISABLED for Tizen decoder compatibility, id="
                    + std::to_string(*colorSpaceId)
                    + "; relying on HEVC VUI/SEI metadata");
            } else {
                gateway::disableRtpColorSpace(session->videoRtpConfiguration);
                log("WebRTC RTP HDR color-space extension: NOT NEGOTIATED");
            }
        } else {
            gateway::disableRtpColorSpace(session->videoRtpConfiguration);
        }
        peerConnection->setRemoteDescription(rtc::Description(sdp, "answer"));

        std::vector<PendingCandidate> pendingCandidates;
        {
            const std::lock_guard lock(session->candidateMutex);
            session->hasRemoteDescription = true;
            pendingCandidates.swap(session->pendingCandidates);
        }

        log("Received SDP answer");

        for (const auto& candidate : pendingCandidates) {
            peerConnection->addRemoteCandidate(
                rtc::Candidate(candidate.candidate, candidate.mid));
        }
    }

    void handleCandidate(const std::shared_ptr<Session>& session,
                         std::uint64_t sessionId,
                         std::string candidate,
                         std::string mid)
    {
        if (!isCurrentSession(session, sessionId)) {
            log("Ignored ICE candidate for inactive session "
                + std::to_string(sessionId));
            return;
        }
        const auto peerConnection = session->peerConnection;
        if (!peerConnection) {
            return;
        }

        log("Received ICE candidate: " + candidate);

        {
            const std::lock_guard lock(session->candidateMutex);
            if (!session->hasRemoteDescription) {
                session->pendingCandidates.push_back(
                    {std::move(candidate), std::move(mid)});
                return;
            }
        }

        peerConnection->addRemoteCandidate(rtc::Candidate(std::move(candidate), std::move(mid)));
    }

    void sendJson(const std::shared_ptr<Session>& session, const Json& message)
    {
        const std::string payload = message.dump();
        const std::lock_guard lock(session->sendMutex);
        if (session->socket && session->socket->isOpen()) {
            if (payload.size() > MaxGatewayWebSocketMessageSize) {
                throw std::runtime_error("Gateway WebSocket message exceeds its configured limit");
            }
            session->socket->send(payload);
        }
    }

    void closeSession(const std::shared_ptr<Session>& session)
    {
        stopActiveSession(session, false);

        const std::lock_guard lock(sessionMutex_);
        if (activeSession_ == session) {
            activeSession_.reset();
        }
    }

    MediaSourceMode sourceMode_;
    gateway::moonlight::MoonlightSessionOptions moonlightOptions_;
    bool explicitMoonlightHost_ = false;
    mutable std::mutex moonlightOptionsMutex_;
    std::unique_ptr<gateway::moonlight::MoonlightIdentity> identity_;
    gateway::ApplicationArtworkCache artworkCache_;
    std::mutex applicationTitleMutex_;
    std::unordered_map<std::string, std::string> applicationTitles_;
    std::unique_ptr<moonlight::H264AnnexBReader> videoSource_;
    std::unique_ptr<moonlight::OpusFileReader> audioSource_;
    Logger logger_;
    std::mutex logMutex_;
    std::mutex sessionMutex_;
    std::shared_ptr<Session> activeSession_;
    std::mutex pairingMutex_;
    std::thread pairingThread_;
    bool pairingInProgress_ = false;
    gateway::managementipc::Result pairingResult_{false, "not-started", "No pairing operation is active"};
    std::atomic<std::uint64_t> nextSessionId_ = 1;
    rtc::WebSocketServer server_;

};

int runMoonlightPairing(const ProgramOptions& options)
{
    gateway::moonlight::MoonlightIdentity identity(
        gateway::moonlight::MoonlightIdentity::resolveStorageDirectory(
            options.dataDirectory,
            options.hostMode == ProgramHostMode::Service
                ? gateway::moonlight::MoonlightDataDirectoryMode::Service
                : gateway::moonlight::MoonlightDataDirectoryMode::Console));
    const auto logger = [](const std::string& message) {
        std::cout << message << std::endl;
    };
    logger("Moonlight identity path: " + identity.storageDirectory().string());

    auto detected = gateway::moonlight::MoonlightSession::detectSunshine(
        identity, options.host, logger);
    auto httpClient = std::make_unique<gateway::moonlight::SunshineHttpClient>(
        identity, detected.address);
    httpClient->setHttpsPort(detected.serverInfo.httpsPort);
    if (detected.pairedHost) {
        httpClient->setPinnedServerCertificate(
            detected.pairedHost->serverCertificatePem);
    }

    if (detected.pairedHost && detected.serverInfo.pairStatus == 1) {
        logger("Moonlight client already paired");
    } else {
        const std::string pin = gateway::moonlight::MoonlightPairing::generatePin();
        std::cout << "====================================\n"
                  << "MOONLIGHT PAIRING PIN: " << pin << '\n'
                  << "Enter this PIN in Sunshine.\n"
                  << "====================================" << std::endl;

        gateway::moonlight::MoonlightPairing pairing(identity, *httpClient);
        const auto outcome = pairing.pair(detected.serverInfo.appVersion, pin);
        if (outcome.result
            == gateway::moonlight::MoonlightPairing::Result::IncorrectPin) {
            throw std::runtime_error("Sunshine rejected the Moonlight pairing PIN");
        }
        if (outcome.result
            == gateway::moonlight::MoonlightPairing::Result::AlreadyInProgress) {
            throw std::runtime_error("Sunshine is already handling another pairing request");
        }
        if (outcome.result != gateway::moonlight::MoonlightPairing::Result::Paired) {
            throw std::runtime_error("Sunshine pairing failed");
        }

        gateway::moonlight::PairedSunshineHost host{
            detected.serverInfo.uniqueId,
            detected.serverInfo.hostname,
            detected.address,
            detected.serverInfo.httpsPort,
            outcome.serverCertificatePem,
        };
        identity.savePairedHost(host);
        detected.pairedHost = host;
        httpClient->setPinnedServerCertificate(outcome.serverCertificatePem);

        const auto pairedServerInfo = httpClient->getServerInfo(true, std::chrono::seconds(5));
        if (pairedServerInfo.pairStatus != 1) {
            throw std::runtime_error(
                "Sunshine pairing completed but /serverinfo does not report paired");
        }
        detected.serverInfo = pairedServerInfo;
        logger("Moonlight pairing completed");
        logger("Sunshine certificate pinned");
    }

    auto applications = httpClient->getAppList();
    for (const auto& application : applications) {
        logger("Sunshine application: " + application.title + " (ID "
               + application.id + ")");
    }
    const auto* selectedApplication =
        gateway::moonlight::SunshineHttpClient::findApplication(
            applications, options.application);
    if (!selectedApplication) {
        throw std::runtime_error(
            "Requested Sunshine application was not found: " + options.application);
    }
    logger("Selected Sunshine application: " + selectedApplication->title + " (ID "
           + selectedApplication->id + ")");
    return 0;
}

class RuntimeLogger {
public:
    explicit RuntimeLogger(const ProgramOptions& options)
        : console_(options.hostMode == ProgramHostMode::Console)
    {
        if (options.hostMode == ProgramHostMode::Service) {
            const auto logPath = *options.dataDirectory / "gateway-service.log";
            file_.open(logPath, std::ios::out | std::ios::app);
            if (!file_) {
                throw std::runtime_error("Unable to open service log: " + logPath.string());
            }
        }
    }

    void operator()(const std::string& message)
    {
        std::lock_guard lock(mutex_);
        if (console_) {
            std::cout << message << std::endl;
        }
        if (file_) {
            file_ << message << '\n';
            file_.flush();
        }
    }

private:
    bool console_ = false;
    std::ofstream file_;
    std::mutex mutex_;
};

int runGatewayRuntime(const ProgramOptions& options,
                      gateway::GatewayShutdownSignal& shutdown,
                      RuntimeLogger& logger,
                      const gateway::WindowsServiceHost::ReadyCallback& ready)
{
    SignalingServer server(options, std::ref(logger));
    std::unique_ptr<gateway::serviceipc::ServiceIpcServer> serviceIpc;
    std::unique_ptr<gateway::managementipc::ManagementIpcClient> managementIpc;
    if (options.hostMode == ProgramHostMode::Service) {
        serviceIpc = std::make_unique<gateway::serviceipc::ServiceIpcServer>(
            [&server] { return server.localServiceStatus(); });
        serviceIpc->start();
        logger("Local service IPC listening on \\\\.\\pipe\\MoonlightWebRTCGateway");
        managementIpc = std::make_unique<gateway::managementipc::ManagementIpcClient>(
            [&server](const gateway::managementipc::Command& command) {
                return server.handleManagementCommand(command);
            }, [&logger](const std::string& message) { logger(message); });
        managementIpc->start();
        logger("Management IPC client reconnecting to the interactive tray");
    }
    ready();
    server.wait(shutdown);
    if (serviceIpc) {
        serviceIpc->stop();
    }
    if (managementIpc) {
        managementIpc->stop();
    }
    logger("Gateway runtime stopped");
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const auto options = parseProgramOptions(argc, argv);
        if (options.migrationSourceDirectory) {
            const auto result = gateway::moonlight::MoonlightIdentity::migrateStorageDirectory(
                *options.migrationSourceDirectory, *options.dataDirectory);
            std::cout << (result == gateway::moonlight::MoonlightIdentityMigrationResult::Migrated
                              ? "Gateway data migration completed"
                              : "Gateway data destination is already authoritative")
                      << std::endl;
            return 0;
        }
        if (options.pair) {
            return runMoonlightPairing(options);
        }

        RuntimeLogger logger(options);
        if (options.hostMode == ProgramHostMode::Service) {
            logger((options.dataDirectoryExplicit
                        ? "Service data root selected from explicit --data-dir: "
                        : "Service data root selected from ProgramData default: ")
                   + options.dataDirectory->string());
        }
        gateway::GatewayShutdownSignal shutdown;
        const auto runtime = [&options, &shutdown, &logger](
                                 const gateway::WindowsServiceHost::ReadyCallback& ready) {
            return runGatewayRuntime(options, shutdown, logger, ready);
        };

        if (options.hostMode == ProgramHostMode::Service) {
            return gateway::WindowsServiceHost::run(
                L"MoonlightWebRTCGateway", shutdown, runtime);
        }

        std::signal(SIGINT, requestShutdown);
        std::signal(SIGTERM, requestShutdown);
        return runtime([] {});
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
