#include "gateway/ManagementIpcProtocol.h"

#include <iostream>
#include <stdexcept>

namespace { void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); } }

int main()
{
    try {
        using namespace gateway::managementipc;
        const auto setHost = parseCommand(R"({"version":1,"type":"set-host","host":"sunshine.local"})");
        require(setHost.type == CommandType::SetHost && setHost.host == "sunshine.local", "set-host was not parsed");
        require(parseCommand(R"({"version":1,"type":"test"})").type == CommandType::Test, "test was not parsed");
        require(parseCommand(R"({"version":1,"type":"pair"})").type == CommandType::Pair, "pair was not parsed");
        require(parseCommand(R"({"version":1,"type":"pair-status"})").type == CommandType::PairStatus, "pair-status was not parsed");
        require(parseCommand(R"({"version":1,"type":"unpair"})").type == CommandType::Unpair, "unpair was not parsed");
        for (const char* invalid : {R"({"version":2,"type":"test"})", R"({"version":1,"type":"pair","pin":"1234"})", R"({"version":1,"type":"set-host"})", R"({"version":1,"type":"test","x":1})"}) {
            bool rejected = false; try { (void)parseCommand(invalid); } catch (...) { rejected = true; } require(rejected, "invalid command was accepted");
        }
        const Result expected{true, "reachable", "Sunshine is reachable and paired"};
        const auto parsed = parseResult(makeResult(CommandType::Test, expected), CommandType::Test);
        require(parsed.ok && parsed.code == "reachable", "result was not preserved");
        const Result pairingStarted{true, "pairing-started", "Enter the PIN", "1234"};
        const auto pairingResult = parseResult(makeResult(CommandType::Pair, pairingStarted), CommandType::Pair);
        require(pairingResult.pin == "1234", "pairing PIN was not preserved in the authenticated response");
        bool invalidPinRejected = false;
        try { (void)parseResult(R"({"version":1,"type":"result","command":"pair","ok":true,"code":"pairing-started","message":"x","pin":"12"})", CommandType::Pair); } catch (...) { invalidPinRejected = true; }
        require(invalidPinRejected, "invalid pairing response PIN was accepted");
        bool mismatch = false; try { (void)parseResult(makeResult(CommandType::Test, expected), CommandType::SetHost); } catch (...) { mismatch = true; } require(mismatch, "mismatched response accepted");
        std::cout << "Management IPC protocol tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "Management IPC protocol test failed: " << error.what() << '\n'; return 1; }
}
