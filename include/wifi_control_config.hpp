#pragma once

// Update these credentials to match the Wi-Fi network both ESP32 boards should
// join for HTTP control from your computer.
#ifndef RF3_WIFI_SSID
#define RF3_WIFI_SSID "Dalton's iPhone"
#endif

#ifndef RF3_WIFI_PASSWORD
#define RF3_WIFI_PASSWORD "hi12345678"
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
