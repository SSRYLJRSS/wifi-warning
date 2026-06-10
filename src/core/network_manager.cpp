#include "core/network_manager.h"

#include "core/util.h"
#include "core/wifi_detector.h"

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ww {

static std::string trimAscii(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) ++start;
    return start ? value.substr(start) : value;
}

static std::optional<NetworkStatus> testNetworkOverride() {
    std::wstring idWide = getEnvWide(L"WW_TEST_CURRENT_NETWORK_ID");
    std::wstring typeWide = getEnvWide(L"WW_TEST_CURRENT_NETWORK_TYPE");
    if (idWide.empty() && typeWide.empty()) return std::nullopt;

    NetworkStatus status;
    status.type = typeWide.empty() ? "wired" : wideToUtf8(typeWide);
    std::string id = trimAscii(wideToUtf8(idWide));
    if (id == "<disconnected>") {
        status.adapter_available = true;
        status.wired_available = status.type == "wired";
        status.wifi_available = status.type == "wifi";
        status.connected = false;
        return status;
    }
    if (id == "<no-adapter>") {
        status.error = "No network adapter";
        return status;
    }
    status.adapter_available = true;
    status.connected = !id.empty();
    status.id = id;
    status.name = id;
    status.interface_description = id;
    status.wired_available = status.type == "wired";
    status.wifi_available = status.type == "wifi";
    return status;
}

static std::vector<WiredAdapter> testWiredAdapters() {
    std::wstring jsonWide = getEnvWide(L"WW_TEST_WIRED_ADAPTERS_JSON");
    std::vector<WiredAdapter> adapters;
    if (jsonWide.empty()) return adapters;

    std::string error;
    auto parsed = parseJson(wideToUtf8(jsonWide), &error);
    if (!parsed) return adapters;
    for (const auto& item : parsed->asArray()) {
        WiredAdapter adapter;
        if (const auto* value = item.get("id")) adapter.id = value->asString();
        if (const auto* value = item.get("name")) adapter.name = value->asString();
        if (const auto* value = item.get("connected")) adapter.connected = value->asBool();
        if (const auto* value = item.get("enabled")) adapter.enabled = value->asBool(true);
        if (const auto* value = item.get("status")) adapter.status = value->asString();
        if (adapter.name.empty()) adapter.name = adapter.id;
        if (adapter.id.empty()) adapter.id = adapter.name;
        if (!adapter.id.empty()) adapters.push_back(adapter);
    }
    return adapters;
}

static std::string operStatusString(IF_OPER_STATUS status) {
    switch (status) {
        case IfOperStatusUp: return "up";
        case IfOperStatusDown: return "down";
        case IfOperStatusTesting: return "testing";
        case IfOperStatusDormant: return "dormant";
        case IfOperStatusNotPresent: return "not_present";
        case IfOperStatusLowerLayerDown: return "lower_layer_down";
        default: return "unknown";
    }
}

std::vector<WiredAdapter> listWiredAdapters() {
    auto testAdapters = testWiredAdapters();
    if (!testAdapters.empty()) return testAdapters;

    std::vector<WiredAdapter> adapters;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    IP_ADAPTER_ADDRESSES* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &size);
    }
    if (result != NO_ERROR) return adapters;

    for (auto* item = addresses; item; item = item->Next) {
        if (item->IfType != IF_TYPE_ETHERNET_CSMACD) continue;
        WiredAdapter adapter;
        adapter.name = item->FriendlyName ? wideToUtf8(item->FriendlyName) : "";
        if (!adapter.name.empty()) {
            adapter.id = adapter.name;
        } else if (item->AdapterName) {
            adapter.id = item->AdapterName;
        }
        adapter.connected = item->OperStatus == IfOperStatusUp && item->FirstUnicastAddress != nullptr;
        adapter.enabled = item->OperStatus != IfOperStatusNotPresent;
        adapter.status = operStatusString(item->OperStatus);
        if (!adapter.id.empty()) adapters.push_back(adapter);
    }
    return adapters;
}

