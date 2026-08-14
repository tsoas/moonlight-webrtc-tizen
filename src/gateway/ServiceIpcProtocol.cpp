#include "gateway/ServiceIpcProtocol.h"

#include <stdexcept>

#include <nlohmann/json.hpp>

namespace gateway::serviceipc {
namespace {

using Json = nlohmann::json;

void requireProtocolVersion(const Json& message)
{
    if (!message.is_object() || message.value("version", 0U) != ProtocolVersion) {
        throw std::invalid_argument("Unsupported local IPC protocol version");
    }
}

} // namespace

RequestType parseRequest(std::string_view payload)
{
    const Json message = Json::parse(payload);
    requireProtocolVersion(message);
    if (message.value("type", "") != "status") {
        throw std::invalid_argument("Unsupported local IPC request");
    }
    return RequestType::Status;
}

std::string makeStatusResponse(const StatusSnapshot& snapshot)
{
    Json response{
        {"version", ProtocolVersion},
        {"type", "status"},
        {"serviceRunning", snapshot.serviceRunning},
    };
    if (snapshot.sunshineConnected) {
        response["sunshineConnected"] = *snapshot.sunshineConnected;
    }
    if (snapshot.sunshinePaired) {
        response["sunshinePaired"] = *snapshot.sunshinePaired;
    }
    if (snapshot.sunshineHost) {
        response["sunshineHost"] = *snapshot.sunshineHost;
    }
    if (snapshot.runningApplicationId) {
        response["runningApplicationId"] = *snapshot.runningApplicationId;
    }
    if (snapshot.runningApplicationName) {
        response["runningApplicationName"] = *snapshot.runningApplicationName;
    }
    if (snapshot.sessionActive) {
        response["sessionActive"] = *snapshot.sessionActive;
    }
    if (snapshot.connectedTvClients) {
        response["connectedTvClients"] = *snapshot.connectedTvClients;
    }
    return response.dump();
}

std::string makeErrorResponse(std::string_view code, std::string_view message)
{
    return Json{
        {"version", ProtocolVersion},
        {"type", "error"},
        {"code", code},
        {"message", message},
    }
        .dump();
}

} // namespace gateway::serviceipc
