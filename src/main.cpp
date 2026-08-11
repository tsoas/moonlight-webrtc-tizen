#include "H264AnnexBReader.h"
#include "OpusFileReader.h"
#include "media/MediaSender.h"
#include "moonlight/MoonlightMediaBridge.h"
#include "moonlight/control/MoonlightIdentity.h"
#include "moonlight/control/MoonlightPairing.h"
#include "moonlight/control/MoonlightSession.h"
#include "moonlight/control/SunshineHttpClient.h"
#include "webrtc/WebRtcMediaSender.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
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
constexpr std::string_view SamsungGameModeImageAttribute =
    "imageattr:96 send [x=[1280:1280],y=[720:720],fps=[60:60]]";
constexpr std::string_view SamsungGameModeSdpLine =
    "a=imageattr:96 send [x=[1280:1280],y=[720:720],fps=[60:60]]";
volatile std::sig_atomic_t ShutdownRequested = 0;

void requestShutdown(int)
{
    ShutdownRequested = 1;
}

bool hasValidSamsungGameModeImageAttribute(std::string_view sdp)
{
    for (std::size_t index = 0; index < sdp.size(); ++index) {
        if ((sdp[index] == '\n' && (index == 0 || sdp[index - 1] != '\r'))
            || (sdp[index] == '\r'
                && (index + 1 == sdp.size() || sdp[index + 1] != '\n'))) {
            return false;
        }
    }

    const auto attributePosition = sdp.find(SamsungGameModeSdpLine);
    if (attributePosition == std::string_view::npos
        || sdp.find(SamsungGameModeSdpLine,
                    attributePosition + SamsungGameModeSdpLine.size())
            != std::string_view::npos
        || sdp.find("a=imageattr:") != attributePosition
        || sdp.find("a=imageattr:", attributePosition + 1) != std::string_view::npos) {
        return false;
    }

    const bool completeLine =
        (attributePosition == 0
         || sdp.substr(attributePosition - 2, 2) == "\r\n")
        && sdp.substr(attributePosition + SamsungGameModeSdpLine.size(), 2) == "\r\n";
    const auto videoPosition = sdp.find("m=video ");
    const auto nextMediaPosition = videoPosition == std::string_view::npos
        ? std::string_view::npos
        : sdp.find("\r\nm=", videoPosition + 1);

    return completeLine && videoPosition != std::string_view::npos
        && attributePosition > videoPosition
        && (nextMediaPosition == std::string_view::npos
            || attributePosition < nextMediaPosition);
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

enum class MediaSourceMode {
    Test,
    Moonlight,
};

struct ProgramOptions {
    MediaSourceMode sourceMode = MediaSourceMode::Test;
    std::optional<std::string> host;
    std::string application = "Desktop";
    bool pair = false;
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
        } else {
            throw std::invalid_argument(
                "Usage: moonlight_webrtc [--source=test|--source=moonlight] "
                "[--host=<host>] [--app=<name>] [--pair]");
        }
    }

    if (options.sourceMode != MediaSourceMode::Moonlight
        && (options.host || options.application != "Desktop" || options.pair)) {
        throw std::invalid_argument(
            "--host, --app, and --pair require --source=moonlight");
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
    std::shared_ptr<gateway::WebRtcMediaSender> mediaSender;
    std::shared_ptr<gateway::moonlight::MoonlightSession> moonlightSession;
    std::uint32_t videoRtpStartTimestamp = 0;
    std::uint32_t audioRtpStartTimestamp = 0;

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
    std::atomic<bool> videoKeyframeRequested = false;
    std::atomic<std::uint64_t> keyframeRequestCount = 0;
    std::thread streamingThread;
};

class SignalingServer {
public:
    explicit SignalingServer(const ProgramOptions& options)
        : sourceMode_(options.sourceMode)
        , moonlightOptions_{options.host, options.application}
        , identity_(sourceMode_ == MediaSourceMode::Moonlight
                        ? std::make_unique<gateway::moonlight::MoonlightIdentity>()
                        : nullptr)
        , server_(makeServerConfiguration())
    {
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
            log("Moonlight source selected");
            log("Moonlight identity path: " + identity_->storageDirectory().string());
        }
        log("WebSocket signaling server listening on 0.0.0.0:8000");
    }

    ~SignalingServer()
    {
        std::shared_ptr<Session> session;
        {
            const std::lock_guard lock(sessionMutex_);
            session = std::exchange(activeSession_, nullptr);
        }
        if (session) {
            session->requestStreamingStop();
            session->stopStreaming();
            if (session->peerConnection) {
                session->peerConnection->close();
            }
        }
    }

    void wait()
    {
        std::unique_lock lock(waitMutex_);
        while (!ShutdownRequested) {
            waitCondition_.wait_for(lock, std::chrono::milliseconds(200));
        }
    }

