#include "platform/WindowsDisplayState.h"

#include <sstream>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace gateway::platform {

std::optional<WindowsDisplayState> primaryWindowsDisplayState()
{
#ifndef _WIN32
    return std::nullopt;
#else
    DISPLAY_DEVICEA primary{};
    primary.cb = sizeof(primary);
    bool foundPrimary = false;
    for (DWORD index = 0; EnumDisplayDevicesA(nullptr, index, &primary, 0); ++index) {
        if ((primary.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0) {
            foundPrimary = true;
            break;
        }
        primary = {};
        primary.cb = sizeof(primary);
    }
    if (!foundPrimary) {
        return std::nullopt;
    }

    DEVMODEA mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsA(primary.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
        return std::nullopt;
    }

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount)
        != ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
                           &pathCount,
                           paths.data(),
                           &modeCount,
                           modes.data(),
                           nullptr)
        != ERROR_SUCCESS) {
        return std::nullopt;
    }

    for (UINT32 index = 0; index < pathCount; ++index) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[index].sourceInfo.adapterId;
        source.header.id = paths[index].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
            continue;
        }

        const int required = MultiByteToWideChar(
            CP_ACP, 0, primary.DeviceName, -1, nullptr, 0);
        if (required <= 0) {
            continue;
        }
        std::wstring primaryName(static_cast<std::size_t>(required), L'\0');
        MultiByteToWideChar(
            CP_ACP, 0, primary.DeviceName, -1, primaryName.data(), required);
        if (_wcsicmp(source.viewGdiDeviceName, primaryName.c_str()) != 0) {
            continue;
        }

        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
        color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        color.header.size = sizeof(color);
        color.header.adapterId = paths[index].targetInfo.adapterId;
        color.header.id = paths[index].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&color.header) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        return WindowsDisplayState{
            primary.DeviceName,
            static_cast<int>(mode.dmPelsWidth),
            static_cast<int>(mode.dmPelsHeight),
            static_cast<int>(mode.dmDisplayFrequency),
            color.advancedColorSupported != 0,
            color.advancedColorEnabled != 0,
        };
    }
    return std::nullopt;
#endif
}

std::string formatWindowsDisplayState(const WindowsDisplayState& state)
{
    std::ostringstream output;
    output << state.deviceName << ' ' << state.width << 'x' << state.height << " @ "
           << state.refreshRate << " Hz, HDR "
           << (state.hdrEnabled ? "ENABLED" : "DISABLED") << " (supported: "
           << (state.hdrSupported ? "yes" : "no") << ')';
    return output.str();
}

} // namespace gateway::platform
