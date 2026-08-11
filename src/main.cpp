#include "H264AnnexBReader.h"
#include "OpusFileReader.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
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
constexpr auto VideoSamplePath = "samples/test-720p60.h264";
constexpr auto AudioSamplePath = "samples/test-tone-48k-stereo.opus";
constexpr auto RtpCname = "moonlight-webrtc";
constexpr auto MediaStreamId = "stream1";
constexpr std::string_view SamsungGameModeImageAttribute =
    "imageattr:96 send [x=[1280:1280],y=[720:720],fps=[60:60]]";
constexpr std::string_view SamsungGameModeSdpLine =
    "a=imageattr:96 send [x=[1280:1280],y=[720:720],fps=[60:60]]";

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
    SignalingServer()
        : videoSource_(VideoSamplePath)
        , audioSource_(AudioSamplePath)
        , server_(makeServerConfiguration())
    {
        server_.onClient([this](std::shared_ptr<rtc::WebSocket> socket) {
            acceptClient(std::move(socket));
        });

        {
            std::ostringstream message;
            message << "Loaded H.264 sample: " << videoSource_.accessUnits().size()
                    << " access units";
            log(message.str());
        }
        {
            std::ostringstream message;
            message << "Loaded Opus sample: " << audioSource_.packets().size()
                    << " encoded packets";
            log(message.str());
        }
        log("WebSocket signaling server listening on 0.0.0.0:8000");
    }

    void wait()
    {
        std::unique_lock lock(waitMutex_);
        waitCondition_.wait(lock);
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
        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence, rtpConfiguration);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtpConfiguration));
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        packetizer->addToChain(std::make_shared<rtc::PliHandler>([this, weakSession] {
            if (const auto currentSession = weakSession.lock()) {
                currentSession->videoKeyframeRequested.store(true, std::memory_order_relaxed);
                const auto requestNumber = currentSession->keyframeRequestCount.fetch_add(
                                               1, std::memory_order_relaxed)
                    + 1;
                log("RTCP PLI/FIR received: H.264 IDR requested (#"
                    + std::to_string(requestNumber) + ")");
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

        session->streamingThread = std::thread([this, sessionPointer = session.get()] {
            streamMedia(*sessionPointer);
        });

        peerConnection->setLocalDescription();
    }

    void streamMedia(Session& session)
    {
        {
            std::unique_lock lock(session.streamMutex);
            session.streamCondition.wait(lock, [&session] {
                return session.stopRequested
                    || (session.peerConnected && session.videoTrackOpen
                        && session.audioTrackOpen);
            });

            if (session.stopRequested) {
                return;
            }
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
                        const auto idrIndex = videoSource_.firstIdrAccessUnitIndex();
                        if (idrIndex) {
                            videoFrameIndex = *idrIndex;
                            idrScheduled = true;
                        }
                    }

                    const auto& accessUnit = videoSource_.accessUnits()[videoFrameIndex];
                    session.videoTrack->sendFrame(
                        reinterpret_cast<const rtc::byte*>(accessUnit.data()),
                        accessUnit.size(),
                        videoTimestamp);

                    ++videoFramesSent;
                    videoBytesSent += accessUnit.size();
                    ++videoFrameIndex;

                    if (idrScheduled) {
                        log("H.264 IDR sent in response to RTCP PLI/FIR");
                    } else if (keyframeRequested) {
                        log("RTCP PLI/FIR response unavailable: sample has no H.264 IDR");
                    }

                    if (videoFrameIndex == videoSource_.accessUnits().size()) {
                        videoFrameIndex = 0;
                        log("Video loop restarted");
                    }
                } else {
                    const auto& packet = audioSource_.packets()[audioPacketIndex];
                    session.audioTrack->sendFrame(
                        reinterpret_cast<const rtc::byte*>(packet.data()),
                        packet.size(),
                        audioTimestamp);

                    ++audioPacketsSent;
                    audioBytesSent += packet.size();
                    audioElapsed += audioSource_.packetDuration(audioPacketIndex);
                    ++audioPacketIndex;

                    if (audioPacketIndex == audioSource_.packets().size()) {
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

    moonlight::H264AnnexBReader videoSource_;
    moonlight::OpusFileReader audioSource_;
    std::mutex logMutex_;
    std::mutex sessionMutex_;
    std::shared_ptr<Session> activeSession_;
    rtc::WebSocketServer server_;

    std::mutex waitMutex_;
    std::condition_variable waitCondition_;
};

} // namespace

int main()
{
    try {
        SignalingServer server;
        server.wait();
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
