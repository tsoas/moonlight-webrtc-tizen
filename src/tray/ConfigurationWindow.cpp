#include "tray/ConfigurationWindow.h"

#include "tray/WindowsTheme.h"

#include <dwmapi.h>

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
constexpr UINT WindowIconResourceId = 1;
constexpr int HeaderHeight = 62;
constexpr int NavigationLeft = 22;
constexpr int NavigationTop = 82;
constexpr int NavigationWidth = 170;
constexpr int NavigationItemHeight = 50;
constexpr int ContentLeft = 214;
constexpr int ContentTop = 82;
constexpr int ContentBottom = 22;
constexpr int DefaultClientWidth = 880;
constexpr int DefaultClientHeight = 590;
constexpr int MinimumClientWidth = 720;
constexpr int MinimumClientHeight = 500;
constexpr UINT SaveHostCommandId = 301;
constexpr UINT TestHostCommandId = 302;
constexpr UINT PairCommandId = 303;
constexpr UINT UnpairCommandId = 304;
constexpr UINT ManagementResultMessage = WM_APP + 31;

struct PageDefinition {
    ConfigurationWindow::Page page;
    const wchar_t* label;
};

constexpr std::array Pages{
    PageDefinition{ConfigurationWindow::Page::Status, L"Status"},
    PageDefinition{ConfigurationWindow::Page::Sunshine, L"Sunshine"},
    PageDefinition{ConfigurationWindow::Page::Network, L"Network"},
};

HFONT makeFont(int logicalHeight, int weight, UINT dpi)
{
    return CreateFontW(-MulDiv(logicalHeight, static_cast<int>(dpi), 96), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
}

RECT windowRectForClientSize(int width, int height, UINT dpi)
{
    RECT rect{0, 0, width, height};
    AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    return rect;
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
    if (state.status->runningApplicationName) return widen(*state.status->runningApplicationName);
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

} // namespace

ConfigurationWindow::~ConfigurationWindow()
{
    if (operationThread_.joinable()) operationThread_.join();
    destroyFonts();
}

void ConfigurationWindow::show(HINSTANCE instance, StatusProvider statusProvider,
                               ManagementProvider managementProvider)
{
    statusProvider_ = std::move(statusProvider);
    managementProvider_ = std::move(managementProvider);
    if (window_) {
        ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
        SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(window_);
        return;
    }

    WNDCLASSW windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(WindowIconResourceId));
    windowClass.hbrBackground = CreateSolidBrush(theme::PageBackground);
    windowClass.lpszClassName = WindowClassName;
    RegisterClassW(&windowClass);

    const UINT dpi = GetDpiForSystem();
    const RECT initialRect = windowRectForClientSize(
        MulDiv(DefaultClientWidth, static_cast<int>(dpi), 96),
        MulDiv(DefaultClientHeight, static_cast<int>(dpi), 96), dpi);
    window_ = CreateWindowExW(0, WindowClassName, L"Moonlight WebRTC",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, initialRect.right - initialRect.left,
                              initialRect.bottom - initialRect.top,
                              nullptr, nullptr, instance, this);
    if (window_) {
        enableDarkTitleBar(window_);
        const HICON smallIcon = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(WindowIconResourceId), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED));
        const HICON largeIcon = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(WindowIconResourceId), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED));
        SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
        SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
        ShowWindow(window_, SW_SHOWNORMAL);
        SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(window_);
    }
}

void ConfigurationWindow::close()
{
    if (window_) DestroyWindow(window_);
}

