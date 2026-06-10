#pragma once

#include "core/json.h"

#include <string>
#include <vector>

namespace ww {

struct NetworkIdentity {
    std::string type;
    std::string id;
    std::string name;
};

struct NetworkStatus {
    bool adapter_available = false;
    bool connected = false;
    bool wifi_available = false;
    bool wired_available = false;
    std::string type;
    std::string id;
    std::string name;
    std::string interface_description;
    std::string error;
};

struct WiredAdapter {
    std::string id;
    std::string name;
    bool connected = false;
    bool enabled = true;
    std::string status;
};

struct NetworkActionResult {
    bool ok = false;
    bool requested = false;
    bool elevated = false;
    std::string status;
    std::string target_id;
    std::string error;
};

NetworkStatus getCurrentNetwork();
std::vector<NetworkIdentity> getActiveNetworks();
bool isNoNetworkAdapter(const NetworkStatus& status);
std::vector<WiredAdapter> listWiredAdapters();
NetworkActionResult setWiredAdapterEnabled(const std::string& adapterId, bool enabled);

JsonValue networkStatusJson(const NetworkStatus& status);
JsonValue wiredAdaptersJson();
JsonValue networkActionJson(const NetworkActionResult& result);

}
