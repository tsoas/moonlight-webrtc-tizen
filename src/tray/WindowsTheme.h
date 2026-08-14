#pragma once

#include <windows.h>

namespace gateway::tray::theme {

// Mirrors the current Tizen ui.css product palette.
inline constexpr COLORREF TopBar = RGB(5, 5, 5);
inline constexpr COLORREF PageBackground = RGB(107, 107, 107);
inline constexpr COLORREF PanelBackground = RGB(27, 27, 29);
inline constexpr COLORREF PanelFocus = RGB(32, 32, 35);
inline constexpr COLORREF PanelBorder = RGB(48, 48, 52);
inline constexpr COLORREF TextPrimary = RGB(255, 255, 255);
inline constexpr COLORREF TextSecondary = RGB(198, 196, 203);
inline constexpr COLORREF TextMuted = RGB(162, 159, 168);
inline constexpr COLORREF Accent = RGB(173, 103, 255);
inline constexpr COLORREF AccentHover = RGB(194, 138, 255);
inline constexpr COLORREF Danger = RGB(239, 102, 124);

} // namespace gateway::tray::theme
