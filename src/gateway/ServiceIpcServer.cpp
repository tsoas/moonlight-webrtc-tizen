#include "gateway/ServiceIpcServer.h"

#include <windows.h>
#include <sddl.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace gateway::serviceipc {
namespace {

constexpr wchar_t PipeName[] = L"\\\\.\\pipe\\MoonlightWebRTCGateway";
constexpr DWORD MaximumMessageBytes = 16 * 1024;
constexpr wchar_t PipeSecurityDescriptor[] =
    // Local Users receive the standard named-pipe read mask only. Opening the
    // pipe is the read-only snapshot request, avoiding a write-up from the
    // interactive user to the LocalService-owned pipe.
    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;0x00120089;;;BU)";

class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE)
        : value_(value)
    {
    }

    ~Handle()
    {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) {
            CloseHandle(value_);
        }
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    HANDLE get() const { return value_; }

private:
    HANDLE value_;
};

bool waitForIo(HANDLE pipe,
               OVERLAPPED& overlapped,
               const std::atomic<bool>& stopRequested,
               DWORD& transferred)
{
    for (;;) {
        const DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 200);
        if (waitResult == WAIT_OBJECT_0) {
            return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
        }
        if (waitResult != WAIT_TIMEOUT || stopRequested.load(std::memory_order_acquire)) {
            CancelIoEx(pipe, &overlapped);
            return false;
        }
    }
}

bool connectPipe(HANDLE pipe, const std::atomic<bool>& stopRequested)
{
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (event.get() == nullptr) {
        return false;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    if (ConnectNamedPipe(pipe, &overlapped)) {
        return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) {
        return true;
    }
    if (error != ERROR_IO_PENDING) {
        return false;
    }
    DWORD ignored = 0;
    return waitForIo(pipe, overlapped, stopRequested, ignored);
}

bool writeMessage(HANDLE pipe,
                  const std::atomic<bool>& stopRequested,
                  const std::string& message)
{
    if (message.size() > MaximumMessageBytes) {
        return false;
    }
    std::string framed(sizeof(std::uint32_t) + message.size(), '\0');
    const auto size = static_cast<std::uint32_t>(message.size());
    for (std::size_t index = 0; index < sizeof(size); ++index) {
        framed[index] = static_cast<char>((size >> (index * 8U)) & 0xffU);
    }
    framed.replace(sizeof(size), message.size(), message);

    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (event.get() == nullptr) {
        return false;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD written = 0;
    if (!WriteFile(pipe, framed.data(), static_cast<DWORD>(framed.size()), &written, &overlapped)) {
        if (GetLastError() != ERROR_IO_PENDING
            || !waitForIo(pipe, overlapped, stopRequested, written)) {
            return false;
        }
    }
    return written == framed.size();
}

Handle createPipe()
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            PipeSecurityDescriptor, SDDL_REVISION_1, &descriptor, nullptr)) {
        throw std::runtime_error("Unable to create local IPC security descriptor");
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;

    const HANDLE pipe = CreateNamedPipeW(
        PipeName,
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES,
        MaximumMessageBytes + sizeof(std::uint32_t),
        MaximumMessageBytes,
        0,
        &attributes);
    LocalFree(descriptor);
    if (pipe == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Unable to create local Gateway IPC pipe");
    }
    return Handle(pipe);
}

} // namespace

ServiceIpcServer::ServiceIpcServer(StatusProvider statusProvider)
    : statusProvider_(std::move(statusProvider))
{
}

ServiceIpcServer::~ServiceIpcServer()
{
    stop();
}

void ServiceIpcServer::start()
{
    if (thread_.joinable()) {
        return;
    }
    stopRequested_.store(false, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
}

void ServiceIpcServer::stop()
{
    stopRequested_.store(true, std::memory_order_release);
    if (const auto pipe = static_cast<HANDLE>(activePipe_.load(std::memory_order_acquire))) {
        CancelIoEx(pipe, nullptr);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    activePipe_.store(nullptr, std::memory_order_release);
}

void ServiceIpcServer::run()
{
    while (!stopRequested_.load(std::memory_order_acquire)) {
        try {
            auto pipe = createPipe();
            activePipe_.store(pipe.get(), std::memory_order_release);
            if (!connectPipe(pipe.get(), stopRequested_)) {
                activePipe_.store(nullptr, std::memory_order_release);
                continue;
            }

            try {
                writeMessage(pipe.get(), stopRequested_, makeStatusResponse(statusProvider_()));
            } catch (const std::exception&) {
                writeMessage(pipe.get(), stopRequested_,
                             makeErrorResponse("status-unavailable", "Local IPC status unavailable"));
            }
            DisconnectNamedPipe(pipe.get());
            activePipe_.store(nullptr, std::memory_order_release);
        } catch (const std::exception&) {
            // A later tray refresh can reconnect after a transient pipe failure.
            activePipe_.store(nullptr, std::memory_order_release);
            Sleep(200);
        }
    }
}

} // namespace gateway::serviceipc
