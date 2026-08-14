#include "tray/ServiceIpcClient.h"
#include "tray/ConfigurationWindow.h"
#include "tray/ManagementIpcServer.h"
#include "tray/StatusMonitor.h"

#include <windows.h>
#include <shellapi.h>

#include <optional>
#include <string>
#include <memory>

namespace {

constexpr UINT TrayIconId = 1;
constexpr UINT ExitCommandId = 100;
constexpr UINT OpenCommandId = 101;
constexpr UINT TrayCallbackMessage = WM_APP + 1;
constexpr UINT TrayIconResourceId = 1;

struct TrayState {
    std::unique_ptr<gateway::tray::StatusMonitor> monitor;
    std::unique_ptr<gateway::tray::ManagementIpcServer> management;
    gateway::tray::ConfigurationWindow configurationWindow;
};

std::wstring widen(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

gateway::tray::StatusState currentStatus(const TrayState& state)
{
    return state.monitor ? state.monitor->snapshot() : gateway::tray::StatusState{};
}

void refreshStatus(HWND window, const TrayState& state)
{
    const auto status = currentStatus(state);
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window;
    icon.uID = TrayIconId;
    icon.uFlags = NIF_TIP;
    const std::wstring tooltip = status.gatewayAvailable
        ? L"Moonlight WebRTC Gateway: Running"
        : L"Moonlight WebRTC Gateway: Unavailable";
    wcsncpy_s(icon.szTip, tooltip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &icon);
}

void showMenu(HWND window, TrayState& state)
{
    const auto status = currentStatus(state);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, L"Moonlight WebRTC");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu,
                MF_STRING | MF_DISABLED,
                0,
                status.gatewayAvailable ? L"Gateway: Running" : L"Gateway: Unavailable");

    std::wstring sunshine = L"Sunshine: Disconnected";
    if (status.status && status.status->sunshineConnected && *status.status->sunshineConnected) {
        sunshine = L"Sunshine: Connected";
        if (status.status->sunshineHost) {
            sunshine += L" (" + widen(*status.status->sunshineHost) + L")";
        }
    }
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, sunshine.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, OpenCommandId, L"Open Moonlight WebRTC");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ExitCommandId, L"Exit");

    SetForegroundWindow(window);
    const POINT cursor = [] {
        POINT point{};
        GetCursorPos(&point);
        return point;
    }();
    const UINT command = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window, nullptr);
    DestroyMenu(menu);
    // Let the shell complete the tray-menu dismissal before handling a command
    // that may create and foreground the configuration window.
    PostMessageW(window, WM_NULL, 0, 0);
    if (command == OpenCommandId) {
        auto* monitor = state.monitor.get();
        state.configurationWindow.show(
            GetModuleHandleW(nullptr), [monitor] {
                return monitor ? monitor->snapshot() : gateway::tray::StatusState{};
            }, [management = state.management.get()](const gateway::managementipc::Command& command) {
                return management ? management->execute(command)
                                  : gateway::managementipc::Result{false, "unavailable", "Management IPC is unavailable"};
            });
    } else if (command == ExitCommandId) {
        PostMessageW(window, WM_COMMAND, command, 0);
    }
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<TrayState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE:
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new TrayState()));
        state = reinterpret_cast<TrayState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        {
            NOTIFYICONDATAW icon{};
            icon.cbSize = sizeof(icon);
            icon.hWnd = window;
            icon.uID = TrayIconId;
            icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
            icon.uCallbackMessage = TrayCallbackMessage;
            const HICON smallIcon = static_cast<HICON>(LoadImageW(
                GetModuleHandleW(nullptr), MAKEINTRESOURCEW(TrayIconResourceId), IMAGE_ICON,
                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED));
            const HICON largeIcon = static_cast<HICON>(LoadImageW(
                GetModuleHandleW(nullptr), MAKEINTRESOURCEW(TrayIconResourceId), IMAGE_ICON,
                GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED));
            SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
            SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
            icon.hIcon = smallIcon;
            wcsncpy_s(icon.szTip, L"Moonlight WebRTC Gateway", _TRUNCATE);
            Shell_NotifyIconW(NIM_ADD, &icon);
        }
        state->monitor = std::make_unique<gateway::tray::StatusMonitor>(window);
        state->monitor->start();
        state->management = std::make_unique<gateway::tray::ManagementIpcServer>();
        state->management->start();
        refreshStatus(window, *state);
        return 0;
    case gateway::tray::StatusChangedMessage:
        if (state) {
            refreshStatus(window, *state);
            state->configurationWindow.statusChanged();
        }
        return 0;
    case WM_COMMAND:
        if (!state) return 0;
        if (LOWORD(wParam) == OpenCommandId) {
            auto* monitor = state->monitor.get();
            state->configurationWindow.show(
                GetModuleHandleW(nullptr), [monitor] {
                    return monitor ? monitor->snapshot() : gateway::tray::StatusState{};
                }, [management = state->management.get()](const gateway::managementipc::Command& command) {
                    return management ? management->execute(command)
                                      : gateway::managementipc::Result{false, "unavailable", "Management IPC is unavailable"};
                });
        } else if (LOWORD(wParam) == ExitCommandId) {
            DestroyWindow(window);
        }
        return 0;
    case TrayCallbackMessage:
        if ((lParam == WM_RBUTTONUP || lParam == WM_LBUTTONDBLCLK) && state) {
            showMenu(window, *state);
        }
        return 0;
    case WM_DESTROY:
        if (state) {
            state->configurationWindow.close();
            state->monitor->stop();
            state->management->stop();
        }
        {
            NOTIFYICONDATAW icon{};
            icon.cbSize = sizeof(icon);
            icon.hWnd = window;
            icon.uID = TrayIconId;
            Shell_NotifyIconW(NIM_DELETE, &icon);
        }
        delete state;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    // This must happen before the hidden tray window (and any configuration
    // window) is created so each monitor supplies its actual DPI.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    constexpr wchar_t WindowClassName[] = L"MoonlightWebRTCTrayWindow";
    WNDCLASSW windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.lpszClassName = WindowClassName;
    RegisterClassW(&windowClass);

    HWND window = CreateWindowExW(0,
                                  WindowClassName,
                                  L"Moonlight WebRTC",
                                  WS_OVERLAPPED,
                                  0,
                                  0,
                                  0,
                                  0,
                                  nullptr,
                                  nullptr,
                                  instance,
                                  nullptr);
    if (!window) {
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
