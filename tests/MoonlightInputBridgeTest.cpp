#include "moonlight/input/MoonlightInputBridge.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
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

std::uint32_t protocolButtons(std::initializer_list<GamepadProtocolButton> buttons)
{
    std::uint32_t mask = 0;
    for (const auto button : buttons) {
        mask |= gateway::moonlight::protocolButtonMask(button);
    }
    return mask;
}

Json stateMessage(std::uint64_t sequence,
                  std::uint32_t buttons,
                  double leftTrigger = 0.0,
                  double rightTrigger = 0.0,
                  double leftStickX = 0.0,
                  double leftStickY = 0.0,
                  double rightStickX = 0.0,
                  double rightStickY = 0.0)
{
    return {
        {"v", gateway::moonlight::GamepadProtocolVersion},
        {"type", "gamepad-state"},
        {"seq", sequence},
        {"timestampMs", static_cast<double>(sequence) * 8.0},
        {"buttons", buttons},
        {"leftTrigger", leftTrigger},
        {"rightTrigger", rightTrigger},
        {"leftStickX", leftStickX},
        {"leftStickY", leftStickY},
        {"rightStickX", rightStickX},
        {"rightStickY", rightStickY},
    };
}

struct ArrivalCall {
    std::uint8_t controllerNumber;
    std::uint16_t activeMask;
    std::uint8_t type;
    std::uint32_t supportedButtons;
    std::uint16_t capabilities;
};

struct StateCall {
    std::int16_t controllerNumber;
    std::int16_t activeMask;
    int buttons;
    std::uint8_t leftTrigger;
    std::uint8_t rightTrigger;
    std::int16_t leftStickX;
    std::int16_t leftStickY;
    std::int16_t rightStickX;
    std::int16_t rightStickY;
};

} // namespace

