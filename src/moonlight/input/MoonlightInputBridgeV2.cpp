#include "moonlight/input/MoonlightInputBridge.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include <Limelight.h>
#include <nlohmann/json.hpp>

namespace gateway::moonlight {
namespace {

using Json = nlohmann::json;

constexpr std::uint32_t AllProtocolButtons = (1U << 15) - 1;
constexpr std::uint32_t MouseButtonMask =
    protocolButtonMask(GamepadProtocolButton::A)
    | protocolButtonMask(GamepadProtocolButton::B)
    | protocolButtonMask(GamepadProtocolButton::X)
    | protocolButtonMask(GamepadProtocolButton::LeftBumper)
    | protocolButtonMask(GamepadProtocolButton::RightBumper);

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

bool readControllerId(const Json& message, std::uint32_t& output)
{
    if (!message.contains("controllerId")
        || !message["controllerId"].is_number_unsigned()) {
        return false;
    }
    const auto value = message["controllerId"].get<std::uint64_t>();
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    output = static_cast<std::uint32_t>(value);
    return true;
}

bool readOptionalBoolean(const Json& message, const char* name, bool& output)
{
    if (!message.contains(name)) {
        output = false;
        return true;
    }
    if (!message[name].is_boolean()) {
        return false;
    }
    output = message[name].get<bool>();
    return true;
}

std::string lowerCase(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

bool hasButton(std::uint32_t flags, GamepadProtocolButton button)
{
    return (flags & protocolButtonMask(button)) != 0;
}

int moonlightMouseButton(GamepadProtocolButton button)
{
    switch (button) {
    case GamepadProtocolButton::A:
        return BUTTON_LEFT;
    case GamepadProtocolButton::B:
        return BUTTON_RIGHT;
    case GamepadProtocolButton::X:
        return BUTTON_MIDDLE;
    case GamepadProtocolButton::LeftBumper:
        return BUTTON_X1;
    case GamepadProtocolButton::RightBumper:
        return BUTTON_X2;
    default:
        return 0;
    }
}

} // namespace

MoonlightInputBridge::MoonlightInputBridge(Logger logger, ControlSender controlSender)
    : MoonlightInputBridge(std::move(logger),
                           makeMoonlightInputApi(),
                           std::move(controlSender))
{
}

MoonlightInputBridge::MoonlightInputBridge(Logger logger,
                                           MoonlightInputApi api,
                                           ControlSender controlSender)
    : logger_(std::move(logger))
    , api_(std::move(api))
    , controlSender_(std::move(controlSender))
{
    if (!api_.controllerArrival || !api_.controllerState || !api_.mouseMove
        || !api_.mouseButton || !api_.scroll || !api_.horizontalScroll) {
        throw std::invalid_argument("Moonlight input API callbacks are required");
    }
    worker_ = std::jthread([this](std::stop_token stopToken) {
        workerLoop(stopToken);
    });
}

MoonlightInputBridge::~MoonlightInputBridge()
{
    shutdown();
}

bool MoonlightInputBridge::handleControlMessage(std::string_view text)
{
    try {
        const Json message = Json::parse(text.begin(), text.end());
        std::uint32_t clientControllerId = 0;
        if (hasProtocolEnvelope(message, "gamepad-connected")) {
            if (!readControllerId(message, clientControllerId)
                || !message.contains("id") || !message["id"].is_string()
                || !message.contains("mapping") || !message["mapping"].is_string()) {
                return false;
            }

            const std::string id = message["id"].get<std::string>();
            const std::string mapping = message["mapping"].get<std::string>();
            std::size_t buttonCount = 0;
            std::size_t axisCount = 0;
            bool dualRumble = false;
            bool triggerRumble = false;
            if (id.size() > 512 || mapping.size() > 128
                || !readBoundedSize(message, "buttons", 64, buttonCount)
                || !readBoundedSize(message, "axes", 16, axisCount)
                || !readOptionalBoolean(message, "dualRumble", dualRumble)
                || !readOptionalBoolean(message, "triggerRumble", triggerRumble)) {
                return false;
            }

            const std::lock_guard lock(mutex_);
            if (shutdown_) {
                return false;
            }
            if (controllers_.contains(clientControllerId)) {
                removeControllerLocked(clientControllerId);
            }
            const auto slot = allocateSlotLocked();
            if (!slot) {
                log("Gamepad rejected: all 16 Moonlight controller slots are occupied");
                return false;
            }

            Controller controller;
            controller.clientControllerId = clientControllerId;
            controller.slot = *slot;
            controller.id = id;
            controller.mapping = mapping;
            controller.buttonCount = buttonCount;
            controller.axisCount = axisCount;
            controller.type = detectControllerType(id);
            if (buttonCount > 7) {
                controller.capabilities |= LI_CCAP_ANALOG_TRIGGERS;
            }
            if (dualRumble) {
                controller.capabilities |= LI_CCAP_RUMBLE;
            }
            if (triggerRumble) {
                controller.capabilities |= LI_CCAP_TRIGGER_RUMBLE;
            }

            slotToController_[*slot] = clientControllerId;
            activeGamepadMask_ |= static_cast<std::uint16_t>(1U << *slot);
            auto [iterator, inserted] = controllers_.emplace(clientControllerId,
                                                              std::move(controller));
            if (!inserted) {
                return false;
            }
            log("Gamepad connected: client=" + std::to_string(clientControllerId)
                + ", slot=" + std::to_string(*slot) + ", id=" + id);
            announceControllerLocked(iterator->second);
            return true;
        }

        if (hasProtocolEnvelope(message, "gamepad-disconnected")) {
            if (!readControllerId(message, clientControllerId)) {
                return false;
            }
            const std::lock_guard lock(mutex_);
            if (shutdown_) {
                return false;
            }
            const bool existed = controllers_.contains(clientControllerId);
            removeControllerLocked(clientControllerId);
            return existed;
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
    std::uint32_t clientControllerId = 0;
    std::uint64_t sequence = 0;
    std::uint32_t protocolButtons = 0;
    double clientTimestamp = 0.0;
    double leftTrigger = 0.0;
    double rightTrigger = 0.0;
    double leftStickX = 0.0;
    double leftStickY = 0.0;
    double rightStickX = 0.0;
    double rightStickY = 0.0;

    try {
        const Json message = Json::parse(text.begin(), text.end());
        if (!hasProtocolEnvelope(message, "gamepad-state")
            || !readControllerId(message, clientControllerId)
            || !message.contains("seq") || !message["seq"].is_number_unsigned()
            || !message.contains("buttons") || !message["buttons"].is_number_unsigned()) {
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
            return false;
        }
        protocolButtons = static_cast<std::uint32_t>(buttons);
    } catch (const Json::exception&) {
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
    const auto iterator = controllers_.find(clientControllerId);
    if (shutdown_ || iterator == controllers_.end()) {
        return false;
    }
    auto& controller = iterator->second;
    ++controller.diagnostics.receivedMessages;
    if (controller.diagnostics.lastSequence
        && sequence <= *controller.diagnostics.lastSequence) {
        ++controller.diagnostics.staleStates;
        return false;
    }
    if (controller.diagnostics.lastSequence
        && sequence > *controller.diagnostics.lastSequence + 1) {
        controller.diagnostics.sequenceGaps +=
            sequence - *controller.diagnostics.lastSequence - 1;
    }
    controller.diagnostics.lastSequence = sequence;
    ++controller.diagnostics.acceptedStates;
    updateDiagnosticsLocked(controller, receivedAt);

    controller.latestState = state;
    controller.latestProtocolButtons = protocolButtons;

    const bool startPressed = hasButton(protocolButtons, GamepadProtocolButton::Start);
    const bool startWasPressed = hasButton(controller.previousProtocolButtons,
                                           GamepadProtocolButton::Start);
    if (startPressed && !startWasPressed) {
        controller.startPressedAt = receivedAt;
    }

    bool mouseModeToggled = false;
    if (!startPressed && startWasPressed && controller.startPressedAt
        && receivedAt - *controller.startPressedAt > MouseEmulationLongPressTime) {
        if (!controller.mouseMode && moonlightSessionActive_ && controller.announced) {
            api_.controllerState(controller.slot,
                                 static_cast<std::int16_t>(activeGamepadMask_),
                                 state.buttonFlags,
                                 state.leftTrigger,
                                 state.rightTrigger,
                                 state.leftStickX,
                                 state.leftStickY,
                                 state.rightStickX,
                                 state.rightStickY);
        }
        if (controller.mouseMode) {
            releaseMouseButtonsLocked(controller);
        }
        controller.mouseMode = !controller.mouseMode;
        controller.suppressedMouseButtons = controller.mouseMode
            ? protocolButtons & MouseButtonMask
            : 0;
        queueMouseModeStatusLocked(controller);
        log("Gamepad mouse mode " + std::string(controller.mouseMode ? "enabled" : "disabled")
            + ": slot=" + std::to_string(controller.slot));
        mouseModeToggled = true;
    }
    if (!startPressed) {
        controller.startPressedAt.reset();
    }

    if (moonlightSessionActive_ && controller.announced) {
        if (controller.mouseMode) {
            const auto effectiveProtocolButtons =
                protocolButtons & ~controller.suppressedMouseButtons;
            const auto effectivePreviousProtocolButtons =
                controller.previousProtocolButtons & ~controller.suppressedMouseButtons;
            if (!mouseModeToggled) {
                constexpr std::array<GamepadProtocolButton, 5> mouseButtons{
                    GamepadProtocolButton::A,
                    GamepadProtocolButton::B,
                    GamepadProtocolButton::X,
                    GamepadProtocolButton::LeftBumper,
                    GamepadProtocolButton::RightBumper,
                };
                for (const auto button : mouseButtons) {
                    const bool pressed = hasButton(effectiveProtocolButtons, button);
                    const bool wasPressed = hasButton(effectivePreviousProtocolButtons, button);
                    if (pressed != wasPressed) {
                        api_.mouseButton(pressed ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE,
                                         moonlightMouseButton(button));
                    }
                }
                const auto rising = [protocolButtons, &controller](GamepadProtocolButton button) {
                    return hasButton(protocolButtons, button)
                        && !hasButton(controller.previousProtocolButtons, button);
                };
                if (rising(GamepadProtocolButton::DpadUp)) {
                    api_.scroll(1);
                }
                if (rising(GamepadProtocolButton::DpadDown)) {
                    api_.scroll(-1);
                }
                if (rising(GamepadProtocolButton::DpadRight)) {
                    api_.horizontalScroll(1);
                }
                if (rising(GamepadProtocolButton::DpadLeft)) {
                    api_.horizontalScroll(-1);
                }
            }
            controller.suppressedMouseButtons &= protocolButtons;
            controller.simulatedMouseButtons = effectiveProtocolButtons & MouseButtonMask;
        } else {
            api_.controllerState(controller.slot,
                                 static_cast<std::int16_t>(activeGamepadMask_),
                                 state.buttonFlags,
                                 state.leftTrigger,
                                 state.rightTrigger,
                                 state.leftStickX,
                                 state.leftStickY,
                                 state.rightStickX,
                                 state.rightStickY);
        }
    }
    controller.previousProtocolButtons = protocolButtons;
    return true;
}

void MoonlightInputBridge::handleRumble(std::uint16_t controllerNumber,
                                        std::uint16_t lowFrequencyMotor,
                                        std::uint16_t highFrequencyMotor)
{
    const std::lock_guard lock(mutex_);
    if (shutdown_ || controllerNumber >= slotToController_.size()
        || !slotToController_[controllerNumber]) {
        return;
    }
    auto& controller = controllers_.at(*slotToController_[controllerNumber]);
    if ((controller.capabilities & LI_CCAP_RUMBLE) == 0) {
        return;
    }
    controller.strongMagnitude = rumbleMagnitude(lowFrequencyMotor);
    controller.weakMagnitude = rumbleMagnitude(highFrequencyMotor);
    queueRumbleStateLocked(controller);
    log("Moonlight rumble: controller=" + std::to_string(controllerNumber)
        + " low=" + std::to_string(lowFrequencyMotor)
        + " high=" + std::to_string(highFrequencyMotor));
}

void MoonlightInputBridge::handleTriggerRumble(std::uint16_t controllerNumber,
                                               std::uint16_t leftTriggerMotor,
                                               std::uint16_t rightTriggerMotor)
{
    const std::lock_guard lock(mutex_);
    if (shutdown_ || controllerNumber >= slotToController_.size()
        || !slotToController_[controllerNumber]) {
        return;
    }
    auto& controller = controllers_.at(*slotToController_[controllerNumber]);
    if ((controller.capabilities & LI_CCAP_TRIGGER_RUMBLE) == 0) {
        return;
    }
    controller.leftTriggerMagnitude = rumbleMagnitude(leftTriggerMotor);
    controller.rightTriggerMagnitude = rumbleMagnitude(rightTriggerMotor);
    queueRumbleStateLocked(controller);
}

void MoonlightInputBridge::setMoonlightSessionActive(bool active)
{
    const std::lock_guard lock(mutex_);
    if (shutdown_ || moonlightSessionActive_ == active) {
        return;
    }
    if (active) {
        moonlightSessionActive_ = true;
        for (auto& [clientControllerId, controller] : controllers_) {
            announceControllerLocked(controller);
        }
    } else {
        clearControllersLocked();
        moonlightSessionActive_ = false;
    }
}

void MoonlightInputBridge::onTransportClosed()
{
    const std::lock_guard lock(mutex_);
    if (!shutdown_) {
        clearControllersLocked();
    }
}

void MoonlightInputBridge::shutdown()
{
    {
        const std::lock_guard lock(mutex_);
        if (shutdown_) {
            return;
        }
        clearControllersLocked();
        moonlightSessionActive_ = false;
        shutdown_ = true;
    }
    worker_.request_stop();
    workerCondition_.notify_all();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

bool MoonlightInputBridge::controllerConnected() const
{
    return controllerCount() != 0;
}

std::size_t MoonlightInputBridge::controllerCount() const
{
    const std::lock_guard lock(mutex_);
    return controllers_.size();
}

std::uint16_t MoonlightInputBridge::activeGamepadMask() const
{
    const std::lock_guard lock(mutex_);
    return activeGamepadMask_;
}

std::optional<std::uint8_t> MoonlightInputBridge::slotForController(
    std::uint32_t clientControllerId) const
{
    const std::lock_guard lock(mutex_);
    const auto iterator = controllers_.find(clientControllerId);
    return iterator == controllers_.end()
        ? std::nullopt
        : std::optional<std::uint8_t>(iterator->second.slot);
}

std::optional<MoonlightInputDiagnostics> MoonlightInputBridge::diagnostics(
    std::uint32_t clientControllerId) const
{
    const std::lock_guard lock(mutex_);
    const auto iterator = controllers_.find(clientControllerId);
    return iterator == controllers_.end()
        ? std::nullopt
        : std::optional<MoonlightInputDiagnostics>(iterator->second.diagnostics);
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
        [](std::int16_t deltaX, std::int16_t deltaY) {
            return LiSendMouseMoveEvent(deltaX, deltaY);
        },
        [](char action, int button) {
            return LiSendMouseButtonEvent(action, button);
        },
        [](std::int8_t clicks) {
            return LiSendScrollEvent(clicks);
        },
        [](std::int8_t clicks) {
            return LiSendHScrollEvent(clicks);
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
    return static_cast<std::uint8_t>(
        std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
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

std::uint8_t MoonlightInputBridge::detectControllerType(std::string_view id)
{
    const auto normalized = lowerCase(id);
    if (normalized.find("vendor: 045e") != std::string::npos
        || normalized.find("vendor 045e") != std::string::npos
        || normalized.find("xbox") != std::string::npos
        || normalized.find("microsoft") != std::string::npos) {
        return LI_CTYPE_XBOX;
    }
    if (normalized.find("vendor: 054c") != std::string::npos
        || normalized.find("vendor 054c") != std::string::npos
        || normalized.find("dualshock") != std::string::npos
        || normalized.find("dualsense") != std::string::npos
        || normalized.find("playstation") != std::string::npos
        || normalized.find("sony") != std::string::npos) {
        return LI_CTYPE_PS;
    }
    if (normalized.find("vendor: 057e") != std::string::npos
        || normalized.find("vendor 057e") != std::string::npos
        || normalized.find("nintendo") != std::string::npos
        || normalized.find("joy-con") != std::string::npos
        || normalized.find("switch pro") != std::string::npos) {
        return LI_CTYPE_NINTENDO;
    }
    return LI_CTYPE_UNKNOWN;
}

double MoonlightInputBridge::rumbleMagnitude(std::uint32_t value)
{
    return static_cast<double>(std::min<std::uint32_t>(value, 65535U)) / 65535.0;
}

std::pair<std::int16_t, std::int16_t> MoonlightInputBridge::mouseDelta(
    const MoonlightControllerState& state)
{
    const int leftY = -static_cast<int>(state.leftStickY);
    const int rightY = -static_cast<int>(state.rightStickY);
    const bool useLeft = std::abs(static_cast<int>(state.leftStickX)) + std::abs(leftY)
        > std::abs(static_cast<int>(state.rightStickX)) + std::abs(rightY);
    const int rawX = useLeft ? state.leftStickX : state.rightStickX;
    const int rawY = useLeft ? leftY : rightY;
    const auto accelerate = [](int raw) {
        double delta = std::pow(raw / 32766.0 * MouseEmulationMotionMultiplier, 3);
        delta = std::abs(delta) > MouseEmulationDeadzone
            ? delta - MouseEmulationDeadzone
            : 0.0;
        return static_cast<std::int16_t>(std::clamp(
            std::lround(delta),
            static_cast<long>(std::numeric_limits<std::int16_t>::min()),
            static_cast<long>(std::numeric_limits<std::int16_t>::max())));
    };
    return {accelerate(rawX), accelerate(rawY)};
}

std::optional<std::uint8_t> MoonlightInputBridge::allocateSlotLocked() const
{
    for (std::size_t index = 0; index < slotToController_.size(); ++index) {
        if (!slotToController_[index]) {
            return static_cast<std::uint8_t>(index);
        }
    }
    return std::nullopt;
}

void MoonlightInputBridge::announceControllerLocked(Controller& controller)
{
    if (!moonlightSessionActive_ || controller.announced) {
        return;
    }
    const int result = api_.controllerArrival(
        controller.slot,
        activeGamepadMask_,
        controller.type,
        standardSupportedButtonFlags(controller.buttonCount),
        controller.capabilities);
    if (result == 0) {
        controller.announced = true;
        queueControlMessageLocked(Json{
            {"v", GamepadProtocolVersion},
            {"type", "controller-status"},
            {"controllerId", controller.clientControllerId},
            {"controllerSlot", controller.slot},
            {"activeGamepadMask", activeGamepadMask_},
            {"controllerType", controller.type},
            {"capabilities", controller.capabilities},
        }.dump());
        log("Moonlight controller announced: slot=" + std::to_string(controller.slot)
            + ", mask=" + std::to_string(activeGamepadMask_));
    } else {
        log("Moonlight controller announcement failed: " + std::to_string(result));
    }
}

void MoonlightInputBridge::neutralizeAndRemoveLocked(Controller& controller)
{
    releaseMouseButtonsLocked(controller);
    if (controller.mouseMode) {
        controller.mouseMode = false;
        queueMouseModeStatusLocked(controller);
    }
    controller.strongMagnitude = 0.0;
    controller.weakMagnitude = 0.0;
    controller.leftTriggerMagnitude = 0.0;
    controller.rightTriggerMagnitude = 0.0;
    queueRumbleStateLocked(controller);

    const auto slotBit = static_cast<std::uint16_t>(1U << controller.slot);
    if (moonlightSessionActive_ && controller.announced) {
        const auto neutral = neutralControllerState();
        api_.controllerState(controller.slot,
                             static_cast<std::int16_t>(activeGamepadMask_),
                             neutral.buttonFlags,
                             neutral.leftTrigger,
                             neutral.rightTrigger,
                             neutral.leftStickX,
                             neutral.leftStickY,
                             neutral.rightStickX,
                             neutral.rightStickY);
        activeGamepadMask_ &= static_cast<std::uint16_t>(~slotBit);
        api_.controllerState(controller.slot,
                             static_cast<std::int16_t>(activeGamepadMask_),
                             0, 0, 0, 0, 0, 0, 0);
        log("Moonlight controller removed: slot=" + std::to_string(controller.slot)
            + ", mask=" + std::to_string(activeGamepadMask_));
    } else {
        activeGamepadMask_ &= static_cast<std::uint16_t>(~slotBit);
    }
    controller.announced = false;
}

void MoonlightInputBridge::releaseMouseButtonsLocked(Controller& controller)
{
    constexpr std::array<GamepadProtocolButton, 5> mouseButtons{
        GamepadProtocolButton::A,
        GamepadProtocolButton::B,
        GamepadProtocolButton::X,
        GamepadProtocolButton::LeftBumper,
        GamepadProtocolButton::RightBumper,
    };
    for (const auto button : mouseButtons) {
        if (hasButton(controller.simulatedMouseButtons, button)) {
            api_.mouseButton(BUTTON_ACTION_RELEASE, moonlightMouseButton(button));
        }
    }
    controller.simulatedMouseButtons = 0;
    controller.suppressedMouseButtons = 0;
}

void MoonlightInputBridge::removeControllerLocked(std::uint32_t clientControllerId)
{
    const auto iterator = controllers_.find(clientControllerId);
    if (iterator == controllers_.end()) {
        return;
    }
    const auto slot = iterator->second.slot;
    neutralizeAndRemoveLocked(iterator->second);
    slotToController_[slot].reset();
    controllers_.erase(iterator);
    log("Gamepad disconnected: client=" + std::to_string(clientControllerId)
        + ", slot=" + std::to_string(slot));
}

void MoonlightInputBridge::clearControllersLocked()
{
    while (!controllers_.empty()) {
        removeControllerLocked(controllers_.begin()->first);
    }
    activeGamepadMask_ = 0;
}

void MoonlightInputBridge::updateDiagnosticsLocked(Controller& controller,
                                                   Clock::time_point receivedAt)
{
    if (controller.previousArrival) {
        const double interval = std::chrono::duration<double, std::milli>(
                                    receivedAt - *controller.previousArrival)
                                    .count();
        if (interval >= 0.0) {
            controller.interArrivalTotalMilliseconds += interval;
            ++controller.interArrivalSamples;
            controller.diagnostics.averageInterArrivalMilliseconds =
                controller.interArrivalTotalMilliseconds
                / static_cast<double>(controller.interArrivalSamples);
        }
    }
    controller.previousArrival = receivedAt;
    if (!controller.rateWindowStart) {
        controller.rateWindowStart = receivedAt;
    }
    ++controller.statesInRateWindow;
    const double elapsed = std::chrono::duration<double>(
                               receivedAt - *controller.rateWindowStart)
                               .count();
    if (elapsed >= 1.0) {
        controller.diagnostics.statesPerSecond =
            static_cast<double>(controller.statesInRateWindow) / elapsed;
        queueControlMessageLocked(Json{
            {"v", GamepadProtocolVersion},
            {"type", "input-diagnostics"},
            {"controllerId", controller.clientControllerId},
            {"controllerSlot", controller.slot},
            {"messagesPerSecond", controller.diagnostics.statesPerSecond},
            {"lastSequence", controller.diagnostics.lastSequence.value_or(0)},
            {"sequenceGaps", controller.diagnostics.sequenceGaps},
            {"staleStates", controller.diagnostics.staleStates},
        }.dump());
        log("Gamepad input: slot=" + std::to_string(controller.slot)
            + ", states/sec=" + std::to_string(controller.diagnostics.statesPerSecond)
            + ", gaps=" + std::to_string(controller.diagnostics.sequenceGaps)
            + ", stale=" + std::to_string(controller.diagnostics.staleStates));
        controller.rateWindowStart = receivedAt;
        controller.statesInRateWindow = 0;
    }
}

void MoonlightInputBridge::queueControlMessageLocked(std::string message)
{
    if (!controlSender_) {
        return;
    }
    pendingControlMessages_.push_back(std::move(message));
    workerCondition_.notify_all();
}

void MoonlightInputBridge::queueMouseModeStatusLocked(const Controller& controller)
{
    queueControlMessageLocked(Json{
        {"v", GamepadProtocolVersion},
        {"type", "mouse-mode"},
        {"controllerId", controller.clientControllerId},
        {"controllerSlot", controller.slot},
        {"active", controller.mouseMode},
    }.dump());
}

void MoonlightInputBridge::queueRumbleStateLocked(const Controller& controller)
{
    queueControlMessageLocked(Json{
        {"v", GamepadProtocolVersion},
        {"type", "rumble"},
        {"controllerId", controller.clientControllerId},
        {"controllerSlot", controller.slot},
        {"strongMagnitude", controller.strongMagnitude},
        {"weakMagnitude", controller.weakMagnitude},
        {"leftTrigger", controller.leftTriggerMagnitude},
        {"rightTrigger", controller.rightTriggerMagnitude},
    }.dump());
}

void MoonlightInputBridge::workerLoop(std::stop_token stopToken)
{
    auto nextMouseTick = Clock::now() + MouseEmulationPollingInterval;
    while (!stopToken.stop_requested()) {
        std::vector<std::string> messages;
        std::vector<std::pair<std::int16_t, std::int16_t>> movements;
        {
            std::unique_lock lock(mutex_);
            workerCondition_.wait_until(lock, nextMouseTick, [&] {
                return shutdown_ || !pendingControlMessages_.empty()
                    || stopToken.stop_requested();
            });
            if (shutdown_ || stopToken.stop_requested()) {
                break;
            }
            messages.swap(pendingControlMessages_);
            const auto now = Clock::now();
            if (now >= nextMouseTick) {
                for (const auto& [clientControllerId, controller] : controllers_) {
                    if (moonlightSessionActive_ && controller.announced
                        && controller.mouseMode) {
                        const auto delta = mouseDelta(controller.latestState);
                        if (delta.first != 0 || delta.second != 0) {
                            movements.push_back(delta);
                        }
                    }
                }
                nextMouseTick = now + MouseEmulationPollingInterval;
            }
        }
        for (const auto& message : messages) {
            controlSender_(message);
        }
        for (const auto& [deltaX, deltaY] : movements) {
            api_.mouseMove(deltaX, deltaY);
        }
    }
}

void MoonlightInputBridge::log(const std::string& message) const
{
    if (logger_) {
        logger_(message);
    }
}

} // namespace gateway::moonlight
