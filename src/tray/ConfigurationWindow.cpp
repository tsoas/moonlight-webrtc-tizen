#include "tray/ConfigurationWindow.h"

#include "tray/WindowsTheme.h"

#include <dwmapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <windowsx.h>

namespace gateway::tray {
namespace {

constexpr wchar_t WindowClassName[] = L"MoonlightWebRTCConfigurationWindow";
constexpr int HeaderHeight = 62;
constexpr int NavigationLeft = 22;
constexpr int NavigationTop = 82;
constexpr int NavigationWidth = 170;
constexpr int NavigationItemHeight = 50;
constexpr int ContentLeft = 214;
constexpr int ContentTop = 82;
constexpr int ContentBottom = 22;
constexpr UINT SaveHostCommandId = 301;
constexpr UINT TestHostCommandId = 302;
constexpr UINT ManagementResultMessage = WM_APP + 31;

struct PageDefinition {
    ConfigurationWindow::Page page;
    const wchar_t* label;
};

constexpr std::array Pages{
    PageDefinition{ConfigurationWindow::Page::Status, L"Status"},
    PageDefinition{ConfigurationWindow::Page::Sunshine, L"Sunshine"},
    PageDefinition{ConfigurationWindow::Page::Network, L"Network"},
    PageDefinition{ConfigurationWindow::Page::About, L"About"},
};

HFONT makeFont(int height, int weight = FW_NORMAL)
{
    return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void fill(HDC dc, const RECT& area, COLORREF color)
{
    const HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &area, brush);
    DeleteObject(brush);
}

void drawText(HDC dc, const std::wstring& text, RECT area, HFONT font, COLORREF color, UINT format)
{
    const HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &area, format);
    SelectObject(dc, oldFont);
}

std::wstring widen(const std::string& value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::string narrow(const wchar_t* value)
{
    if (!value || !*value) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), length, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::wstring boolValue(const std::optional<bool>& value, const wchar_t* trueValue, const wchar_t* falseValue)
{
    if (!value) return L"Unavailable";
    return *value ? trueValue : falseValue;
}

std::wstring applicationValue(const StatusState& state)
{
    if (!state.status || !state.status->runningApplicationId) return L"None";
    return widen(*state.status->runningApplicationId);
}

void enableDarkTitleBar(HWND window)
{
    const BOOL enabled = TRUE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE is available on supported Windows 10/11
    // builds. Attribute 19 covers earlier Windows 10 implementations.
    constexpr DWORD DarkTitleBarAttribute = 20;
    constexpr DWORD LegacyDarkTitleBarAttribute = 19;
    if (FAILED(DwmSetWindowAttribute(
            window, DarkTitleBarAttribute, &enabled, sizeof(enabled)))) {
        DwmSetWindowAttribute(window, LegacyDarkTitleBarAttribute, &enabled, sizeof(enabled));
    }
}

std::wstring programDataPath()
{
    PWSTR directory = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &directory))) {
        std::wstring path(directory);
        CoTaskMemFree(directory);
        return path + L"\\MoonlightWebRTC";
    }
    return L"Unavailable";
}

} // namespace

ConfigurationWindow::~ConfigurationWindow()
{
    if (operationThread_.joinable()) operationThread_.join();
}

void ConfigurationWindow::show(HINSTANCE instance, StatusProvider statusProvider,
                               ManagementProvider managementProvider)
{
    statusProvider_ = std::move(statusProvider);
    managementProvider_ = std::move(managementProvider);
    if (window_) {
        ShowWindow(window_, SW_RESTORE);
        SetForegroundWindow(window_);
        return;
    }

    WNDCLASSW windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground = CreateSolidBrush(theme::PageBackground);
    windowClass.lpszClassName = WindowClassName;
    RegisterClassW(&windowClass);

    window_ = CreateWindowExW(0, WindowClassName, L"Moonlight WebRTC",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, 880, 590,
                              nullptr, nullptr, instance, this);
    if (window_) enableDarkTitleBar(window_);
}

void ConfigurationWindow::close()
{
    if (window_) DestroyWindow(window_);
}

