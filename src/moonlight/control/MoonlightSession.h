#pragma once

#include "media/MediaSender.h"
#include "moonlight/MoonlightMediaBridge.h"
#include "moonlight/control/MoonlightControlTypes.h"
#include "moonlight/control/MoonlightIdentity.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <Limelight.h>

namespace gateway::moonlight {

struct MoonlightSessionOptions {
    std::optional<std::string> host;
    std::string application = "Desktop";
};

struct DetectedSunshine {
    std::string address;
    SunshineServerInfo serverInfo;
    std::optional<PairedSunshineHost> pairedHost;
};

class ServerInformationStorage {
public:
    ServerInformationStorage(const SunshineServerInfo& serverInfo,
                             std::string address,
                             std::string rtspSessionUrl);

    ServerInformationStorage(const ServerInformationStorage&) = delete;
    ServerInformationStorage& operator=(const ServerInformationStorage&) = delete;
    ServerInformationStorage(ServerInformationStorage&&) = delete;
    ServerInformationStorage& operator=(ServerInformationStorage&&) = delete;

    SERVER_INFORMATION* get();

private:
    void refreshPointers();

    SERVER_INFORMATION value_{};
    std::string address_;
    std::string appVersion_;
    std::string gfeVersion_;
    std::string rtspSessionUrl_;
};

class MoonlightSession {
public:
    using Logger = std::function<void(const std::string&)>;
    using TerminationHandler = std::function<void()>;

    MoonlightSession(MediaSender& sender,
                     MoonlightIdentity& identity,
                     MoonlightSessionOptions options,
                     Logger logger,
                     TerminationHandler terminationHandler);
    ~MoonlightSession();

    MoonlightSession(const MoonlightSession&) = delete;
    MoonlightSession& operator=(const MoonlightSession&) = delete;

    static DetectedSunshine detectSunshine(MoonlightIdentity& identity,
                                           const std::optional<std::string>& requestedHost,
                                           const Logger& logger);
    static STREAM_CONFIGURATION createStreamConfiguration();

    void start();
    void stop();
    void onWebRtcKeyframeRequest();

private:
    enum class State {
        Idle,
        Starting,
        Connected,
        Stopped,
    };

    static void stageStartingCallback(int stage);
    static void stageCompleteCallback(int stage);
    static void stageFailedCallback(int stage, int errorCode);
    static void connectionStartedCallback();
    static void connectionTerminatedCallback(int errorCode);
    static void logMessageCallback(const char* format, ...);
    static void connectionStatusCallback(int status);

    void configureConnectionCallbacks();
    void log(const std::string& message) const;

    static std::atomic<MoonlightSession*> activeSession_;

    MediaSender& sender_;
    MoonlightIdentity& identity_;
    MoonlightSessionOptions options_;
    Logger logger_;
    TerminationHandler terminationHandler_;
    std::atomic<State> state_ = State::Idle;
    CONNECTION_LISTENER_CALLBACKS connectionCallbacks_{};
    std::unique_ptr<MoonlightMediaBridge> mediaBridge_;
    STREAM_CONFIGURATION streamConfiguration_{};
    std::unique_ptr<ServerInformationStorage> serverInformation_;
};

} // namespace gateway::moonlight
