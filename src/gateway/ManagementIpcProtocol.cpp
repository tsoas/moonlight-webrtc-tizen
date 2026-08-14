#include "gateway/ManagementIpcProtocol.h"

#include <stdexcept>

#include <nlohmann/json.hpp>

namespace gateway::managementipc {
namespace {
using Json = nlohmann::json;

void requireVersion(const Json& value)
{
    if (!value.is_object() || value.value("version", 0U) != ProtocolVersion) {
        throw std::invalid_argument("Unsupported management IPC protocol version");
    }
}
}

const char* commandName(CommandType command)
{
    return command == CommandType::SetHost ? "set-host" : "test";
}

Command parseCommand(std::string_view payload)
{
    if (payload.size() > MaximumMessageBytes) throw std::invalid_argument("Management IPC request is too large");
    const Json value = Json::parse(payload);
    requireVersion(value);
    const auto type = value.value("type", "");
    if (type == "set-host") {
        if (!value.contains("host") || !value.at("host").is_string() || value.size() != 3) {
            throw std::invalid_argument("Invalid set-host management request");
        }
        return {CommandType::SetHost, value.at("host").get<std::string>()};
    }
    if (type == "test" && value.size() == 2) return {CommandType::Test, {}};
    throw std::invalid_argument("Unsupported management IPC command");
}

std::string makeCommand(const Command& command)
{
    Json value{{"version", ProtocolVersion}, {"type", commandName(command.type)}};
    if (command.type == CommandType::SetHost) value["host"] = command.host;
    return value.dump();
}

Result parseResult(std::string_view payload, CommandType expected)
{
    if (payload.size() > MaximumMessageBytes) throw std::invalid_argument("Management IPC response is too large");
    const Json value = Json::parse(payload);
    requireVersion(value);
    if (value.value("type", "") != "result" || value.value("command", "") != commandName(expected)
        || !value.contains("ok") || !value.at("ok").is_boolean()
        || !value.contains("code") || !value.at("code").is_string()
        || !value.contains("message") || !value.at("message").is_string()
        || value.size() != 6) {
        throw std::invalid_argument("Invalid management IPC response");
    }
    return {value.at("ok").get<bool>(), value.at("code").get<std::string>(), value.at("message").get<std::string>()};
}

std::string makeResult(CommandType command, const Result& result)
{
    return Json{{"version", ProtocolVersion}, {"type", "result"},
                {"command", commandName(command)}, {"ok", result.ok},
                {"code", result.code}, {"message", result.message}}.dump();
}
} // namespace gateway::managementipc
