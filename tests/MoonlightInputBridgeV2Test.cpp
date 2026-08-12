#include "moonlight/input/MoonlightInputBridge.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <Limelight.h>
#include <nlohmann/json.hpp>

namespace {

using Json = nlohmann::json;
using gateway::moonlight::GamepadProtocolButton;
using gateway::moonlight::MoonlightInputBridge;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint32_t buttons(std::initializer_list<GamepadProtocolButton> values)
{
    std::uint32_t result = 0;
    for (const auto value : values) {
        result |= gateway::moonlight::protocolButtonMask(value);
    }
    return result;
}

Json connected(std::uint32_t controllerId,
               std::string id = "Test Controller",
               bool dualRumble = false,
               bool triggerRumble = false)
{
    return {
        {"v", gateway::moonlight::GamepadProtocolVersion},
        {"type", "gamepad-connected"},
        {"controllerId", controllerId},
        {"id", std::move(id)},
        {"mapping", "standard"},
        {"buttons", 17},
        {"axes", 4},
        {"dualRumble", dualRumble},
        {"triggerRumble", triggerRumble},
    };
}

Json disconnected(std::uint32_t controllerId)
{
    return {
        {"v", gateway::moonlight::GamepadProtocolVersion},
        {"type", "gamepad-disconnected"},
        {"controllerId", controllerId},
    };
}

Json state(std::uint32_t controllerId,
           std::uint64_t sequence,
           std::uint32_t buttonFlags = 0,
           double leftX = 0.0,
           double leftY = 0.0,
           double rightX = 0.0,
           double rightY = 0.0)
{
    return {
        {"v", gateway::moonlight::GamepadProtocolVersion},
        {"type", "gamepad-state"},
        {"controllerId", controllerId},
        {"seq", sequence},
        {"timestampMs", static_cast<double>(sequence)},
        {"buttons", buttonFlags},
        {"leftTrigger", 0.25},
        {"rightTrigger", 0.75},
        {"leftStickX", leftX},
        {"leftStickY", leftY},
        {"rightStickX", rightX},
        {"rightStickY", rightY},
    };
}

struct ArrivalCall {
    std::uint8_t slot;
    std::uint16_t mask;
    std::uint8_t type;
    std::uint32_t buttons;
    std::uint16_t capabilities;
};

struct StateCall {
    std::int16_t slot;
    std::int16_t mask;
    int buttons;
};

struct MouseButtonCall {
    char action;
    int button;
};

struct Recorder {
    std::mutex mutex;
    std::vector<ArrivalCall> arrivals;
    std::vector<StateCall> states;
    std::vector<std::pair<std::int16_t, std::int16_t>> moves;
    std::vector<MouseButtonCall> mouseButtons;
    std::vector<std::int8_t> scrolls;
    std::vector<std::int8_t> horizontalScrolls;
    std::vector<std::string> controlMessages;