void ConfigurationWindow::statusChanged()
{
    if (window_) {
        updatePairingControls();
        InvalidateRect(window_, nullptr, FALSE);
    }
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
    case WM_CREATE:
        applyDpi(window, GetDpiForWindow(window));
        return 0;
    case WM_DPICHANGED: {
        const UINT dpi = HIWORD(wParam);
        const auto* recommended = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, recommended->left, recommended->top,
                     recommended->right - recommended->left, recommended->bottom - recommended->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        applyDpi(window, dpi);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* minMax = reinterpret_cast<MINMAXINFO*>(lParam);
        const RECT minimum = windowRectForClientSize(scale(MinimumClientWidth), scale(MinimumClientHeight), dpi_);
        minMax->ptMinTrackSize.x = minimum.right - minimum.left;
        minMax->ptMinTrackSize.y = minimum.bottom - minimum.top;
        return 0;
    }
    case WM_SIZE:
        layoutControls(window);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
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
        if (LOWORD(wParam) == PairCommandId) {
            startManagementOperation({managementipc::CommandType::Pair, {}});
            return 0;
        }
        if (LOWORD(wParam) == UnpairCommandId) {
            if (MessageBoxW(window, L"Remove this Gateway's local Sunshine trust? Sunshine does not provide remote revocation. This keeps the Gateway identity and other hosts.",
                            L"Unpair Sunshine", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES) {
                startManagementOperation({managementipc::CommandType::Unpair, {}});
            }
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT: {
        SetTextColor(reinterpret_cast<HDC>(wParam), theme::TextPrimary);
        SetBkColor(reinterpret_cast<HDC>(wParam), theme::PanelFocus);
        static const HBRUSH brush = CreateSolidBrush(theme::PanelFocus);
        return reinterpret_cast<LRESULT>(brush);
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item->CtlType != ODT_BUTTON) break;
        const bool disabled = (item->itemState & ODS_DISABLED) != 0;
        const bool pressed = (item->itemState & ODS_SELECTED) != 0;
        fill(item->hDC, item->rcItem, pressed ? theme::PanelBorder : theme::PanelFocus);
        const HBRUSH border = CreateSolidBrush(disabled ? theme::PanelBorder : theme::Accent);
        FrameRect(item->hDC, &item->rcItem, border);
        DeleteObject(border);
        wchar_t text[128]{};
        GetWindowTextW(item->hwndItem, text, static_cast<int>(std::size(text)));
        RECT textArea = item->rcItem;
        drawText(item->hDC, text, textArea, controlFont_,
                 disabled ? theme::TextMuted : theme::TextPrimary, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        if (item->itemState & ODS_FOCUS) {
            InflateRect(&textArea, -4, -4);
            DrawFocusRect(item->hDC, &textArea);
        }
        return TRUE;
    }
    case ManagementResultMessage:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        window_ = nullptr;
        hostEdit_ = saveButton_ = testButton_ = pairButton_ = unpairButton_ = nullptr;
        destroyFonts();
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ConfigurationWindow::selectPageFromPoint(HWND window, POINT point)
{
    if (point.x < scale(NavigationLeft) || point.x >= scale(NavigationLeft + NavigationWidth)
        || point.y < scale(NavigationTop) || point.y >= scale(NavigationTop + static_cast<int>(Pages.size()) * NavigationItemHeight)) {
        return;
    }
    const auto index = static_cast<std::size_t>((point.y - scale(NavigationTop)) / scale(NavigationItemHeight));
    page_ = Pages[index].page;
    updateSunshineControls(window);
    statusChanged();
}

void ConfigurationWindow::updateSunshineControls(HWND window)
{
    const bool sunshine = page_ == Page::Sunshine;
    if (sunshine && !hostEdit_) {
        hostEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
                                    0, 0, 0, 0, window, nullptr, nullptr, nullptr);
        saveButton_ = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                      0, 0, 0, 0, window,
                                      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(SaveHostCommandId)), nullptr, nullptr);
        testButton_ = CreateWindowExW(0, L"BUTTON", L"Test Connection", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                      0, 0, 0, 0, window,
                                      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(TestHostCommandId)), nullptr, nullptr);
        pairButton_ = CreateWindowExW(0, L"BUTTON", L"Pair", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                      0, 0, 0, 0, window,
                                      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(PairCommandId)), nullptr, nullptr);
        unpairButton_ = CreateWindowExW(0, L"BUTTON", L"Unpair", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                        0, 0, 0, 0, window,
                                        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(UnpairCommandId)), nullptr, nullptr);
        applyDpi(window, GetDpiForWindow(window));
        layoutControls(window);
        const StatusState state = statusProvider_ ? statusProvider_() : StatusState{};
        if (state.status && state.status->sunshineHost) SetWindowTextW(hostEdit_, widen(*state.status->sunshineHost).c_str());
    }
    if (hostEdit_) {
        ShowWindow(hostEdit_, sunshine ? SW_SHOW : SW_HIDE);
        ShowWindow(saveButton_, sunshine ? SW_SHOW : SW_HIDE);
        ShowWindow(testButton_, sunshine ? SW_SHOW : SW_HIDE);
        updatePairingControls();
    }
}

