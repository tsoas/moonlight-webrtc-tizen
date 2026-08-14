#pragma once

#include "tray/StatusMonitor.h"

#include <windows.h>

#include <functional>

namespace gateway::tray {

class ConfigurationWindow {
public:
    using StatusProvider = std::function<StatusState()>;
    enum class Page { Status, Sunshine, Network, About };

    void show(HINSTANCE instance, StatusProvider statusProvider);
    void close();
    void statusChanged();

private:
    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void paint(HWND window);
    void selectPageFromPoint(POINT point);

    HWND window_ = nullptr;
    StatusProvider statusProvider_;
    Page page_ = Page::Status;
};

} // namespace gateway::tray