    gateway::moonlight::MoonlightInputApi api()
    {
        return {
            [this](std::uint8_t slot, std::uint16_t mask, std::uint8_t type,
                   std::uint32_t buttonFlags, std::uint16_t capabilities) {
                const std::lock_guard lock(mutex);
                arrivals.push_back({slot, mask, type, buttonFlags, capabilities});
                return 0;
            },
            [this](std::int16_t slot, std::int16_t mask, int buttonFlags,
                   std::uint8_t, std::uint8_t, std::int16_t, std::int16_t,
                   std::int16_t, std::int16_t) {
                const std::lock_guard lock(mutex);
                states.push_back({slot, mask, buttonFlags});
                return 0;
            },
            [this](std::int16_t x, std::int16_t y) {
                const std::lock_guard lock(mutex);
                moves.emplace_back(x, y);
                return 0;
            },
            [this](char action, int button) {
                const std::lock_guard lock(mutex);
                mouseButtons.push_back({action, button});
                return 0;
            },
            [this](std::int8_t value) {
                const std::lock_guard lock(mutex);
                scrolls.push_back(value);
                return 0;
            },
            [this](std::int8_t value) {
                const std::lock_guard lock(mutex);
                horizontalScrolls.push_back(value);
                return 0;
            },
        };
    }
};

template <typename Predicate>
bool waitUntil(Predicate predicate)
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

void testMappings()
{
    require(MoonlightInputBridge::detectControllerType(
                "Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 02e0)")
                == LI_CTYPE_XBOX,
            "Xbox controller family detection failed");
    require(MoonlightInputBridge::detectControllerType("Sony DualSense Wireless Controller")
                == LI_CTYPE_PS,
            "PlayStation controller family detection failed");
    require(MoonlightInputBridge::detectControllerType("Nintendo Switch Pro Controller")
                == LI_CTYPE_NINTENDO,
            "Nintendo controller family detection failed");
    require(MoonlightInputBridge::detectControllerType("Generic Bluetooth Controller")
                == LI_CTYPE_UNKNOWN,
            "Unknown controller was guessed incorrectly");

    require(MoonlightInputBridge::rumbleMagnitude(0) == 0.0,
            "Zero rumble conversion failed");
    require(MoonlightInputBridge::rumbleMagnitude(65535) == 1.0,
            "Maximum rumble conversion failed");
    require(MoonlightInputBridge::rumbleMagnitude(65536) == 1.0,
            "Rumble clamping failed");
    require(MoonlightInputBridge::rumbleMagnitude(32768) > 0.49
                && MoonlightInputBridge::rumbleMagnitude(32768) < 0.51,
            "Mid-range rumble conversion failed");

    require(gateway::moonlight::MouseEmulationLongPressTime
                == std::chrono::milliseconds(750)
                && gateway::moonlight::MouseEmulationPollingInterval
                    == std::chrono::milliseconds(50),
            "Moonlight mouse timing constants changed");

    gateway::moonlight::MoonlightControllerState direction;
    direction.leftStickX = -32767;
    auto delta = MoonlightInputBridge::mouseDelta(direction);
    require(delta.first < 0 && delta.second == 0, "Stick left did not move left");
    direction.leftStickX = 32767;
    delta = MoonlightInputBridge::mouseDelta(direction);
    require(delta.first > 0, "Stick right did not move right");
    direction.leftStickX = 0;
    direction.leftStickY = 32767;
    delta = MoonlightInputBridge::mouseDelta(direction);
    require(delta.second < 0, "Moonlight up did not move cursor up");
    direction.leftStickY = -32767;
    delta = MoonlightInputBridge::mouseDelta(direction);
    require(delta.second > 0, "Moonlight down did not move cursor down");

    direction = {};
    direction.leftStickX = 8192;
    direction.rightStickX = 32767;
    delta = MoonlightInputBridge::mouseDelta(direction);
    require(delta.first > 50, "Stronger right stick was not selected");
}

void testMultiControllerAndRumble()
{
    Recorder recorder;
    MoonlightInputBridge bridge(
        [](const std::string&) {},
        recorder.api(),
        [&recorder](const std::string& message) {
            const std::lock_guard lock(recorder.mutex);
            recorder.controlMessages.push_back(message);
        });

    require(bridge.handleControlMessage(connected(1,
                                                   "Xbox Wireless Controller Vendor: 045e Product: 02e0",
                                                   true,
                                                   false)
                                            .dump()),
            "First controller was rejected");
    require(bridge.handleControlMessage(connected(2).dump()),
            "Second controller was rejected");
    require(bridge.handleControlMessage(connected(3).dump()),
            "Third controller was rejected");
    require(bridge.slotForController(1) == 0 && bridge.slotForController(2) == 1
                && bridge.slotForController(3) == 2,
            "Lowest-free-slot allocation failed");
    require(bridge.activeGamepadMask() == 0x0007,
            "Three-controller mask is not 0x0007");

    bridge.setMoonlightSessionActive(true);
    {
        const std::lock_guard lock(recorder.mutex);
        require(recorder.arrivals.size() == 3,
                "Connected controllers were not announced at session start");
        const auto xbox = std::find_if(recorder.arrivals.begin(), recorder.arrivals.end(),
                                      [](const ArrivalCall& call) { return call.slot == 0; });
        require(xbox != recorder.arrivals.end() && xbox->mask == 0x0007
                    && xbox->type == LI_CTYPE_XBOX
                    && (xbox->capabilities & LI_CCAP_ANALOG_TRIGGERS) != 0
                    && (xbox->capabilities & LI_CCAP_RUMBLE) != 0
                    && (xbox->capabilities & LI_CCAP_TRIGGER_RUMBLE) == 0,
                "Xbox type/capability announcement failed");
    }

    const auto origin = MoonlightInputBridge::Clock::time_point{};
    require(bridge.handleGamepadMessage(state(1, 10, buttons({GamepadProtocolButton::A})).dump(),
                                        origin + std::chrono::milliseconds(10)),
            "Controller 1 state failed");
    require(bridge.handleGamepadMessage(state(2, 1, buttons({GamepadProtocolButton::B})).dump(),
                                        origin + std::chrono::milliseconds(11)),
            "Independent controller 2 sequence failed");
    require(!bridge.handleGamepadMessage(state(1, 9).dump(),
                                         origin + std::chrono::milliseconds(12)),
            "Stale per-controller sequence was accepted");
    require(bridge.diagnostics(1)->staleStates == 1
                && bridge.diagnostics(2)->staleStates == 0,
            "Sequence diagnostics leaked between controllers");
    {
        const std::lock_guard lock(recorder.mutex);
        require(recorder.states[0].slot == 0 && recorder.states[0].buttons == A_FLAG
                    && recorder.states[1].slot == 1 && recorder.states[1].buttons == B_FLAG,
                "Controller state was routed to the wrong slot");
    }

    bridge.handleRumble(0, 65535, 32768);
    require(waitUntil([&] {
        const std::lock_guard lock(recorder.mutex);
        return !recorder.controlMessages.empty();
    }), "Rumble command was not dispatched asynchronously");
    {
        const std::lock_guard lock(recorder.mutex);
        const auto message = Json::parse(recorder.controlMessages.back());
        require(message["type"] == "rumble" && message["controllerId"] == 1
                    && message["controllerSlot"] == 0
                    && message["strongMagnitude"] == 1.0
                    && message["weakMagnitude"].get<double>() > 0.49,
                "Moonlight slot-to-client rumble routing failed");
        recorder.controlMessages.clear();
    }
    bridge.handleRumble(9, 65535, 65535);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    {
        const std::lock_guard lock(recorder.mutex);
        require(recorder.controlMessages.empty(),
                "Rumble for an absent controller was not ignored");
    }

    require(bridge.handleControlMessage(disconnected(2).dump()),
            "Controller removal failed");
    require(bridge.activeGamepadMask() == 0x0005
                && bridge.slotForController(1) == 0
                && bridge.slotForController(3) == 2,
            "Removing slot 1 disturbed other controllers");
    require(bridge.handleControlMessage(connected(4).dump())
                && bridge.slotForController(4) == 1,
            "Freed controller slot was not reused");

    for (std::uint32_t id = 5; id <= 17; ++id) {
        require(bridge.handleControlMessage(connected(id).dump()),
                "Controller up to slot 15 was rejected");
    }
    require(bridge.slotForController(17) == 15,
            "Maximum controller slot 15 was not allocated");
    require(!bridge.handleControlMessage(connected(18).dump()),
            "Seventeenth controller was not rejected safely");

    bridge.setMoonlightSessionActive(false);
    require(bridge.controllerCount() == 0 && bridge.activeGamepadMask() == 0,
            "Session cleanup did not clear mappings and mask");
}

void testMouseMode()
{
    Recorder recorder;
    MoonlightInputBridge bridge(
        [](const std::string&) {},
        recorder.api(),
        [&recorder](const std::string& message) {
            const std::lock_guard lock(recorder.mutex);
            recorder.controlMessages.push_back(message);
        });
    bridge.setMoonlightSessionActive(true);
    require(bridge.handleControlMessage(connected(10).dump())
                && bridge.handleControlMessage(connected(11).dump()),
            "Mouse test controllers were rejected");

    const auto origin = MoonlightInputBridge::Clock::time_point{};
    const auto start = buttons({GamepadProtocolButton::Start});
    require(bridge.handleGamepadMessage(state(10, 1, start).dump(), origin),
            "Short Start press failed");
    require(bridge.handleGamepadMessage(state(10, 2, 0).dump(),
                                        origin + std::chrono::milliseconds(750)),
            "Exact-threshold Start release failed");
    {
        const std::lock_guard lock(recorder.mutex);
        require(recorder.states.size() == 2,
                "Start at exactly 750 ms incorrectly toggled mouse mode");
    }

    require(bridge.handleGamepadMessage(state(10, 3, start).dump(),
                                        origin + std::chrono::milliseconds(1000)),
            "Long Start press failed");
    require(bridge.handleGamepadMessage(state(10, 4, start).dump(),
                                        origin + std::chrono::milliseconds(2000)),
            "Held Start state failed");
    {
        const std::lock_guard lock(recorder.mutex);
        const auto mouseModeMessages = std::count_if(
            recorder.controlMessages.begin(), recorder.controlMessages.end(),
            [](const std::string& message) {
                return Json::parse(message)["type"] == "mouse-mode";
            });
        require(mouseModeMessages == 0,
                "Mouse mode toggled before Start was released");
    }
    require(bridge.handleGamepadMessage(state(10, 5, 0, 1.0, 0.0).dump(),
                                        origin + std::chrono::milliseconds(2001)),
            "Long Start release failed");
    require(waitUntil([&] {
        const std::lock_guard lock(recorder.mutex);
        return std::any_of(recorder.controlMessages.begin(),
                           recorder.controlMessages.end(),
                           [](const std::string& message) {
                               return Json::parse(message)["type"] == "mouse-mode";
                           });
    }), "Mouse mode status was not sent");
    {
        const std::lock_guard lock(recorder.mutex);
        const auto iterator = std::find_if(
            recorder.controlMessages.rbegin(), recorder.controlMessages.rend(),
            [](const std::string& message) {
                return Json::parse(message)["type"] == "mouse-mode";
            });
        require(iterator != recorder.controlMessages.rend(),
                "Mouse activation status was not found");
        const auto status = Json::parse(*iterator);
        require(status["type"] == "mouse-mode" && status["controllerId"] == 10
                    && status["active"] == true,
                "Mouse activation status is invalid");
    }

    std::size_t normalStateCount = 0;
    {
        const std::lock_guard lock(recorder.mutex);
        normalStateCount = recorder.states.size();
    }
    require(bridge.handleGamepadMessage(
                state(10, 6, buttons({GamepadProtocolButton::A,
                                      GamepadProtocolButton::DpadUp}),
                      1.0, 0.0)
                    .dump(),
                origin + std::chrono::milliseconds(2010)),
            "Mouse-mode input failed");
    require(bridge.handleGamepadMessage(state(11, 1, buttons({GamepadProtocolButton::Y})).dump(),
                                        origin + std::chrono::milliseconds(2011)),
            "Other controller input failed in mouse mode");
    {
        const std::lock_guard lock(recorder.mutex);
        require(recorder.states.size() == normalStateCount + 1
                    && recorder.states.back().slot == 1
                    && recorder.states.back().buttons == Y_FLAG,
                "Mouse mode suppressed the wrong controller");
        require(!recorder.mouseButtons.empty()
                    && recorder.mouseButtons.back().action == BUTTON_ACTION_PRESS
                    && recorder.mouseButtons.back().button == BUTTON_LEFT,
                "A did not map to left mouse press");
        require(!recorder.scrolls.empty() && recorder.scrolls.back() == 1,
                "D-pad Up did not map to vertical scroll");
    }
    require(waitUntil([&] {
        const std::lock_guard lock(recorder.mutex);
        return !recorder.moves.empty();
    }), "50 ms mouse polling did not produce relative movement");

    require(bridge.handleGamepadMessage(
                state(10, 7, buttons({GamepadProtocolButton::B,
                                      GamepadProtocolButton::X,
                                      GamepadProtocolButton::LeftBumper,
                                      GamepadProtocolButton::RightBumper,
                                      GamepadProtocolButton::DpadLeft}))
                    .dump(),
                origin + std::chrono::milliseconds(2100)),
            "Additional mouse mappings failed");
    {
        const std::lock_guard lock(recorder.mutex);
        require(std::any_of(recorder.mouseButtons.begin(), recorder.mouseButtons.end(),
                            [](const MouseButtonCall& call) {
                                return call.action == BUTTON_ACTION_PRESS
                                    && call.button == BUTTON_RIGHT;
                            })
                    && std::any_of(recorder.mouseButtons.begin(), recorder.mouseButtons.end(),
                                   [](const MouseButtonCall& call) {
                                       return call.action == BUTTON_ACTION_PRESS
                                           && call.button == BUTTON_MIDDLE;
                                   })
                    && std::any_of(recorder.mouseButtons.begin(), recorder.mouseButtons.end(),
                                   [](const MouseButtonCall& call) {
                                       return call.button == BUTTON_X1;
                                   })
                    && std::any_of(recorder.mouseButtons.begin(), recorder.mouseButtons.end(),
                                   [](const MouseButtonCall& call) {
                                       return call.button == BUTTON_X2;
                                   }),
                "Mouse button mapping is incomplete");
        require(!recorder.horizontalScrolls.empty()
                    && recorder.horizontalScrolls.back() == -1,
                "D-pad Left did not map to horizontal scroll");
    }

    require(bridge.handleGamepadMessage(state(10, 8, start).dump(),
                                        origin + std::chrono::milliseconds(3000))
                && bridge.handleGamepadMessage(state(10, 9, 0).dump(),
                                               origin + std::chrono::milliseconds(3751)),
            "Second long Start toggle failed");
    {
        const std::lock_guard lock(recorder.mutex);
        require(recorder.states.back().slot == 0 && recorder.states.back().buttons == 0,
                "Normal controller forwarding did not resume cleanly");
        const auto released = std::count_if(recorder.mouseButtons.begin(),
                                            recorder.mouseButtons.end(),
                                            [](const MouseButtonCall& call) {
                                                return call.action == BUTTON_ACTION_RELEASE;
                                            });
        require(released >= 5,
                "Mouse mode cleanup did not release simulated buttons");
    }

    bridge.onTransportClosed();
    require(bridge.controllerCount() == 0 && bridge.activeGamepadMask() == 0,
            "Transport cleanup leaked mouse/controller state");
}

void testMouseActivationIgnoresHeldButtons()
{
    Recorder recorder;
    MoonlightInputBridge bridge(
        [](const std::string&) {},
        recorder.api(),
        [&recorder](const std::string& message) {
            const std::lock_guard lock(recorder.mutex);
            recorder.controlMessages.push_back(message);
        });
    bridge.setMoonlightSessionActive(true);
    require(bridge.handleControlMessage(connected(20).dump()),
            "Held-button mouse test controller was rejected");

    const auto origin = MoonlightInputBridge::Clock::time_point{};
    const auto start = buttons({GamepadProtocolButton::Start});
    const auto heldAtActivation = buttons({GamepadProtocolButton::A});
    require(bridge.handleGamepadMessage(state(20, 1, start).dump(), origin)
                && bridge.handleGamepadMessage(state(20, 2, heldAtActivation).dump(),
                                               origin + std::chrono::milliseconds(751))
                && bridge.handleGamepadMessage(state(20, 3, 0).dump(),
                                               origin + std::chrono::milliseconds(800)),
            "Held-button mouse activation sequence failed");

    const std::lock_guard lock(recorder.mutex);
    require(recorder.mouseButtons.empty(),
            "Mouse activation emitted an unmatched event for an already-held button");
}

} // namespace

int main()
{
    try {
        testMappings();
        testMultiControllerAndRumble();
        testMouseMode();
        testMouseActivationIgnoresHeldButtons();
        std::cout << "Moonlight input bridge v2 tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Moonlight input bridge v2 test failed: " << error.what() << '\n';
        return 1;
    }
}
