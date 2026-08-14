#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gateway::managementipc {

inline constexpr std::uint32_t ProtocolVersion = 1;
inline constexpr std::size_t MaximumMessageBytes = 16 * 1024;

enum class CommandType { SetHost, Test, Pair, PairStatus, Unpair };

struct Command {
    CommandType type;
    std::string host;
};

struct Result {
    bool ok = false;
    std::string code;
    std::string message;
    // The PIN is carried only on the already mutually-authenticated local pipe
    // and is never written to logs or persistent storage.
    std::optional<std::string> pin;
};

Command parseCommand(std::string_view payload);
std::string makeCommand(const Command& command);
Result parseResult(std::string_view payload, CommandType expected);
std::string makeResult(CommandType command, const Result& result);
const char* commandName(CommandType command);

} // namespace gateway::managementipc
