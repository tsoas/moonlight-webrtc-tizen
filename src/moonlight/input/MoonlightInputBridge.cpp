#include "moonlight/input/MoonlightInputBridge.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include <Limelight.h>
#include <nlohmann/json.hpp>

namespace gateway::moonlight {
namespace {

using Json = nlohmann::json;

constexpr std::uint16_t ControllerZeroMask = 0x0001;
constexpr std::uint32_t AllProtocolButtons = (1U << 15) - 1;

bool hasProtocolEnvelope(const Json& message, std::string_view expectedType)
{
    return message.is_object()
        && message.contains("v")
        && message["v"].is_number_unsigned()
        && message["v"].get<std::uint32_t>() == GamepadProtocolVersion
        && message.contains("type")
        && message["type"].is_string()
        && message["type"].get<std::string_view>() == expectedType;
}

bool readBoundedSize(const Json& message,
                     const char* name,
                     std::size_t maximum,
                     std::size_t& output)
{
    if (!message.contains(name) || !message[name].is_number_unsigned()) {
        return false;
    }
    const auto value = message[name].get<std::uint64_t>();
    if (value > maximum) {
        return false;
    }
    output = static_cast<std::size_t>(value);
    return true;
}

bool readFiniteNumber(const Json& message, const char* name, double& output)
{
    if (!message.contains(name) || !message[name].is_number()) {
        return false;
    }
    output = message[name].get<double>();
    return std::isfinite(output);
}

} // namespace

MoonlightInputBridge::MoonlightInputBridge(Logger logger)
    : MoonlightInputBridge(std::move(logger), makeMoonlightInputApi())
{
}

MoonlightInputBridge::MoonlightInputBridge(Logger logger, MoonlightInputApi api)
    : logger_(std::move(logger))
    , api_(std::move(api))
{
    if (!api_.controllerArrival || !api_.controllerState) {
        throw std::invalid_argument("Moonlight input API callbacks are required");
    }
}

MoonlightInputBridge::~MoonlightInputBridge()
{
    shutdown();
}

bool MoonlightInputBridge::handleControlMessage(std::string_view text)
{
    try {
        const Json message = Json::parse(text.begin(), text.end());
        if (hasProtocolEnvelope(message, "gamepad-connected")) {
            if (!message.contains("index") || !message["index"].is_number_unsigned()
                || message["index"].get<std::uint64_t>() > 255
                || !message.contains("id") || !message["id"].is_string()
                || !message.contains("mapping") || !message["mapping"].is_string()) {
                return false;
            }

            const std::string id = message["id"].get<std::string>();
            const std::string mapping = message["mapping"].get<std::string>();
            std::size_t buttonCount = 0;
            std::size_t axisCount = 0;
            if (id.size() > 512 || mapping.size() > 128
                || !readBoundedSize(message, "buttons", 64, buttonCount)
                || !readBoundedSize(message, "axes", 16, axisCount)) {
                return false;
            }

            const std::lock_guard lock(mutex_);
            if (shutdown_) {
                return false;
            }
            if (controllerAnnounced_) {
                neutralizeAndRemoveLocked();
            }
            controllerConnected_ = true;
            controllerId_ = id;
            controllerMapping_ = mapping;
            buttonCount_ = buttonCount;
            axisCount_ = axisCount;
            resetSequenceLocked();
            log("Gamepad connected: " + controllerId_);
            announceControllerLocked();
            return true;
        }

        if (hasProtocolEnvelope(message, "gamepad-disconnected")) {
            const std::lock_guard lock(mutex_);
            if (shutdown_) {
                return false;
            }
            const bool wasConnected = controllerConnected_;
            neutralizeAndRemoveLocked();
            controllerConnected_ = false;
            controllerId_.clear();
            controllerMapping_.clear();
            buttonCount_ = 0;
            axisCount_ = 0;
            resetSequenceLocked();
            if (wasConnected) {
                log("Gamepad disconnected");
            }
            return true;
        }
    } catch (const Json::exception&) {
        return false;
    }
    return false;
}

bool MoonlightInputBridge::handleGamepadMessage(std::string_view text)
{
    return handleGamepadMessage(text, Clock::now());
}

bool MoonlightInputBridge::handleGamepadMessage(std::string_view text,
                                                Clock::time_point receivedAt)
{
    std::uint64_t sequence = 0;
    std::uint32_t protocolButtons = 0;
    double clientTimestamp = 0.0;
    double leftTrigger = 0.0;
    double rightTrigger = 0.0;
    double leftStickX = 0.0;
    double leftStickY = 0.0;
    double rightStickX = 0.0;
    double rightStickY = 0.0;

    {
        const std::lock_guard lock(mutex_);
        ++diagnostics_.receivedMessages;
    }

    try {
        const Json message = Json::parse(text.begin(), text.end());
        if (!hasProtocolEnvelope(message, "gamepad-state")
            || !message.contains("seq") || !message["seq"].is_number_unsigned()
            || !message.contains("buttons") || !message["buttons"].is_number_unsigned()) {
            const std::lock_guard lock(mutex_);
            ++diagnostics_.malformedMessages;
            return false;
        }
        sequence = message["seq"].get<std::uint64_t>();
        const auto buttons = message["buttons"].get<std::uint64_t>();
        if (buttons > AllProtocolButtons
            || !readFiniteNumber(message, "timestampMs", clientTimestamp)
            || clientTimestamp < 0.0
            || !readFiniteNumber(message, "leftTrigger", leftTrigger)
            || !readFiniteNumber(message, "rightTrigger", rightTrigger)
            || !readFiniteNumber(message, "leftStickX", leftStickX)
            || !readFiniteNumber(message, "leftStickY", leftStickY)
            || !readFiniteNumber(message, "rightStickX", rightStickX)
            || !readFiniteNumber(message, "rightStickY", rightStickY)) {
            const std::lock_guard lock(mutex_);
            ++diagnostics_.malformedMessages;
            return false;
        }
        protocolButtons = static_cast<std::uint32_t>(buttons);
    } catch (const Json::exception&) {
        const std::lock_guard lock(mutex_);
        ++diagnostics_.malformedMessages;
        return false;
    }

    MoonlightControllerState state;
    state.buttonFlags = mapStandardButtons(protocolButtons);
    state.leftTrigger = mapTrigger(leftTrigger);
    state.rightTrigger = mapTrigger(rightTrigger);
    state.leftStickX = mapStick(leftStickX, false);
    state.leftStickY = mapStick(leftStickY, true);
    state.rightStickX = mapStick(rightStickX, false);
    state.rightStickY = mapStick(rightStickY, true);

    const std::lock_guard lock(mutex_);
    if (shutdown_ || !controllerConnected_) {
        ++diagnostics_.malformedMessages;
        return false;
    }
    if (diagnostics_.lastSequence && sequence <= *diagnostics_.lastSequence) {
        ++diagnostics_.staleStates;
        return false;
    }
    if (diagnostics_.lastSequence && sequence > *diagnostics_.lastSequence + 1) {
        diagnostics_.sequenceGaps += sequence - *diagnostics_.lastSequence - 1;
    }
    diagnostics_.lastSequence = sequence;
    ++diagnostics_.acceptedStates;

    if (previousArrival_) {
        const double interval = std::chrono::duration<double, std::milli>(
                                    receivedAt - *previousArrival_)
                                    .count();
        if (interval >= 0.0) {
            interArrivalTotalMilliseconds_ += interval;
            ++interArrivalSamples_;
            diagnostics_.averageInterArrivalMilliseconds =
                interArrivalTotalMilliseconds_ / static_cast<double>(interArrivalSamples_);
        }
    }
    previousArrival_ = receivedAt;

    if (!rateWindowStart_) {
        rateWindowStart_ = receivedAt;
    }
    ++statesInRateWindow_;
    const double rateWindowSeconds =
        std::chrono::duration<double>(receivedAt - *rateWindowStart_).count();
    if (rateWindowSeconds >= 1.0) {
        diagnostics_.statesPerSecond =
            static_cast<double>(statesInRateWindow_) / rateWindowSeconds;
        log("Gamepad input: " + std::to_string(diagnostics_.statesPerSecond)
            + " states/sec, gaps=" + std::to_string(diagnostics_.sequenceGaps)
            + ", stale=" + std::to_string(diagnostics_.staleStates)
            + ", inter-arrival="
            + std::to_string(diagnostics_.averageInterArrivalMilliseconds) + " ms");
        rateWindowStart_ = receivedAt;
        statesInRateWindow_ = 0;
    }

    if (moonlightSessionActive_ && controllerAnnounced_) {
        api_.controllerState(0,
                             ControllerZeroMask,
                             state.buttonFlags,
                             state.leftTrigger,
                             state.rightTrigger,
                             state.leftStickX,
                             state.leftStickY,
                             state.rightStickX,
                             state.rightStickY);
    }
    return true;
}

void MoonlightInputBridge::setMoonlightSessionActive(bool active)
{
    const std::lock_guard lock(mutex_);
    if (shutdown_ || moonlightSessionActive_ == active) {
        return;
    }
    if (!active) {
        neutralizeAndRemoveLocked();
        moonlightSessionActive_ = false;
        return;
    }
    moonlightSessionActive_ = true;
    announceControllerLocked();
}

void MoonlightInputBridge::onTransportClosed()
{
    const std::lock_guard lock(mutex_);
    if (shutdown_) {
        return;
    }
    const bool wasConnected = controllerConnected_;
    neutralizeAndRemoveLocked();
    controllerConnected_ = false;
    resetSequenceLocked();
    if (wasConnected) {
        log("Gamepad disconnected");
    }
}

void MoonlightInputBridge::shutdown()
{
    const std::lock_guard lock(mutex_);
    if (shutdown_) {
        return;
    }
    neutralizeAndRemoveLocked();
    controllerConnected_ = false;
    moonlightSessionActive_ = false;
    shutdown_ = true;
}

bool MoonlightInputBridge::controllerConnected() const
{
    const std::lock_guard lock(mutex_);
    return controllerConnected_;
}

MoonlightInputDiagnostics MoonlightInputBridge::diagnostics() const
{
    const std::lock_guard lock(mutex_);
    return diagnostics_;
}

MoonlightInputApi MoonlightInputBridge::makeMoonlightInputApi()
{
    return {
        [](std::uint8_t controllerNumber,
           std::uint16_t activeGamepadMask,
           std::uint8_t type,
           std::uint32_t supportedButtonFlags,
           std::uint16_t capabilities) {
            return LiSendControllerArrivalEvent(controllerNumber,
                                                activeGamepadMask,
                                                type,
                                                supportedButtonFlags,
                                                capabilities);
        },
        [](std::int16_t controllerNumber,
           std::int16_t activeGamepadMask,
           int buttonFlags,
           std::uint8_t leftTrigger,
           std::uint8_t rightTrigger,
           std::int16_t leftStickX,
           std::int16_t leftStickY,
           std::int16_t rightStickX,
           std::int16_t rightStickY) {
            return LiSendMultiControllerEvent(controllerNumber,
                                              activeGamepadMask,
                                              buttonFlags,
                                              leftTrigger,
                                              rightTrigger,
                                              leftStickX,
                                              leftStickY,
                                              rightStickX,
                                              rightStickY);
        },
    };
}

int MoonlightInputBridge::mapStandardButtons(std::uint32_t protocolButtons)
{
    int flags = 0;
    const auto map = [&flags, protocolButtons](GamepadProtocolButton input, int output) {
        if ((protocolButtons & protocolButtonMask(input)) != 0) {
            flags |= output;
        }
    };
    map(GamepadProtocolButton::A, A_FLAG);
    map(GamepadProtocolButton::B, B_FLAG);
    map(GamepadProtocolButton::X, X_FLAG);
    map(GamepadProtocolButton::Y, Y_FLAG);
    map(GamepadProtocolButton::LeftBumper, LB_FLAG);
    map(GamepadProtocolButton::RightBumper, RB_FLAG);
    map(GamepadProtocolButton::Back, BACK_FLAG);
    map(GamepadProtocolButton::Start, PLAY_FLAG);
    map(GamepadProtocolButton::LeftStick, LS_CLK_FLAG);
    map(GamepadProtocolButton::RightStick, RS_CLK_FLAG);
    map(GamepadProtocolButton::DpadUp, UP_FLAG);
    map(GamepadProtocolButton::DpadDown, DOWN_FLAG);
    map(GamepadProtocolButton::DpadLeft, LEFT_FLAG);
    map(GamepadProtocolButton::DpadRight, RIGHT_FLAG);
    map(GamepadProtocolButton::Guide, SPECIAL_FLAG);
    return flags;
}

std::uint8_t MoonlightInputBridge::mapTrigger(double value)
{
    const double clamped = std::clamp(value, 0.0, 1.0);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0));
}