NetworkStatus getCurrentNetwork() {
    if (auto test = testNetworkOverride()) return *test;

    NetworkStatus status;
    auto wifi = getCurrentWifi();
    status.wifi_available = wifi.adapter_available;
    if (wifi.connected && !wifi.ssid.empty()) {
        status.adapter_available = true;
        status.connected = true;
        status.type = "wifi";
        status.id = wifi.ssid;
        status.name = wifi.ssid;
        status.interface_description = wifi.interface_description;
        return status;
    }

    for (const auto& adapter : listWiredAdapters()) {
        status.wired_available = true;
        if (!adapter.connected) continue;
        status.adapter_available = true;
        status.connected = true;
        status.type = "wired";
        status.id = adapter.id;
        status.name = adapter.name.empty() ? adapter.id : adapter.name;
        status.interface_description = status.name;
        return status;
    }

    status.adapter_available = status.wifi_available || status.wired_available;
    status.error = status.adapter_available ? "No connected network" : "No network adapter";
    return status;
}


std::vector<NetworkIdentity> getActiveNetworks() {
    std::vector<NetworkIdentity> networks;
    if (auto test = testNetworkOverride()) {
        if (test->connected && !test->id.empty()) {
            networks.push_back({test->type, test->id, test->name});
        }
        return networks;
    }

    // Collect ALL active networks (WiFi + wired)
    auto wifi = getCurrentWifi();
    if (wifi.connected && !wifi.ssid.empty()) {
        networks.push_back({"wifi", wifi.ssid, wifi.ssid});
    }

    for (const auto& adapter : listWiredAdapters()) {
        if (adapter.connected && !adapter.id.empty()) {
            networks.push_back({"wired", adapter.id, adapter.name.empty() ? adapter.id : adapter.name});
        }
    }
    return networks;
}

bool isNoNetworkAdapter(const NetworkStatus& status) {
    return !status.adapter_available && (status.error.empty() || status.error == "No network adapter");
}

static std::wstring quoteNetshName(const std::string& value) {
    std::wstring wide = utf8ToWide(value);
    std::wstring out = L"\"";
    for (wchar_t ch : wide) {
        if (ch == L'"') out += L'\\';
        out += ch;
    }
    out += L"\"";
    return out;
}

NetworkActionResult setWiredAdapterEnabled(const std::string& adapterId, bool enabled) {
    NetworkActionResult result;
    result.target_id = adapterId;
    if (adapterId.empty()) {
        result.status = "failed";
        result.error = "missing adapter id";
        return result;
    }

    std::wstring testAction = getEnvWide(L"WW_TEST_WIRED_ACTION");
    if (!testAction.empty()) {
        std::string mode = wideToUtf8(testAction);
        result.ok = mode == "success" || mode == adapterId;
        result.requested = result.ok;
        result.status = result.ok ? (enabled ? "enable_requested" : "disable_requested") : "failed";
        result.error = result.ok ? "" : "WW_TEST_WIRED_ACTION requested failure";
        return result;
    }

    std::wstring args = L"interface set interface name=" + quoteNetshName(adapterId)
        + L" admin=" + (enabled ? L"enabled" : L"disabled");
    HINSTANCE launch = ShellExecuteW(nullptr, L"runas", L"netsh.exe", args.c_str(), nullptr, SW_HIDE);
    intptr_t code = reinterpret_cast<intptr_t>(launch);
    result.ok = code > 32;
    result.requested = result.ok;
    result.elevated = result.ok;
    result.status = result.ok ? (enabled ? "enable_requested" : "disable_requested") : "failed";
    if (!result.ok) result.error = "could not start elevated netsh";
    return result;
}

JsonValue networkStatusJson(const NetworkStatus& status) {
    return JsonValue::Object{
        {"ok", true},
        {"adapter_available", status.adapter_available},
        {"connected", status.connected},
        {"wifi_available", status.wifi_available},
        {"wired_available", status.wired_available},
        {"type", status.type},
        {"id", status.id},
        {"name", status.name},
        {"interface", status.interface_description},
        {"error", status.error}
    };
}

JsonValue wiredAdaptersJson() {
    JsonValue::Array array;
    for (const auto& adapter : listWiredAdapters()) {
        array.push_back(JsonValue::Object{
            {"id", adapter.id},
            {"name", adapter.name},
            {"connected", adapter.connected},
            {"enabled", adapter.enabled},
            {"status", adapter.status}
        });
    }
    return array;
}

JsonValue networkActionJson(const NetworkActionResult& result) {
    return JsonValue::Object{
        {"ok", result.ok},
        {"requested", result.requested},
        {"elevated", result.elevated},
        {"status", result.status},
        {"target_id", result.target_id},
        {"error", result.error}
    };
}

}