int ConfigurationWindow::scale(int logicalPixels) const
{
    return MulDiv(logicalPixels, static_cast<int>(dpi_), 96);
}

void ConfigurationWindow::destroyFonts()
{
    const auto destroy = [](HFONT& font) {
        if (font) DeleteObject(font);
        font = nullptr;
    };
    destroy(titleFont_);
    destroy(sectionFont_);
    destroy(navigationFont_);
    destroy(labelFont_);
    destroy(valueFont_);
    destroy(controlFont_);
}

void ConfigurationWindow::recreateFonts()
{
    destroyFonts();
    titleFont_ = makeFont(21, FW_BOLD, dpi_);
    sectionFont_ = makeFont(25, FW_BOLD, dpi_);
    navigationFont_ = makeFont(16, FW_BOLD, dpi_);
    labelFont_ = makeFont(15, FW_NORMAL, dpi_);
    valueFont_ = makeFont(16, FW_NORMAL, dpi_);
    controlFont_ = makeFont(15, FW_SEMIBOLD, dpi_);
}

void ConfigurationWindow::applyDpi(HWND window, UINT dpi)
{
    dpi_ = dpi ? dpi : 96;
    recreateFonts();
    for (HWND control : {hostEdit_, saveButton_, testButton_, pairButton_, unpairButton_}) {
        if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(controlFont_), TRUE);
    }
    layoutControls(window);
    InvalidateRect(window, nullptr, FALSE);
}

void ConfigurationWindow::layoutControls(HWND window)
{
    if (!hostEdit_) return;
    RECT client{};
    GetClientRect(window, &client);
    const int panelLeft = scale(ContentLeft);
    const int panelRight = client.right - scale(22);
    const int valueLeft = panelLeft + (panelRight - panelLeft) / 2;
    const int rightMargin = scale(28);
    const int controlRight = panelRight - rightMargin;
    const int controlWidth = (std::max)(scale(180), controlRight - valueLeft);
    const int controlHeight = scale(30);
    const int editTop = scale(ContentTop + 77);
    const int buttonTop = scale(ContentTop + 122);
    const int pairingTop = scale(ContentTop + 162);
    const int saveWidth = scale(92);
    const int gap = scale(10);
    MoveWindow(hostEdit_, valueLeft, editTop, controlWidth, controlHeight, TRUE);
    MoveWindow(saveButton_, valueLeft, buttonTop, saveWidth, controlHeight, TRUE);
    MoveWindow(testButton_, valueLeft + saveWidth + gap, buttonTop,
               (std::max)(scale(120), controlWidth - saveWidth - gap), controlHeight, TRUE);
    MoveWindow(pairButton_, valueLeft, pairingTop, saveWidth, controlHeight, TRUE);
    MoveWindow(unpairButton_, valueLeft + saveWidth + gap, pairingTop,
               (std::max)(scale(120), controlWidth - saveWidth - gap), controlHeight, TRUE);
}