int main()
{
    try {
        const auto allProtocolButtons = protocolButtons({
            GamepadProtocolButton::A,
            GamepadProtocolButton::B,
            GamepadProtocolButton::X,
            GamepadProtocolButton::Y,
            GamepadProtocolButton::LeftBumper,
            GamepadProtocolButton::RightBumper,
            GamepadProtocolButton::Back,
            GamepadProtocolButton::Start,
            GamepadProtocolButton::LeftStick,
            GamepadProtocolButton::RightStick,
            GamepadProtocolButton::DpadUp,
            GamepadProtocolButton::DpadDown,
            GamepadProtocolButton::DpadLeft,
            GamepadProtocolButton::DpadRight,
            GamepadProtocolButton::Guide,
        });
        const int allMoonlightButtons = A_FLAG | B_FLAG | X_FLAG | Y_FLAG
            | LB_FLAG | RB_FLAG | BACK_FLAG | PLAY_FLAG | LS_CLK_FLAG
            | RS_CLK_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG
            | SPECIAL_FLAG;
        require(MoonlightInputBridge::mapStandardButtons(allProtocolButtons)
                    == allMoonlightButtons,
                "Standard button mapping failed");

        require(MoonlightInputBridge::mapTrigger(0.0) == 0,
                "Trigger 0.0 mapping failed");
        require(MoonlightInputBridge::mapTrigger(1.0) == 255,
                "Trigger 1.0 mapping failed");
        require(MoonlightInputBridge::mapTrigger(-1.0) == 0
                    && MoonlightInputBridge::mapTrigger(2.0) == 255,
                "Trigger clamping failed");

        require(MoonlightInputBridge::mapStick(-1.0, false) == -32767
                    && MoonlightInputBridge::mapStick(0.0, false) == 0
                    && MoonlightInputBridge::mapStick(1.0, false) == 32767,
                "Stick endpoint mapping failed");
        require(MoonlightInputBridge::mapStick(-2.0, false) == -32767
                    && MoonlightInputBridge::mapStick(2.0, false) == 32767,
                "Stick clamping failed");
        require(MoonlightInputBridge::mapStick(-1.0, true) == 32767
                    && MoonlightInputBridge::mapStick(1.0, true) == -32767,
                "Moonlight Y-axis orientation mapping failed");

        const auto neutral = MoonlightInputBridge::neutralControllerState();
        require(neutral.buttonFlags == 0 && neutral.leftTrigger == 0
                    && neutral.rightTrigger == 0 && neutral.leftStickX == 0
                    && neutral.leftStickY == 0 && neutral.rightStickX == 0
                    && neutral.rightStickY == 0,
                "Neutral controller state was not fully centered");

        std::vector<ArrivalCall> arrivals;
        std::vector<StateCall> states;
        gateway::moonlight::MoonlightInputApi api{
            [&arrivals](std::uint8_t number,
                        std::uint16_t mask,
                        std::uint8_t type,
                        std::uint32_t buttons,
                        std::uint16_t capabilities) {
                arrivals.push_back({number, mask, type, buttons, capabilities});
                return 0;
            },
            [&states](std::int16_t number,
                      std::int16_t mask,
                      int buttons,
                      std::uint8_t leftTrigger,
                      std::uint8_t rightTrigger,
                      std::int16_t leftStickX,
                      std::int16_t leftStickY,
                      std::int16_t rightStickX,
                      std::int16_t rightStickY) {
                states.push_back({number,
                                  mask,
                                  buttons,
                                  leftTrigger,
                                  rightTrigger,
                                  leftStickX,
                                  leftStickY,
                                  rightStickX,
                                  rightStickY});
                return 0;
            },
        };

        std::vector<std::string> logs;
        MoonlightInputBridge bridge(
            [&logs](const std::string& message) { logs.push_back(message); },
            std::move(api));

        require(!bridge.handleControlMessage("{}"),
                "Malformed control protocol message was accepted");
        require(bridge.handleControlMessage(Json{
                                                {"v", 1},
                                                {"type", "gamepad-connected"},
                                                {"index", 3},
                                                {"id", "Standard Test Controller"},
                                                {"mapping", "standard"},
                                                {"buttons", 17},
                                                {"axes", 4},
                                            }
                                                .dump()),
                "Valid controller arrival was rejected");
        require(arrivals.empty(),
                "Moonlight arrival was sent before the Moonlight session was active");

        bridge.setMoonlightSessionActive(true);
        require(arrivals.size() == 1 && arrivals[0].controllerNumber == 0
                    && arrivals[0].activeMask == 1
                    && arrivals[0].type == LI_CTYPE_UNKNOWN
                    && arrivals[0].supportedButtons == allMoonlightButtons
                    && arrivals[0].capabilities == LI_CCAP_ANALOG_TRIGGERS,
                "Controller arrival state/capabilities mapping failed");

        const auto start = MoonlightInputBridge::Clock::time_point{};
        require(bridge.handleGamepadMessage(
                    stateMessage(10,
                                 protocolButtons({GamepadProtocolButton::A,
                                                  GamepadProtocolButton::DpadRight}),
                                 0.25,
                                 0.75,
                                 -1.0,
                                 1.0,
                                 0.5,
                                 -0.5)
                        .dump(),
                    start + std::chrono::milliseconds(10)),
                "Valid gamepad state was rejected");
        require(states.size() == 1 && states[0].controllerNumber == 0
                    && states[0].activeMask == 1
                    && states[0].buttons == (A_FLAG | RIGHT_FLAG)
                    && states[0].leftTrigger == 64 && states[0].rightTrigger == 191
                    && states[0].leftStickX == -32767
                    && states[0].leftStickY == -32767
                    && states[0].rightStickX == 16384
                    && states[0].rightStickY == 16384,
                "Gamepad state to Moonlight API argument mapping failed");

        require(!bridge.handleGamepadMessage(stateMessage(9, 0).dump(),
                                             start + std::chrono::milliseconds(20)),
                "Stale sequence was accepted");
        require(bridge.handleGamepadMessage(stateMessage(12, 0, -2.0, 2.0, -2.0,
                                                          2.0, -2.0, 2.0)
                                                .dump(),
                                            start + std::chrono::milliseconds(30)),
                "Monotonic sequence after a gap was rejected");
        const auto diagnostics = bridge.diagnostics();
        require(diagnostics.lastSequence == 12 && diagnostics.sequenceGaps == 1
                    && diagnostics.staleStates == 1 && diagnostics.acceptedStates == 2,
                "Monotonic sequence diagnostics failed");

        require(!bridge.handleGamepadMessage(
                    R"({"v":1,"type":"gamepad-state","seq":13})"),
                "Malformed gamepad state was accepted");

        require(bridge.handleControlMessage(
                    R"({"v":1,"type":"gamepad-disconnected"})"),
                "Valid controller removal was rejected");
        require(states.size() == 4 && states[2].activeMask == 1
                    && states[2].buttons == 0 && states[2].leftTrigger == 0
                    && states[2].rightTrigger == 0 && states[2].leftStickX == 0
                    && states[2].leftStickY == 0 && states[2].rightStickX == 0
                    && states[2].rightStickY == 0 && states[3].activeMask == 0,
                "Controller neutralization/removal state failed");
        require(!bridge.controllerConnected(),
                "Controller remained connected after removal");

        std::cout << "Moonlight input bridge tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Moonlight input bridge test failed: " << error.what() << '\n';
        return 1;
    }
}
