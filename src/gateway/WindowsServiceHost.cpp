#include "gateway/WindowsServiceHost.h"

#include <windows.h>

#include <atomic>
#include <stdexcept>

namespace gateway {
namespace {

struct ServiceContext {
    GatewayShutdownSignal* shutdown = nullptr;
    WindowsServiceHost::RuntimeCallback runtime;
    SERVICE_STATUS_HANDLE statusHandle = nullptr;
    SERVICE_STATUS status{};
    std::atomic<bool> running = false;
};

ServiceContext* activeContext = nullptr;

void reportStatus(ServiceContext& context, DWORD state, DWORD win32ExitCode = NO_ERROR)
{
    context.status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    context.status.dwCurrentState = state;
    context.status.dwWin32ExitCode = win32ExitCode;
    context.status.dwServiceSpecificExitCode = 0;
    context.status.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    context.status.dwCheckPoint = (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) ? 1 : 0;
    context.status.dwWaitHint = (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) ? 15000 : 0;
    SetServiceStatus(context.statusHandle, &context.status);
}

DWORD WINAPI serviceControlHandler(DWORD control, DWORD, LPVOID, LPVOID)
{
    auto* context = activeContext;
    if (!context) {
        return NO_ERROR;
    }

    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        reportStatus(*context, SERVICE_STOP_PENDING);
        // The handler only signals. Runtime teardown runs on the service main thread.
        context->shutdown->request();
        break;
    case SERVICE_CONTROL_INTERROGATE:
        SetServiceStatus(context->statusHandle, &context->status);
        break;
    default:
        break;
    }
    return NO_ERROR;
}

void WINAPI serviceMain(DWORD, LPWSTR*)
{
    auto& context = *activeContext;
    context.statusHandle = RegisterServiceCtrlHandlerExW(
        L"MoonlightWebRTCGateway", serviceControlHandler, nullptr);
    if (!context.statusHandle) {
        return;
    }

    reportStatus(context, SERVICE_START_PENDING);
    DWORD exitCode = NO_ERROR;
    try {
        const int result = context.runtime([&context] {
            context.running.store(true, std::memory_order_release);
            reportStatus(context, SERVICE_RUNNING);
        });
        exitCode = result == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
    } catch (...) {
        exitCode = ERROR_SERVICE_SPECIFIC_ERROR;
    }

    reportStatus(context, SERVICE_STOPPED, exitCode);
}

} // namespace

int WindowsServiceHost::run(const std::wstring& serviceName,
                            GatewayShutdownSignal& shutdown,
                            const RuntimeCallback& runtime)
{
    ServiceContext context;
    context.shutdown = &shutdown;
    context.runtime = runtime;
    activeContext = &context;

    SERVICE_TABLE_ENTRYW dispatchTable[] = {
        { const_cast<LPWSTR>(serviceName.c_str()), serviceMain },
        { nullptr, nullptr },
    };
    const BOOL dispatched = StartServiceCtrlDispatcherW(dispatchTable);
    const DWORD error = dispatched ? NO_ERROR : GetLastError();
    activeContext = nullptr;

    if (!dispatched) {
        throw std::runtime_error("StartServiceCtrlDispatcherW failed: " + std::to_string(error));
    }
    return 0;
}

} // namespace gateway