std::int16_t MoonlightInputBridge::mapStick(double value, bool invert)
{
    double clamped = std::clamp(value, -1.0, 1.0);
    if (invert) {
        clamped = -clamped;
    }
    return static_cast<std::int16_t>(std::lround(clamped * 32767.0));
}

MoonlightControllerState MoonlightInputBridge::neutralControllerState()
{
    return {};
}

std::uint32_t MoonlightInputBridge::standardSupportedButtonFlags(std::size_t buttonCount)
{
    std::uint32_t flags = 0;
    const auto include = [&flags, buttonCount](std::size_t index, std::uint32_t flag) {
        if (buttonCount > index) {
            flags |= flag;
        }
    };
    include(0, A_FLAG);
    include(1, B_FLAG);
    include(2, X_FLAG);
    include(3, Y_FLAG);
    include(4, LB_FLAG);
    include(5, RB_FLAG);
    include(8, BACK_FLAG);
    include(9, PLAY_FLAG);
    include(10, LS_CLK_FLAG);
    include(11, RS_CLK_FLAG);
    include(12, UP_FLAG);
    include(13, DOWN_FLAG);
    include(14, LEFT_FLAG);
    include(15, RIGHT_FLAG);
    include(16, SPECIAL_FLAG);
    return flags;
}