private:
    static rtc::WebSocketServer::Configuration makeServerConfiguration()
    {
        rtc::WebSocketServer::Configuration configuration;
        configuration.port = 8000;
        configuration.enableTls = false;
        configuration.bindAddress = "0.0.0.0";
        return configuration;
    }

    void log(const std::string& message)
    {
        const std::lock_guard lock(logMutex_);
        std::cout << message << std::endl;
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
                createPeerConnection(currentSession);
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

    void createPeerConnection(const std::shared_ptr<Session>& session)
    {
        if (session->peerConnection) {
            return;
        }

        rtc::Configuration configuration;
        auto peerConnection = std::make_shared<rtc::PeerConnection>(configuration);
        session->peerConnection = peerConnection;

        const std::weak_ptr<Session> weakSession = session;

        peerConnection->onStateChange(
            [this, weakSession](rtc::PeerConnection::State state) {
                std::ostringstream message;
                message << "PeerConnection state: " << state;
                log(message.str());

                if (const auto currentSession = weakSession.lock()) {
                    {
                        const std::lock_guard lock(currentSession->streamMutex);
                        currentSession->peerConnected =
                            state == rtc::PeerConnection::State::Connected;

                        if (state == rtc::PeerConnection::State::Disconnected
                            || state == rtc::PeerConnection::State::Failed
                            || state == rtc::PeerConnection::State::Closed) {
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
            [this, weakSession](rtc::Description description) {
                if (description.type() != rtc::Description::Type::Offer) {
                    return;
                }

                if (const auto currentSession = weakSession.lock()) {
                    const std::string sdp = std::string(description);
                    if (!hasValidSamsungGameModeImageAttribute(sdp)) {
                        log("Samsung Game Mode SDP imageattr validation failed");
                        currentSession->requestStreamingStop();
                        currentSession->socket->close();
                        return;
                    }

                    log("Samsung Game Mode SDP imageattr enabled");
                    log("Outgoing SDP m=video section:\n"
                        + std::string(videoMediaSection(sdp)));
                    sendJson(currentSession,
                             {{"type", "offer"}, {"sdp", sdp}});
                    log("Sent SDP offer");
                }
            });

        peerConnection->onLocalCandidate([this, weakSession](rtc::Candidate candidate) {
            if (const auto currentSession = weakSession.lock()) {
                const std::string candidateText = candidate.candidate();
                sendJson(currentSession,
                         {{"type", "candidate"},
                          {"candidate", candidateText},
                          {"mid", candidate.mid()}});
                log("Sent ICE candidate: " + candidateText);
            }
        });

        rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        video.addH264Codec(VideoPayloadType);
        video.addAttribute(std::string(SamsungGameModeImageAttribute));
        video.addSSRC(VideoSsrc, RtpCname, MediaStreamId, "video");

        session->videoTrack = peerConnection->addTrack(video);

        auto rtpConfiguration = std::make_shared<rtc::RtpPacketizationConfig>(
            VideoSsrc,
            RtpCname,
            VideoPayloadType,
            rtc::H264RtpPacketizer::ClockRate);
        session->videoRtpStartTimestamp = rtpConfiguration->startTimestamp;
        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence, rtpConfiguration);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtpConfiguration));
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        packetizer->addToChain(std::make_shared<rtc::PliHandler>([this, weakSession] {
            if (const auto currentSession = weakSession.lock()) {
                const auto requestNumber = currentSession->keyframeRequestCount.fetch_add(
                                               1, std::memory_order_relaxed)
                    + 1;
                log("RTCP PLI/FIR received: H.264 IDR requested (#"
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

        session->videoTrack->onOpen([this, weakSession] {
            if (const auto currentSession = weakSession.lock()) {
                {
                    const std::lock_guard lock(currentSession->streamMutex);
                    currentSession->videoTrackOpen = true;
                }
                log("Video track opened");
                currentSession->streamCondition.notify_all();
            }
        });

        session->videoTrack->onClosed([weakSession] {
            if (const auto currentSession = weakSession.lock()) {
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

        session->audioTrack->onOpen([this, weakSession] {
            if (const auto currentSession = weakSession.lock()) {
                {
                    const std::lock_guard lock(currentSession->streamMutex);
                    currentSession->audioTrackOpen = true;
                }
                log("Audio track opened");
                currentSession->streamCondition.notify_all();
            }
        });

        session->audioTrack->onClosed([weakSession] {
            if (const auto currentSession = weakSession.lock()) {
                currentSession->requestStreamingStop();
            }
        });

        session->mediaSender = std::make_shared<gateway::WebRtcMediaSender>(
            session->videoTrack, session->audioTrack);

        session->streamingThread = std::thread([this, sessionPointer = session.get()] {
            if (sourceMode_ == MediaSourceMode::Moonlight) {
                streamMoonlightMedia(*sessionPointer);
            } else {
                streamTestMedia(*sessionPointer);
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

    void streamMoonlightMedia(Session& session)
    {
        if (!waitForMediaTracks(session)) {
            return;
        }

        auto moonlightSession = std::make_shared<gateway::moonlight::MoonlightSession>(
            *session.mediaSender,
            *identity_,
            moonlightOptions_,
            [this](const std::string& message) { log(message); },
            [&session] { session.requestStreamingStop(); });
        {
            const std::lock_guard lock(session.streamMutex);
            session.moonlightSession = moonlightSession;
        }

        try {
            moonlightSession->start();
            std::unique_lock lock(session.streamMutex);
            session.streamCondition.wait(lock, [&session] { return session.stopRequested; });
        } catch (const std::exception& error) {
            log("Moonlight streaming error: " + std::string(error.what()));
            session.requestStreamingStop();
        }

        moonlightSession->stop();
        {
            const std::lock_guard lock(session.streamMutex);
            session.moonlightSession.reset();
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
                    session.mediaSender->sendH264AccessUnit(accessUnit, rtpTimestamp);

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

    void handleMessage(const std::shared_ptr<Session>& session, const std::string& text)
    {
        try {
            const Json message = Json::parse(text);
            const std::string type = message.at("type").get<std::string>();

            if (type == "answer") {
                handleAnswer(session, message.at("sdp").get<std::string>());
            } else if (type == "candidate") {
                handleCandidate(session,
                                message.at("candidate").get<std::string>(),
                                message.at("mid").get<std::string>());
            }
        } catch (const std::exception& error) {
            log("Signaling error: " + std::string(error.what()));
        }
    }

    void handleAnswer(const std::shared_ptr<Session>& session, const std::string& sdp)
    {
        const auto peerConnection = session->peerConnection;
        if (!peerConnection) {
            return;
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
                         std::string candidate,
                         std::string mid)
    {
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
        const std::lock_guard lock(session->sendMutex);
        if (session->socket && session->socket->isOpen()) {
            session->socket->send(message.dump());
        }
    }

    void closeSession(const std::shared_ptr<Session>& session)
    {
        session->stopStreaming();

        if (session->peerConnection) {
            session->peerConnection->close();
        }

        const std::lock_guard lock(sessionMutex_);
        if (activeSession_ == session) {
            activeSession_.reset();
        }
    }

    MediaSourceMode sourceMode_;
    gateway::moonlight::MoonlightSessionOptions moonlightOptions_;
    std::unique_ptr<gateway::moonlight::MoonlightIdentity> identity_;
    std::unique_ptr<moonlight::H264AnnexBReader> videoSource_;
    std::unique_ptr<moonlight::OpusFileReader> audioSource_;
    std::mutex logMutex_;
    std::mutex sessionMutex_;
    std::shared_ptr<Session> activeSession_;
    rtc::WebSocketServer server_;

    std::mutex waitMutex_;
    std::condition_variable waitCondition_;
};

int runMoonlightPairing(const ProgramOptions& options)
{
    gateway::moonlight::MoonlightIdentity identity;
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
               + std::to_string(application.id) + ")");
    }
    const auto* selectedApplication =
        gateway::moonlight::SunshineHttpClient::findApplication(
            applications, options.application);
    if (!selectedApplication) {
        throw std::runtime_error(
            "Requested Sunshine application was not found: " + options.application);
    }
    logger("Selected Sunshine application: " + selectedApplication->title + " (ID "
           + std::to_string(selectedApplication->id) + ")");
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const auto options = parseProgramOptions(argc, argv);
        if (options.pair) {
            return runMoonlightPairing(options);
        }

        std::signal(SIGINT, requestShutdown);
        std::signal(SIGTERM, requestShutdown);
        SignalingServer server(options);
        server.wait();
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
