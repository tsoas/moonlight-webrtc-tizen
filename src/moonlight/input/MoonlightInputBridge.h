#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace gateway::moonlight {

constexpr std::uint32_t GamepadProtocolVersion = 1;

enum class GamepadProtocolButton : std::uint32_t {
    A = 1U << 0,
    B = 1U << 1,
    X = 1U << 2,
    Y = 1U << 3,
    LeftBumper = 1U << 4,
    RightBumper = 1U << 5,
    Back = 1U << 6,
    Start = 1U << 7,
    LeftStick = 1U << 8,
    RightStick = 1U << 9,
    DpadUp = 1U << 10,
    DpadDown = 1U << 11,
    DpadLeft = 1U << 12,
    DpadRight = 1U << 13,
    Guide = 1U << 14,
};

constexpr std::uint32_t protocolButtonMask(GamepadProtocolButton button)
{
    return static_cast<std::uint32_t>(button);
}

struct MoonlightControllerState {
    int buttonFlags = 0;
    std::uint8_t leftTrigger = 0;
    std::uint8_t rightTrigger = 0;
    std::int16_t leftStickX = 0;
    std::int16_t leftStickY = 0;
    std::int16_t rightStickX = 0;
    std::int16_t rightStickY = 0;
};

struct MoonlightInputApi {
    std::function<int(std::uint8_t, std::uint16_t, std::uint8_t,
                      std::uint32_t, std::uint16_t)>
        controllerArrival;
    std::function<int(std::int16_t, std::int16_t, int,
                      std::uint8_t, std::uint8_t,
                      std::int16_t, std::int16_t,
                      std::int16_t, std::int16_t)>
        controllerState;
};

struct MoonlightInputDiagnostics {
    std::uint64_t receivedMessages = 0;
    std::uint64_t acceptedStates = 0;
    std::uint64_t malformedMessages = 0;
    std::uint64_t sequenceGaps = 0;
    std::uint64_t staleStates = 0;
    std::optional<std::uint64_t> lastSequence;
    double statesPerSecond = 0.0;
    double averageInterArrivalMilliseconds = 0.0;
};

class MoonlightInputBridge {
public:
    using Logger = std::function<void(const std::string&)>;
    using Clock = std::chrono::steady_clock;

    explicit MoonlightInputBridge(Logger logger);
    MoonlightInputBridge(Logger logger, MoonlightInputApi api);
    ~MoonlightInputBridge();

    MoonlightInputBridge(const MoonlightInputBridge&) = delete;
    MoonlightInputBridge& operator=(const MoonlightInputBridge&) = delete;

    bool handleControlMessage(std::string_view text);
    bool handleGamepadMessage(std::string_view text);
    bool handleGamepadMessage(std::string_view text, Clock::time_point receivedAt);

    void setMoonlightSessionActive(bool active);
    void onTransportClosed();
    void shutdown();

    bool controllerConnected() const;
    MoonlightInputDiagnostics diagnostics() const;

    static MoonlightInputApi makeMoonlightInputApi();
    static int mapStandardButtons(std::uint32_t protocolButtons);
    static std::uint8_t mapTrigger(double value);
    static std::int16_t mapStick(double value, bool invert);
    static MoonlightControllerState neutralControllerState();
    static std::uint32_t standardSupportedButtonFlags(std::size_t buttonCount);

private:
    void announceControllerLocked();
    void neutralizeAndRemoveLocked();
    void resetSequenceLocked();
    void log(const std::string& message) const;

    Logger logger_;
    MoonlightInputApi api_;
    mutable std::mutex mutex_;
    bool moonlightSessionActive_ = false;
    bool controllerConnected_ = false;
    bool controllerAnnounced_ = false;
    std::string controllerId_;
    std::string controllerMapping_;
    std::size_t buttonCount_ = 0;
    std::size_t axisCount_ = 0;
    MoonlightInputDiagnostics diagnostics_;
    std::optional<Clock::time_point> previousArrival_;
    double interArrivalTotalMilliseconds_ = 0.0;
    std::uint64_t interArrivalSamples_ = 0;
    std::optional<Clock::time_point> rateWindowStart_;
    std::uint64_t statesInRateWindow_ = 0;
    bool shutdown_ = false;
};

} // namespace gateway::moonlight