void ConfigurationWindow::statusChanged()
{
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

LRESULT CALLBACK ConfigurationWindow::windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<ConfigurationWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<ConfigurationWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handleMessage(window, message, wParam, lParam)
                : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT ConfigurationWindow::handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_PAINT:
        paint(window);
        return 0;
    case WM_LBUTTONUP:
        selectPageFromPoint(window, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == SaveHostCommandId || LOWORD(wParam) == TestHostCommandId) {
            wchar_t host[256]{};
            GetWindowTextW(hostEdit_, host, static_cast<int>(std::size(host)));
            startManagementOperation({LOWORD(wParam) == SaveHostCommandId
                                         ? managementipc::CommandType::SetHost : managementipc::CommandType::Test,
                                     narrow(host)});
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT: {
        SetTextColor(reinterpret_cast<HDC>(wParam), theme::TextPrimary);
        SetBkColor(reinterpret_cast<HDC>(wParam), theme::PanelFocus);
        static const HBRUSH brush = CreateSolidBrush(theme::PanelFocus);
        return reinterpret_cast<LRESULT>(brush);
    }
    case WM_CTLCOLORBTN:
        SetTextColor(reinterpret_cast<HDC>(wParam), theme::TextPrimary);
        SetBkColor(reinterpret_cast<HDC>(wParam), theme::PanelBackground);
        static const HBRUSH brush = CreateSolidBrush(theme::PanelBackground);
        return reinterpret_cast<LRESULT>(brush);
    case ManagementResultMessage:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        window_ = nullptr;
        hostEdit_ = saveButton_ = testButton_ = nullptr;
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ConfigurationWindow::selectPageFromPoint(HWND window, POINT point)
{
    if (point.x < NavigationLeft || point.x >= NavigationLeft + NavigationWidth
        || point.y < NavigationTop || point.y >= NavigationTop + static_cast<int>(Pages.size()) * NavigationItemHeight) {
        return;
    }
    const auto index = static_cast<std::size_t>((point.y - NavigationTop) / NavigationItemHeight);
    page_ = Pages[index].page;
    updateSunshineControls(window);
    statusChanged();
}

void ConfigurationWindow::updateSunshineControls(HWND window)
{
    const bool sunshine = page_ == Page::Sunshine;
    if (sunshine && !hostEdit_) {
        hostEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
                                    ContentLeft + 320, ContentTop + 77, 278, 30, window, nullptr, nullptr, nullptr);
        saveButton_ = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                      ContentLeft + 320, ContentTop + 122, 92, 30, window,
                                      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(SaveHostCommandId)), nullptr, nullptr);
        testButton_ = CreateWindowExW(0, L"BUTTON", L"Test Connection", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                      ContentLeft + 422, ContentTop + 122, 176, 30, window,
                                      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(TestHostCommandId)), nullptr, nullptr);
        const StatusState state = statusProvider_ ? statusProvider_() : StatusState{};
        if (state.status && state.status->sunshineHost) SetWindowTextW(hostEdit_, widen(*state.status->sunshineHost).c_str());
    }
    if (hostEdit_) {
        ShowWindow(hostEdit_, sunshine ? SW_SHOW : SW_HIDE);
        ShowWindow(saveButton_, sunshine ? SW_SHOW : SW_HIDE);
        ShowWindow(testButton_, sunshine ? SW_SHOW : SW_HIDE);
    }
}

void ConfigurationWindow::startManagementOperation(managementipc::Command command)
{
    std::lock_guard lock(operationMutex_);
    if (operationActive_ || !managementProvider_) return;
    if (operationThread_.joinable()) operationThread_.join();
    operationActive_ = true;
    operationStatus_ = L"Working…";
    const HWND notificationWindow = window_;
    operationThread_ = std::thread([this, command = std::move(command), notificationWindow] {
        managementipc::Result result{false, "unavailable", "Management IPC is unavailable"};
        try { result = managementProvider_(command); } catch (...) {}
        {
            std::lock_guard resultLock(operationMutex_);
            operationActive_ = false;
            operationStatus_ = result.ok ? L"Success: " : L"Failed: ";
            operationStatus_ += widen(result.message);
        }
        if (notificationWindow) PostMessageW(notificationWindow, ManagementResultMessage, 0, 0);
    });
}

