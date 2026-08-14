#include "gateway/GatewayLifecycle.h"

namespace gateway {

void GatewayShutdownSignal::request() noexcept
{
    requested_.store(true, std::memory_order_release);
    condition_.notify_all();
}

bool GatewayShutdownSignal::requested() const noexcept
{
    return requested_.load(std::memory_order_acquire);
}

void GatewayShutdownSignal::wait()
{
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return requested(); });
}

bool GatewayShutdownSignal::waitFor(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] { return requested(); });
}

} // namespace gateway
