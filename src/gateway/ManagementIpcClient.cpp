#include "gateway/ManagementIpcClient.h"

#include <windows.h>

#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace gateway::managementipc {
namespace {
constexpr wchar_t PipeName[] = L"\\\\.\\pipe\\MoonlightWebRTCGateway.Management";

class Handle { public: explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {} ~Handle() { if (value_ != INVALID_HANDLE_VALUE && value_) CloseHandle(value_); } HANDLE get() const { return value_; } private: HANDLE value_; };

bool readExact(HANDLE pipe, void* data, DWORD size)
{
    DWORD received = 0;
    return ReadFile(pipe, data, size, &received, nullptr) && received == size;
}

bool readFrame(HANDLE pipe, std::string& result)
{
    std::uint32_t size = 0;
    if (!readExact(pipe, &size, sizeof(size)) || size > MaximumMessageBytes) return false;
    result.resize(size);
    return size == 0 || readExact(pipe, result.data(), size);
}

bool writeFrame(HANDLE pipe, const std::string& value)
{
    if (value.size() > MaximumMessageBytes) return false;
    const std::uint32_t size = static_cast<std::uint32_t>(value.size());
    DWORD written = 0;
    return WriteFile(pipe, &size, sizeof(size), &written, nullptr) && written == sizeof(size)
        && (size == 0 || (WriteFile(pipe, value.data(), size, &written, nullptr) && written == size));
}

std::string win32Error(DWORD error)
{
    return std::system_category().message(static_cast<int>(error)) + " (" + std::to_string(error) + ")";
}

bool verifyTrayServer(HANDLE pipe, std::string& failure)
{
    ULONG pid = 0;
    ULONG session = 0;
    const DWORD activeSession = WTSGetActiveConsoleSessionId();
    if (!GetNamedPipeServerProcessId(pipe, &pid) || pid == 0) { failure = "server PID query failed: " + win32Error(GetLastError()); return false; }
    if (!GetNamedPipeServerSessionId(pipe, &session)) { failure = "server session query failed: " + win32Error(GetLastError()); return false; }
    if (session == 0 || activeSession == 0xFFFFFFFF || session != activeSession) { failure = "server session mismatch (server=" + std::to_string(session) + ", active=" + std::to_string(activeSession) + ")"; return false; }
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    HANDLE rawToken = nullptr;
    if (!process.get()) { failure = "server process inspection failed: " + win32Error(GetLastError()); return false; }
    if (!OpenProcessToken(process.get(), TOKEN_QUERY, &rawToken)) { failure = "server token inspection failed: " + win32Error(GetLastError()); return false; }
    Handle token(rawToken);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> data(size);
    if (!size || !GetTokenInformation(token.get(), TokenUser, data.data(), size, &size)) { failure = "server token user query failed: " + win32Error(GetLastError()); return false; }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(data.data());
    if (IsWellKnownSid(user->User.Sid, WinLocalServiceSid) || IsWellKnownSid(user->User.Sid, WinLocalSystemSid)) { failure = "server is service-owned"; return false; }
    return true;
}
}

ManagementIpcClient::ManagementIpcClient(Handler handler, Logger logger) : handler_(std::move(handler)), logger_(std::move(logger)) {}
ManagementIpcClient::~ManagementIpcClient() { stop(); }
void ManagementIpcClient::start() { if (!thread_.joinable()) { stopRequested_ = false; thread_ = std::thread([this] { run(); }); } }
void ManagementIpcClient::stop() { stopRequested_ = true; if (auto pipe = static_cast<HANDLE>(activePipe_.load())) CancelIoEx(pipe, nullptr); if (thread_.joinable()) thread_.join(); }

void ManagementIpcClient::run()
{
    while (!stopRequested_) {
        if (!WaitNamedPipeW(PipeName, 500)) continue;
        Handle pipe(CreateFileW(PipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr));
        if (pipe.get() == INVALID_HANDLE_VALUE) { logger_("Management IPC connect failed: " + win32Error(GetLastError())); continue; }
        std::string verificationFailure;
        if (!verifyTrayServer(pipe.get(), verificationFailure)) { logger_("Management IPC rejected tray endpoint: " + verificationFailure); continue; }
        logger_("Management IPC connected to verified tray endpoint");
        activePipe_ = pipe.get();
        std::string request;
        if (readFrame(pipe.get(), request)) {
            try {
                const Command command = parseCommand(request);
                if (!writeFrame(pipe.get(), makeResult(command.type, handler_(command)))) logger_("Management IPC response write failed: " + win32Error(GetLastError()));
            } catch (const std::exception& error) {
                // Malformed commands are rejected without revealing service state.
                logger_("Management IPC request rejected: " + std::string(error.what()));
            }
        } else logger_("Management IPC request read failed: " + win32Error(GetLastError()));
        activePipe_ = nullptr;
    }
}
} // namespace gateway::managementipc
