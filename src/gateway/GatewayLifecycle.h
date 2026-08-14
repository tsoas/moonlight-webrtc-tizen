#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace gateway {

class GatewayShutdownSignal {
public:
    void request() noexcept;
    bool requested() const noexcept;
    void wait();
    bool waitFor(std::chrono::milliseconds timeout);

private:
    std::atomic<bool> requested_ = false;
    std::mutex mutex_;
    std::condition_variable condition_;
};

} // namespace gateway
