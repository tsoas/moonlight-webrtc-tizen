#pragma once

#include <optional>
#include <string>

namespace gateway::platform {

struct WindowsDisplayState {
    std::string deviceName;
    int width;
    int height;
    int refreshRate;
    bool hdrSupported;
    bool hdrEnabled;
};

std::optional<WindowsDisplayState> primaryWindowsDisplayState();
std::string formatWindowsDisplayState(const WindowsDisplayState& state);

} // namespace gateway::platform
