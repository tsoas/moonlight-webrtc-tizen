#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace gateway::managementipc {

inline constexpr std::uint32_t ProtocolVersion = 1;
inline constexpr std::size_t MaximumMessageBytes = 16 * 1024;

enum class CommandType { SetHost, Test };

struct Command {
    CommandType type;
    std::string host;
};

struct Result {
    bool ok = false;
    std::string code;
    std::string message;
};

Command parseCommand(std::string_view payload);
std::string makeCommand(const Command& command);
Result parseResult(std::string_view payload, CommandType expected);
std::string makeResult(CommandType command, const Result& result);
const char* commandName(CommandType command);

} // namespace gateway::managementipc
