#pragma once

#include "core/json.h"

#include <string>
#include <vector>

namespace ww {

struct AvailableWifiNetwork {
    std::string ssid;
    int signal_quality = 0;
    bool connected = false;
    bool secure = true;
    std::string auth;
};

struct WifiConnectResult {
    bool connected = false;
    bool native_dialog_opened = false;
    bool connect_requested = false;
    std::string status;
    std::string error;
};

std::vector<AvailableWifiNetwork> listAvailableWifiNetworks();
WifiConnectResult connectToWifiDetailed(const std::string& ssid, const std::string& password = "");
bool connectToWifi(const std::string& ssid, std::string* error = nullptr);
JsonValue availableNetworksJson();

}
