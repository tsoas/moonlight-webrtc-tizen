#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gateway::moonlight {

constexpr std::uint32_t GamepadProtocolVersion = 2;
constexpr std::size_t MaximumGamepads = 16;
constexpr auto MouseEmulationLongPressTime = std::chrono::milliseconds(750);
constexpr auto MouseEmulationPollingInterval = std::chrono::milliseconds(50);
constexpr double MouseEmulationMotionMultiplier = 4.0;
constexpr double MouseEmulationDeadzone = 2.0;

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
    std::function<int(std::int16_t, std::int16_t)> mouseMove;
    std::function<int(char, int)> mouseButton;
    std::function<int(std::int8_t)> scroll;
    std::function<int(std::int8_t)> horizontalScroll;
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
    using ControlSender = std::function<void(const std::string&)>;
    using Clock = std::chrono::steady_clock;

    MoonlightInputBridge(Logger logger, ControlSender controlSender);
    MoonlightInputBridge(Logger logger, MoonlightInputApi api, ControlSender controlSender);
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
    std::size_t controllerCount() const;
    std::uint16_t activeGamepadMask() const;
    std::optional<std::uint8_t> slotForController(std::uint32_t clientControllerId) const;
    std::optional<MoonlightInputDiagnostics> diagnostics(
        std::uint32_t clientControllerId) const;

    void handleRumble(std::uint16_t controllerNumber,
                      std::uint16_t lowFrequencyMotor,
                      std::uint16_t highFrequencyMotor);
    void handleTriggerRumble(std::uint16_t controllerNumber,
                             std::uint16_t leftTriggerMotor,
                             std::uint16_t rightTriggerMotor);

    static MoonlightInputApi makeMoonlightInputApi();
    static int mapStandardButtons(std::uint32_t protocolButtons);
    static std::uint8_t mapTrigger(double value);
    static std::int16_t mapStick(double value, bool invert);
    static MoonlightControllerState neutralControllerState();
    static std::uint32_t standardSupportedButtonFlags(std::size_t buttonCount);
    static std::uint8_t detectControllerType(std::string_view id);
    static double rumbleMagnitude(std::uint32_t value);
    static std::pair<std::int16_t, std::int16_t> mouseDelta(
        const MoonlightControllerState& state);

private:
    struct Controller {
        std::uint32_t clientControllerId = 0;
        std::uint8_t slot = 0;
        std::string id;
        std::string mapping;
        std::size_t buttonCount = 0;
        std::size_t axisCount = 0;
        std::uint8_t type = 0;
        std::uint16_t capabilities = 0;
        bool announced = false;
        MoonlightInputDiagnostics diagnostics;
        MoonlightControllerState latestState;
        std::uint32_t latestProtocolButtons = 0;
        std::uint32_t previousProtocolButtons = 0;
        std::optional<Clock::time_point> startPressedAt;
        bool mouseMode = false;
        std::uint32_t suppressedMouseButtons = 0;
        std::uint32_t simulatedMouseButtons = 0;
        double strongMagnitude = 0.0;
        double weakMagnitude = 0.0;
        double leftTriggerMagnitude = 0.0;
        double rightTriggerMagnitude = 0.0;
        std::optional<Clock::time_point> previousArrival;
        double interArrivalTotalMilliseconds = 0.0;
        std::uint64_t interArrivalSamples = 0;
        std::optional<Clock::time_point> rateWindowStart;
        std::uint64_t statesInRateWindow = 0;
    };

    std::optional<std::uint8_t> allocateSlotLocked() const;
    void announceControllerLocked(Controller& controller);
    void neutralizeAndRemoveLocked(Controller& controller);
    void releaseMouseButtonsLocked(Controller& controller);
    void removeControllerLocked(std::uint32_t clientControllerId);
    void clearControllersLocked();
    void updateDiagnosticsLocked(Controller& controller, Clock::time_point receivedAt);
    void queueControlMessageLocked(std::string message);
    void queueMouseModeStatusLocked(const Controller& controller);
    void queueRumbleStateLocked(const Controller& controller);
    void workerLoop(std::stop_token stopToken);
    void log(const std::string& message) const;

    Logger logger_;
    MoonlightInputApi api_;
    ControlSender controlSender_;
    mutable std::mutex mutex_;
    std::condition_variable workerCondition_;
    std::jthread worker_;
    std::vector<std::string> pendingControlMessages_;
    std::unordered_map<std::uint32_t, Controller> controllers_;
    std::array<std::optional<std::uint32_t>, MaximumGamepads> slotToController_;
    std::uint16_t activeGamepadMask_ = 0;
    bool moonlightSessionActive_ = false;
    bool shutdown_ = false;
};

} // namespace gateway::moonlight
