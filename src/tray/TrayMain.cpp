#include "tray/ServiceIpcClient.h"
#include "tray/ConfigurationWindow.h"
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

struct TrayState {
    std::unique_ptr<gateway::tray::StatusMonitor> monitor;
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
    if (command != 0) {
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
            icon.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
            wcsncpy_s(icon.szTip, L"Moonlight WebRTC Gateway", _TRUNCATE);
            Shell_NotifyIconW(NIM_ADD, &icon);
        }
        state->monitor = std::make_unique<gateway::tray::StatusMonitor>(window);
        state->monitor->start();
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
