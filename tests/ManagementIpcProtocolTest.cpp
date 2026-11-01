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
        const auto bareTest = parseCommand(R"({"version":1,"type":"test"})");
        require(bareTest.type == CommandType::Test && bareTest.host.empty(), "test was not parsed");
        const auto hostedTest = parseCommand(R"({"version":1,"type":"test","host":"192.168.1.85:27786"})");
        require(hostedTest.type == CommandType::Test && hostedTest.host == "192.168.1.85:27786",
                "test did not carry the requested host");
        require(makeCommand({CommandType::Test, "192.168.1.85:27786"})
                    == R"({"host":"192.168.1.85:27786","type":"test","version":1})",
                "test command did not serialize its host");
        require(makeCommand({CommandType::Test, {}}) == R"({"type":"test","version":1})",
                "test command invented a host");
        require(parseCommand(R"({"version":1,"type":"pair"})").type == CommandType::Pair, "pair was not parsed");
        require(parseCommand(R"({"version":1,"type":"pair-status"})").type == CommandType::PairStatus, "pair-status was not parsed");
        require(parseCommand(R"({"version":1,"type":"unpair"})").type == CommandType::Unpair, "unpair was not parsed");
        for (const char* invalid : {R"({"version":2,"type":"test"})", R"({"version":1,"type":"pair","pin":"1234"})", R"({"version":1,"type":"set-host"})", R"({"version":1,"type":"test","x":1})",
                R"({"version":1,"type":"test","host":5})", R"({"version":1,"type":"test","host":"a","x":1})"}) {
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