void ConfigurationWindow::updatePairingControls()
{
    if (!pairButton_) return;
    const StatusState state = statusProvider_ ? statusProvider_() : StatusState{};
    const bool paired = state.status && state.status->sunshinePaired && *state.status->sunshinePaired;
    const bool visible = page_ == Page::Sunshine && state.gatewayAvailable;
    ShowWindow(pairButton_, visible && !paired ? SW_SHOW : SW_HIDE);
    ShowWindow(unpairButton_, visible && paired ? SW_SHOW : SW_HIDE);
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
        if (command.type == managementipc::CommandType::Pair && result.ok
            && result.code == "pairing-started" && result.pin) {
            {
                std::lock_guard resultLock(operationMutex_);
                operationStatus_ = L"PIN " + widen(*result.pin)
                    + L": enter it in Sunshine. Pairing in progress…";
            }
            if (notificationWindow) PostMessageW(notificationWindow, ManagementResultMessage, 0, 0);
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                try { result = managementProvider_({managementipc::CommandType::PairStatus, {}}); } catch (...) { result = {false, "unavailable", "Management IPC is unavailable"}; }
                if (result.code != "pairing-in-progress") break;
            }
        }
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

    RECT header{0, 0, client.right, scale(HeaderHeight)};
    fill(dc, header, theme::TopBar);
    drawText(dc, L"Moonlight WebRTC", RECT{scale(NavigationLeft), 0, client.right - scale(20), scale(HeaderHeight)},
             titleFont_, theme::TextPrimary, DT_SINGLELINE | DT_VCENTER);

    for (std::size_t index = 0; index < Pages.size(); ++index) {
        RECT item{scale(NavigationLeft), scale(NavigationTop + static_cast<int>(index) * NavigationItemHeight),
                  scale(NavigationLeft + NavigationWidth), scale(NavigationTop + static_cast<int>(index + 1) * NavigationItemHeight - 4)};
        fill(dc, item, Pages[index].page == page_ ? theme::PanelFocus : theme::PanelBackground);
        if (Pages[index].page == page_) {
            const HPEN pen = CreatePen(PS_SOLID, scale(2), theme::Accent);
            const HGDIOBJ oldPen = SelectObject(dc, pen);
            const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, item.left, item.top, item.right, item.bottom);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
        item.left += scale(16);
        drawText(dc, Pages[index].label, item, navigationFont_, theme::TextPrimary, DT_SINGLELINE | DT_VCENTER);
    }

    RECT panel{scale(ContentLeft), scale(ContentTop), client.right - scale(22), client.bottom - scale(ContentBottom)};
    fill(dc, panel, theme::PanelBackground);
    const StatusState state = statusProvider_ ? statusProvider_() : StatusState{};
    const auto addRows = [&](const wchar_t* title, const std::vector<std::pair<std::wstring, std::wstring>>& rows,
                             int top = 0) {
        RECT titleArea{panel.left + scale(28), panel.top + scale(24), panel.right - scale(28), panel.top + scale(60)};
        drawText(dc, title, titleArea, sectionFont_, theme::TextPrimary, DT_SINGLELINE | DT_VCENTER);
        if (top == 0) top = panel.top + scale(78);
        for (const auto& [label, value] : rows) {
            const RECT divider{panel.left + scale(28), top - scale(8), panel.right - scale(28), top - scale(7)};
            fill(dc, divider, theme::PanelBorder);
            RECT labelArea{panel.left + scale(28), top, panel.left + (panel.right - panel.left) / 2, top + scale(45)};
            RECT valueArea{panel.left + (panel.right - panel.left) / 2, top, panel.right - scale(28), top + scale(45)};
            drawText(dc, label, labelArea, labelFont_, theme::TextSecondary, DT_SINGLELINE | DT_VCENTER);
            const COLORREF valueColor = value == L"Running" || value == L"Connected" || value == L"Paired"
                ? theme::AccentHover : value == L"Unavailable" || value == L"Disconnected" || value == L"Not paired"
                ? theme::TextMuted : theme::TextPrimary;
            drawText(dc, value, valueArea, valueFont_, valueColor, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS);
            top += scale(58);
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
        const RECT hostDivider{panel.left + scale(28), panel.top + scale(70), panel.right - scale(28), panel.top + scale(71)};
        fill(dc, hostDivider, theme::PanelBorder);
        drawText(dc, L"Host", RECT{panel.left + scale(28), panel.top + scale(78), panel.left + (panel.right - panel.left) / 2, panel.top + scale(123)},
                 labelFont_, theme::TextSecondary, DT_SINGLELINE | DT_VCENTER);
        addRows(L"Sunshine", {
            {L"Connection", state.status ? boolValue(state.status->sunshineConnected, L"Connected", L"Disconnected") : L"Unavailable"},
            {L"Pairing", state.status ? boolValue(state.status->sunshinePaired, L"Paired", L"Not paired") : L"Unavailable"},
            {L"Running application", applicationValue(state)},
            {L"Session", state.status && state.status->sessionActive && *state.status->sessionActive ? L"Active" : L"Inactive"},
        }, panel.top + scale(218));
        std::wstring operation;
        {
            std::lock_guard lock(operationMutex_);
            operation = operationStatus_;
        }
        if (!operation.empty()) {
            drawText(dc, operation, RECT{panel.left + scale(28), panel.bottom - scale(28), panel.right - scale(28), panel.bottom - scale(6)},
                     labelFont_, theme::TextSecondary, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    } else if (page_ == Page::Network) {
        addRows(L"Network", {
            {L"Gateway address", L"All interfaces (0.0.0.0)"},
            {L"Gateway port", L"8000"},
            {L"Connected TVs", state.status && state.status->connectedTvClients ? std::to_wstring(*state.status->connectedTvClients) : L"0"},
        });
    }

    EndPaint(window, &paint);
}

} // namespace gateway::tray
