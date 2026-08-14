#include "tray/StatusMonitor.h"

#include "tray/ServiceIpcClient.h"

#include <chrono>

namespace gateway::tray {

StatusMonitor::StatusMonitor(HWND notificationWindow)
    : notificationWindow_(notificationWindow)
{
}

StatusMonitor::~StatusMonitor()
{
    stop();
}

void StatusMonitor::start()
{
    if (!thread_.joinable()) {
        thread_ = std::thread([this] { run(); });
    }
}

void StatusMonitor::stop()
{
    {
        const std::lock_guard lock(mutex_);
        stopRequested_ = true;
    }
    condition_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

StatusState StatusMonitor::snapshot() const
{
    const std::lock_guard lock(mutex_);
    return state_;
}

void StatusMonitor::run()
{
    for (;;) {
        StatusState next;
        try {
            next.status = requestServiceStatus();
            next.gatewayAvailable = next.status->serviceRunning;
        } catch (...) {
            next.status.reset();
        }
        {
            const std::lock_guard lock(mutex_);
            if (stopRequested_) {
                return;
            }
            state_ = std::move(next);
        }
        PostMessageW(notificationWindow_, StatusChangedMessage, 0, 0);

        std::unique_lock lock(mutex_);
        if (condition_.wait_for(lock, std::chrono::seconds(3), [this] { return stopRequested_; })) {
            return;
        }
    }
}

} // namespace gateway::tray
