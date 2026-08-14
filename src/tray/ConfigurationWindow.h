#pragma once

#include "tray/StatusMonitor.h"
#include "gateway/ManagementIpcProtocol.h"

#include <windows.h>

#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace gateway::tray {

class ConfigurationWindow {
public:
    using StatusProvider = std::function<StatusState()>;
    using ManagementProvider = std::function<managementipc::Result(const managementipc::Command&)>;
    enum class Page { Status, Sunshine, Network };

    ~ConfigurationWindow();
    void show(HINSTANCE instance, StatusProvider statusProvider, ManagementProvider managementProvider);
    void close();
    void statusChanged();

private:
    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void paint(HWND window);
    void selectPageFromPoint(HWND window, POINT point);
    void updateSunshineControls(HWND window);
    void applyDpi(HWND window, UINT dpi);
    void layoutControls(HWND window);
    void recreateFonts();
    void destroyFonts();
    int scale(int logicalPixels) const;
    void startManagementOperation(managementipc::Command command);
    void updatePairingControls();

    HWND window_ = nullptr;
    HWND hostEdit_ = nullptr;
    HWND saveButton_ = nullptr;
    HWND testButton_ = nullptr;
    HWND pairButton_ = nullptr;
    HWND unpairButton_ = nullptr;
    UINT dpi_ = 96;
    HFONT titleFont_ = nullptr;
    HFONT sectionFont_ = nullptr;
    HFONT navigationFont_ = nullptr;
    HFONT labelFont_ = nullptr;
    HFONT valueFont_ = nullptr;
    HFONT controlFont_ = nullptr;
    StatusProvider statusProvider_;
    ManagementProvider managementProvider_;
    Page page_ = Page::Status;
    std::mutex operationMutex_;
    std::thread operationThread_;
    bool operationActive_ = false;
    std::wstring operationStatus_;
};

} // namespace gateway::tray
