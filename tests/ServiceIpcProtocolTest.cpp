#include "gateway/ServiceIpcProtocol.h"

#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main()
{
    try {
        require(gateway::serviceipc::parseRequest(R"({"version":1,"type":"status"})")
                    == gateway::serviceipc::RequestType::Status,
                "Status request was not accepted");

        bool rejectedWrongVersion = false;
        try {
            gateway::serviceipc::parseRequest(R"({"version":2,"type":"status"})");
        } catch (const std::exception&) {
            rejectedWrongVersion = true;
        }
        require(rejectedWrongVersion, "Unsupported IPC protocol version was accepted");

        bool rejectedWriteRequest = false;
        try {
            gateway::serviceipc::parseRequest(R"({"version":1,"type":"stop-service"})");
        } catch (const std::exception&) {
            rejectedWriteRequest = true;
        }
        require(rejectedWriteRequest, "Read-only IPC accepted a write request");

        gateway::serviceipc::StatusSnapshot snapshot;
        snapshot.sunshineConnected = true;
        snapshot.sunshinePaired = true;
        snapshot.sunshineHost = "Sunshine-PC";
        snapshot.runningApplicationId = "7";
        snapshot.runningApplicationName = "Desktop";
        snapshot.sessionActive = true;
        snapshot.connectedTvClients = 1;
        const auto response = nlohmann::json::parse(
            gateway::serviceipc::makeStatusResponse(snapshot));
        require(response.at("version") == gateway::serviceipc::ProtocolVersion
                    && response.at("type") == "status"
                    && response.at("serviceRunning") == true
                    && response.at("sunshinePaired") == true
                    && response.at("sunshineHost") == "Sunshine-PC"
                    && response.at("runningApplicationId") == "7"
                    && response.at("runningApplicationName") == "Desktop"
                    && response.at("connectedTvClients") == 1,
                "IPC status response did not preserve supported snapshot fields");

        const auto minimal = nlohmann::json::parse(
            gateway::serviceipc::makeStatusResponse({}));
        require(!minimal.contains("sunshineHost") && !minimal.contains("runningApplicationId"),
                "IPC status response invented unavailable values");

        const auto error = nlohmann::json::parse(
            gateway::serviceipc::makeErrorResponse("invalid-request", "Local IPC request failed"));
        require(error.at("type") == "error" && error.at("code") == "invalid-request",
                "IPC error response is malformed");

        std::cout << "Service IPC protocol tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Service IPC protocol test failed: " << error.what() << '\n';
        return 1;
    }
}
