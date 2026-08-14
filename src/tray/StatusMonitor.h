#pragma once

#include "gateway/ServiceIpcProtocol.h"

#include <windows.h>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace gateway::tray {

inline constexpr UINT StatusChangedMessage = WM_APP + 2;

struct StatusState {
    std::optional<serviceipc::StatusSnapshot> status;
    bool gatewayAvailable = false;
};

class StatusMonitor {
public:
    explicit StatusMonitor(HWND notificationWindow);
    ~StatusMonitor();

    StatusMonitor(const StatusMonitor&) = delete;
    StatusMonitor& operator=(const StatusMonitor&) = delete;

    void start();
    void stop();
    StatusState snapshot() const;

private:
    void run();

    HWND notificationWindow_ = nullptr;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    StatusState state_;
    bool stopRequested_ = false;
    std::thread thread_;
};

} // namespace gateway::tray
