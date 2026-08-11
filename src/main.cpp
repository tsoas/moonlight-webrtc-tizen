#include "H264AnnexBReader.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

namespace {

using Json = nlohmann::json;

constexpr std::uint8_t VideoPayloadType = 96;
constexpr rtc::SSRC VideoSsrc = 42;
constexpr double VideoFrameRate = 30.0;
constexpr auto VideoSamplePath = "samples/test-720p30.h264";

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

    std::mutex sendMutex;
    std::mutex candidateMutex;
    bool hasRemoteDescription = false;
    std::vector<PendingCandidate> pendingCandidates;

    std::mutex streamMutex;
    std::condition_variable streamCondition;
    bool peerConnected = false;
    bool trackOpen = false;
    bool stopRequested = false;
    std::thread streamingThread;
};

class SignalingServer {
public:
    SignalingServer()
        : videoSource_(VideoSamplePath)
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
                    sendJson(currentSession,
                             {{"type", "offer"}, {"sdp", std::string(description)}});
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
        video.addSSRC(VideoSsrc, "moonlight-webrtc", "moonlight-test", "video");

        session->videoTrack = peerConnection->addTrack(video);

        auto rtpConfiguration = std::make_shared<rtc::RtpPacketizationConfig>(
            VideoSsrc,
            "moonlight-webrtc",
            VideoPayloadType,
            rtc::H264RtpPacketizer::ClockRate);
        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence, rtpConfiguration);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtpConfiguration));
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        session->videoTrack->setMediaHandler(packetizer);

        session->videoTrack->onOpen([this, weakSession] {
            if (const auto currentSession = weakSession.lock()) {
                {
                    const std::lock_guard lock(currentSession->streamMutex);
                    currentSession->trackOpen = true;
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

        session->streamingThread = std::thread([this, sessionPointer = session.get()] {
            streamVideo(*sessionPointer);
        });

        peerConnection->setLocalDescription();
    }

    void streamVideo(Session& session)
    {
        {
            std::unique_lock lock(session.streamMutex);
            session.streamCondition.wait(lock, [&session] {
                return session.stopRequested || (session.peerConnected && session.trackOpen);
            });

            if (session.stopRequested) {
                return;
            }
        }

        log("Video streaming started");

        using Clock = std::chrono::steady_clock;
        const auto streamStart = Clock::now();
        auto nextReport = streamStart + std::chrono::seconds(1);
        std::uint64_t framesSent = 0;
        std::uint64_t bytesSent = 0;
        std::size_t frameIndex = 0;

        while (true) {
            {
                const std::lock_guard lock(session.streamMutex);
                if (session.stopRequested || !session.peerConnected || !session.trackOpen) {
                    break;
                }
            }

            const auto& accessUnit = videoSource_.accessUnits()[frameIndex];
            try {
                session.videoTrack->sendFrame(
                    reinterpret_cast<const rtc::byte*>(accessUnit.data()),
                    accessUnit.size(),
                    std::chrono::duration<double>(
                        static_cast<double>(framesSent) / VideoFrameRate));
            } catch (const std::exception& error) {
                log("Video streaming error: " + std::string(error.what()));
                session.requestStreamingStop();
                break;
            }

            ++framesSent;
            bytesSent += accessUnit.size();
            ++frameIndex;

            if (frameIndex == videoSource_.accessUnits().size()) {
                frameIndex = 0;
                log("Video loop restarted");
            }

            const auto now = Clock::now();
            if (now >= nextReport) {
                std::ostringstream message;
                message << "Video streaming: " << framesSent << " frames sent, " << bytesSent
                        << " bytes";
                log(message.str());

                do {
                    nextReport += std::chrono::seconds(1);
                } while (nextReport <= now);
            }

            const auto nextFrameTime = streamStart
                + std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(
                        static_cast<double>(framesSent) / VideoFrameRate));

            std::unique_lock lock(session.streamMutex);
            session.streamCondition.wait_until(lock, nextFrameTime, [&session] {
                return session.stopRequested || !session.peerConnected || !session.trackOpen;
            });
        }

        log("Video streaming stopped");
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