void ConfigurationWindow::paint(HWND window)
{
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    fill(dc, client, theme::PageBackground);

    RECT header{0, 0, client.right, HeaderHeight};
    fill(dc, header, theme::TopBar);
    const HFONT titleFont = makeFont(21, FW_BOLD);
    const HFONT sectionFont = makeFont(25, FW_BOLD);
    const HFONT navigationFont = makeFont(16, FW_BOLD);
    const HFONT labelFont = makeFont(15);
    const HFONT valueFont = makeFont(16);
    drawText(dc, L"Moonlight WebRTC", RECT{NavigationLeft, 0, client.right - 20, HeaderHeight},
             titleFont, theme::TextPrimary, DT_SINGLELINE | DT_VCENTER);

    for (std::size_t index = 0; index < Pages.size(); ++index) {
        RECT item{NavigationLeft, NavigationTop + static_cast<int>(index) * NavigationItemHeight,
                  NavigationLeft + NavigationWidth, NavigationTop + static_cast<int>(index + 1) * NavigationItemHeight - 4};
        fill(dc, item, Pages[index].page == page_ ? theme::PanelFocus : theme::PanelBackground);
        if (Pages[index].page == page_) {
            const HPEN pen = CreatePen(PS_SOLID, 2, theme::Accent);
            const HGDIOBJ oldPen = SelectObject(dc, pen);
            const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, item.left, item.top, item.right, item.bottom);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
        item.left += 16;
        drawText(dc, Pages[index].label, item, navigationFont, theme::TextPrimary, DT_SINGLELINE | DT_VCENTER);
    }

    RECT panel{ContentLeft, ContentTop, client.right - 22, client.bottom - ContentBottom};
    fill(dc, panel, theme::PanelBackground);
    const StatusState state = statusProvider_ ? statusProvider_() : StatusState{};
    const auto addRows = [&](const wchar_t* title, const std::vector<std::pair<std::wstring, std::wstring>>& rows,
                             int top = 0) {
        RECT titleArea{panel.left + 28, panel.top + 24, panel.right - 28, panel.top + 60};
        drawText(dc, title, titleArea, sectionFont, theme::TextPrimary, DT_SINGLELINE | DT_VCENTER);
        if (top == 0) top = panel.top + 78;
        for (const auto& [label, value] : rows) {
            const RECT divider{panel.left + 28, top - 8, panel.right - 28, top - 7};
            fill(dc, divider, theme::PanelBorder);
            RECT labelArea{panel.left + 28, top, panel.left + (panel.right - panel.left) / 2, top + 45};
            RECT valueArea{panel.left + (panel.right - panel.left) / 2, top, panel.right - 28, top + 45};
            drawText(dc, label, labelArea, labelFont, theme::TextSecondary, DT_SINGLELINE | DT_VCENTER);
            const COLORREF valueColor = value == L"Running" || value == L"Connected" || value == L"Paired"
                ? theme::AccentHover : value == L"Unavailable" || value == L"Disconnected" || value == L"Not paired"
                ? theme::TextMuted : theme::TextPrimary;
            drawText(dc, value, valueArea, valueFont, valueColor, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS);
            top += 58;
        }
    };

    if (page_ == Page::Status) {
        addRows(L"Status", {
            {L"Gateway", state.gatewayAvailable ? L"Running" : L"Unavailable"},
            {L"Service", L"MoonlightWebRTCGateway"},
            {L"Sunshine", state.status ? boolValue(state.status->sunshineConnected, L"Connected", L"Disconnected") : L"Unavailable"},
            {L"Session", state.status && state.status->sessionActive && *state.status->sessionActive ? L"Active" : L"Inactive"},
            {L"Connected TVs", state.status && state.status->connectedTvClients ? std::to_wstring(*state.status->connectedTvClients) : L"0"},
        });
    } else if (page_ == Page::Sunshine) {
        const RECT hostDivider{panel.left + 28, panel.top + 70, panel.right - 28, panel.top + 71};
        fill(dc, hostDivider, theme::PanelBorder);
        drawText(dc, L"Host", RECT{panel.left + 28, panel.top + 78, panel.left + (panel.right - panel.left) / 2, panel.top + 123},
                 labelFont, theme::TextSecondary, DT_SINGLELINE | DT_VCENTER);
        addRows(L"Sunshine", {
            {L"Connection", state.status ? boolValue(state.status->sunshineConnected, L"Connected", L"Disconnected") : L"Unavailable"},
            {L"Pairing", state.status ? boolValue(state.status->sunshinePaired, L"Paired", L"Not paired") : L"Unavailable"},
            {L"Running application", applicationValue(state)},
            {L"Session", state.status && state.status->sessionActive && *state.status->sessionActive ? L"Active" : L"Inactive"},
        }, panel.top + 178);
        std::wstring operation;
        {
            std::lock_guard lock(operationMutex_);
            operation = operationStatus_;
        }
        if (!operation.empty()) {
            drawText(dc, operation, RECT{panel.left + 28, panel.bottom - 52, panel.right - 28, panel.bottom - 20},
                     labelFont, theme::TextSecondary, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    } else if (page_ == Page::Network) {
        addRows(L"Network", {
            {L"Gateway address", L"All interfaces (0.0.0.0)"},
            {L"Gateway port", L"8000"},
            {L"Connected TVs", state.status && state.status->connectedTvClients ? std::to_wstring(*state.status->connectedTvClients) : L"0"},
            {L"Management IPC", L"Local named pipe"},
        });
    } else {
        addRows(L"About", {
            {L"Product", L"Moonlight WebRTC"},
            {L"Architecture", L"Gateway service + tray"},
            {L"Data location", programDataPath()},
            {L"Windows service", L"MoonlightWebRTCGateway"},
            {L"Local IPC protocol", L"Version 1"},
        });
    }

    DeleteObject(valueFont);
    DeleteObject(labelFont);
    DeleteObject(navigationFont);
    DeleteObject(sectionFont);
    DeleteObject(titleFont);
    EndPaint(window, &paint);
}

} // namespace gateway::tray
