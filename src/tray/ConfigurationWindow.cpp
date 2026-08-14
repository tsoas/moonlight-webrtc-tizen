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

void ConfigurationWindow::show(HINSTANCE instance, StatusProvider statusProvider)
{
    statusProvider_ = std::move(statusProvider);
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
        selectPageFromPoint(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        window_ = nullptr;
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void ConfigurationWindow::selectPageFromPoint(POINT point)
{
    if (point.x < NavigationLeft || point.x >= NavigationLeft + NavigationWidth
        || point.y < NavigationTop || point.y >= NavigationTop + static_cast<int>(Pages.size()) * NavigationItemHeight) {
        return;
    }
    const auto index = static_cast<std::size_t>((point.y - NavigationTop) / NavigationItemHeight);
    page_ = Pages[index].page;
    statusChanged();
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
    const auto addRows = [&](const wchar_t* title, const std::vector<std::pair<std::wstring, std::wstring>>& rows) {
        RECT titleArea{panel.left + 28, panel.top + 24, panel.right - 28, panel.top + 60};
        drawText(dc, title, titleArea, sectionFont, theme::TextPrimary, DT_SINGLELINE | DT_VCENTER);
        int top = panel.top + 78;
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
        addRows(L"Sunshine", {
            {L"Host", state.status && state.status->sunshineHost ? widen(*state.status->sunshineHost) : L"Unavailable"},
            {L"Connection", state.status ? boolValue(state.status->sunshineConnected, L"Connected", L"Disconnected") : L"Unavailable"},
            {L"Pairing", state.status ? boolValue(state.status->sunshinePaired, L"Paired", L"Not paired") : L"Unavailable"},
            {L"Running application", applicationValue(state)},
            {L"Session", state.status && state.status->sessionActive && *state.status->sessionActive ? L"Active" : L"Inactive"},
        });
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
