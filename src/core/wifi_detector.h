#pragma once

#include <optional>
#include <string>
#include <vector>

namespace ww {

struct WifiStatus {
    bool adapter_available = false;
    bool connected = false;
    std::string ssid;
    std::string interface_description;
    std::string error;
};

WifiStatus getCurrentWifi();
bool isNoWifiAdapter(const WifiStatus& status);
std::vector<std::string> getKnownWifiProfiles();

}
