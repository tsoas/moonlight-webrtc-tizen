#include "tray/ServiceIpcClient.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace gateway::tray {
namespace {

constexpr wchar_t PipeName[] = L"\\\\.\\pipe\\MoonlightWebRTCGateway";
constexpr DWORD MaximumMessageBytes = 16 * 1024;
constexpr DWORD ClientPipeAccess = FILE_READ_DATA | SYNCHRONIZE;

class Handle {
public:
    explicit Handle(HANDLE value)
        : value_(value)
    {
    }
    ~Handle()
    {
        if (value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }
    HANDLE get() const { return value_; }

private:
    HANDLE value_;
};

} // namespace

serviceipc::StatusSnapshot requestServiceStatus()
{
    if (!WaitNamedPipeW(PipeName, 750)) {
        throw std::runtime_error("Gateway local IPC is unavailable");
    }
    Handle pipe(CreateFileW(PipeName,
                            ClientPipeAccess,
                            0,
                            nullptr,
                            OPEN_EXISTING,
                            0,
                            nullptr));
    if (pipe.get() == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Unable to connect to Gateway local IPC");
    }
    std::array<char, MaximumMessageBytes + sizeof(std::uint32_t)> frame{};
    DWORD frameSize = 0;
    if (!ReadFile(pipe.get(), frame.data(), static_cast<DWORD>(frame.size()), &frameSize, nullptr)
        || frameSize < sizeof(std::uint32_t)) {
        throw std::runtime_error("Gateway local IPC status response was incomplete");
    }

    std::uint32_t responseSize = 0;
    for (std::size_t index = 0; index < sizeof(responseSize); ++index) {
        responseSize |= static_cast<std::uint32_t>(static_cast<unsigned char>(frame[index]))
            << (index * 8U);
    }
    if (responseSize > MaximumMessageBytes
        || frameSize != sizeof(responseSize) + responseSize) {
        throw std::runtime_error("Gateway local IPC returned an invalid status frame");
    }

    const auto message = nlohmann::json::parse(std::string_view(
        frame.data() + sizeof(responseSize), responseSize));
    if (message.value("version", 0U) != serviceipc::ProtocolVersion
        || message.value("type", "") != "status") {
        throw std::runtime_error("Gateway local IPC returned an invalid status response");
    }

    serviceipc::StatusSnapshot snapshot;
    snapshot.serviceRunning = message.value("serviceRunning", false);
    if (message.contains("sunshineConnected")) snapshot.sunshineConnected = message.at("sunshineConnected").get<bool>();
    if (message.contains("sunshinePaired")) snapshot.sunshinePaired = message.at("sunshinePaired").get<bool>();
    if (message.contains("sunshineHost")) snapshot.sunshineHost = message.at("sunshineHost").get<std::string>();
    if (message.contains("runningApplicationId")) snapshot.runningApplicationId = message.at("runningApplicationId").get<std::string>();
    if (message.contains("runningApplicationName")) snapshot.runningApplicationName = message.at("runningApplicationName").get<std::string>();
    if (message.contains("sessionActive")) snapshot.sessionActive = message.at("sessionActive").get<bool>();
    if (message.contains("connectedTvClients")) snapshot.connectedTvClients = message.at("connectedTvClients").get<std::uint32_t>();
    return snapshot;
}

} // namespace gateway::tray
