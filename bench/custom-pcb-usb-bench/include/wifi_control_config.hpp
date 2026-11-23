#pragma once

#if defined(__has_include)
#if __has_include("wifi_control_config.local.hpp")
#include "wifi_control_config.local.hpp"
#endif
#endif

// Wi-Fi and its unauthenticated HTTP control surface are disabled in every
// tracked build environment. A developer who explicitly enables them can
// supply credentials through build macros or an ignored local header copied
// from wifi_control_config.local.example.hpp.
#ifndef RF3_WIFI_SSID
#define RF3_WIFI_SSID ""
#endif

#ifndef RF3_WIFI_PASSWORD
#define RF3_WIFI_PASSWORD ""
#endif

// The board-specific node name is set from the PlatformIO environment so each
// board gets a stable hostname on the network.
#ifndef RF3_NODE_NAME
#define RF3_NODE_NAME "rf3-node"
#endif

namespace WifiControlConfig {
inline constexpr char kSsid[] = RF3_WIFI_SSID;
inline constexpr char kPassword[] = RF3_WIFI_PASSWORD;
inline constexpr char kNodeName[] = RF3_NODE_NAME;
inline constexpr uint16_t kHttpPort = 80;
}  // namespace WifiControlConfig
