#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

namespace {

using Json = nlohmann::json;

struct PendingCandidate {
    std::string candidate;
    std::string mid;
};

struct Session {
    std::shared_ptr<rtc::WebSocket> socket;
    std::shared_ptr<rtc::PeerConnection> peerConnection;
    std::shared_ptr<rtc::Track> videoTrack;

    std::mutex sendMutex;
    std::mutex candidateMutex;
    bool hasRemoteDescription = false;
    std::vector<PendingCandidate> pendingCandidates;
};

class SignalingServer {
public:
    SignalingServer()
        : server_(makeServerConfiguration())
    {
        server_.onClient([this](std::shared_ptr<rtc::WebSocket> socket) {
            acceptClient(std::move(socket));
        });

        log("WebSocket signaling server listening on ws://127.0.0.1:8000");
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
        configuration.bindAddress = "127.0.0.1";
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

        peerConnection->onStateChange([this](rtc::PeerConnection::State state) {
            std::ostringstream message;
            message << "PeerConnection state: " << state;
            log(message.str());
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
        video.addH264Codec(96);
        session->videoTrack = peerConnection->addTrack(video);

        peerConnection->setLocalDescription();
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
        if (session->peerConnection) {
            session->peerConnection->close();
        }

        const std::lock_guard lock(sessionMutex_);
        if (activeSession_ == session) {
            activeSession_.reset();
        }
    }

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