void MoonlightInputBridge::announceControllerLocked()
{
    if (!moonlightSessionActive_ || !controllerConnected_ || controllerAnnounced_) {
        return;
    }
    const int result = api_.controllerArrival(
        0,
        ControllerZeroMask,
        LI_CTYPE_UNKNOWN,
        standardSupportedButtonFlags(buttonCount_),
        LI_CCAP_ANALOG_TRIGGERS);
    if (result == 0) {
        controllerAnnounced_ = true;
        log("Moonlight controller announced");
    } else {
        log("Moonlight controller announcement failed: " + std::to_string(result));
    }
}

void MoonlightInputBridge::neutralizeAndRemoveLocked()
{
    if (!moonlightSessionActive_ || !controllerAnnounced_) {
        controllerAnnounced_ = false;
        return;
    }
    const auto neutral = neutralControllerState();
    api_.controllerState(0,
                         ControllerZeroMask,
                         neutral.buttonFlags,
                         neutral.leftTrigger,
                         neutral.rightTrigger,
                         neutral.leftStickX,
                         neutral.leftStickY,
                         neutral.rightStickX,
                         neutral.rightStickY);
    api_.controllerState(0, 0, 0, 0, 0, 0, 0, 0, 0);
    controllerAnnounced_ = false;
    log("Moonlight controller removed");
}

void MoonlightInputBridge::resetSequenceLocked()
{
    diagnostics_.lastSequence.reset();
    previousArrival_.reset();
    rateWindowStart_.reset();
    statesInRateWindow_ = 0;
}

void MoonlightInputBridge::log(const std::string& message) const
{
    if (logger_) {
        logger_(message);
    }
}

} // namespace gateway::moonlight
