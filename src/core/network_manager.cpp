#include "core/network_manager.h"

#include "core/util.h"
#include "core/wifi_detector.h"

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <stdio.h>

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

    // Pass 1: GetAdaptersAddresses for connected adapters (rich info)
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_ALL_INTERFACES;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    IP_ADAPTER_ADDRESSES* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &size);
    }
    if (result == NO_ERROR) {
        for (auto* item = addresses; item; item = item->Next) {
            if (item->IfType != IF_TYPE_ETHERNET_CSMACD) continue;
            // Skip non-ethernet adapters by common name patterns
            if (item->FriendlyName) {
                std::wstring gaaName(item->FriendlyName);
                std::wstring gaaLower = gaaName;
                for (auto& ch : gaaLower) ch = towlower(ch);
                if (gaaLower.find(L"wi-fi") != std::wstring::npos ||
                    gaaLower.find(L"bluetooth") != std::wstring::npos ||
                    gaaLower.find(L"蓝牙") != std::wstring::npos ||
                    gaaLower.find(L"wlan") != std::wstring::npos) continue;
            }
            WiredAdapter adapter;
            adapter.name = item->FriendlyName ? wideToUtf8(item->FriendlyName) : "";
            if (!adapter.name.empty()) {
                adapter.id = adapter.name;
            } else if (item->AdapterName) {
                adapter.id = item->AdapterName;
            }
            adapter.connected = item->OperStatus == IfOperStatusUp && item->FirstUnicastAddress != nullptr;
            // Note: enabled status will be corrected by netsh pass below
            adapter.enabled = item->OperStatus != IfOperStatusNotPresent;
            adapter.status = operStatusString(item->OperStatus);
            if (!adapter.id.empty()) adapters.push_back(adapter);
        }
    }

    // Pass 2: netsh interface show interface (authoritative for admin status)
    // GAA may hide disabled adapters or misreport their admin enabled state.
    // netsh reliably lists ALL interfaces with correct Admin State.
    // NOTE: MinGW _wpopen+fgetws does NOT correctly convert UTF-8 to UTF-16 on
    //       codepage 65001 systems. Use _popen+fgets+MultiByteToWideChar instead.
    FILE* pipe = _popen("netsh interface show interface", "r");
    if (pipe) {
        char lineBuf[4096];
        int lineNum = 0;
        while (fgets(lineBuf, static_cast<int>(sizeof(lineBuf)), pipe)) {
            ++lineNum;
            // Convert UTF-8 narrow line to UTF-16 wide string
            int wideLen = MultiByteToWideChar(CP_UTF8, 0, lineBuf, -1, nullptr, 0);
            if (wideLen <= 1) continue;
            std::wstring wline;
            wline.resize(static_cast<size_t>(wideLen) - 1);
            MultiByteToWideChar(CP_UTF8, 0, lineBuf, -1, wline.data(), wideLen);

            // Trim trailing whitespace
            while (!wline.empty() && (wline.back() == L'\n' || wline.back() == L'\r' || wline.back() == L' ' || wline.back() == L'\t')) wline.pop_back();
            if (wline.empty()) continue;
            if (lineNum <= 2) continue;
            if (wline.find_first_not_of(L'-') == std::wstring::npos) continue;

            // Split into tokens by whitespace
            std::vector<std::wstring> tokens;
            std::wstring cur;
            bool inSpace = true;
            for (wchar_t ch : wline) {
                if (ch == L' ' || ch == L'\t') {
                    if (!inSpace && !cur.empty()) { tokens.push_back(cur); cur.clear(); }
                    inSpace = true;
                } else { cur += ch; inSpace = false; }
            }
            if (!cur.empty()) tokens.push_back(cur);
            // Format: AdminState ConnState Type InterfaceName...
            if (tokens.size() < 4) continue;

            std::wstring adminStatus = tokens[0];
            std::wstring connStatus  = tokens[1];
            std::wstring connType    = tokens[2];
            std::wstring ifName;
            for (size_t i = 3; i < tokens.size(); ++i) {
                if (!ifName.empty()) ifName += L' ';
                ifName += tokens[i];
            }
            if (ifName.empty()) continue;

            // Only process "Dedicated" type interfaces
            std::wstring lowerType = connType;
            for (auto& ch : lowerType) ch = towlower(ch);
            if (lowerType != L"dedicated" && lowerType != L"\u4e13\u7528") continue;

            // Skip known non-ethernet interface name patterns
            std::wstring lowerName = ifName;
            for (auto& ch : lowerName) ch = towlower(ch);
            if (lowerName.find(L"wi-fi") != std::wstring::npos ||
                lowerName.find(L"wifi") != std::wstring::npos ||
                lowerName.find(L"bluetooth") != std::wstring::npos ||
                lowerName.find(L"\u84dd\u7259") != std::wstring::npos ||
                lowerName.find(L"virtual") != std::wstring::npos ||
                lowerName.find(L"loopback") != std::wstring::npos ||
                lowerName.find(L"isatap") != std::wstring::npos ||
                lowerName.find(L"vethernet") != std::wstring::npos ||
                lowerName.find(L"hyperv") != std::wstring::npos) {
                continue;
            }

            bool enabled = (adminStatus == L"Enabled" || adminStatus == L"\u5df2\u542f\u7528");
            bool connected = (connStatus == L"Connected" || connStatus == L"\u5df2\u8fde\u63a5");
            std::string statusStr = enabled ? (connected ? "up" : "down") : "disabled";
            std::string nameUtf8 = wideToUtf8(ifName);

            // Update existing adapter or add new one
            bool found = false;
            for (auto& existing : adapters) {
                if (existing.id == nameUtf8 || existing.name == nameUtf8) {
                    existing.enabled = enabled;
                    existing.connected = connected;
                    existing.status = statusStr;
                    found = true;
                    break;
                }
            }
            if (!found) {
                WiredAdapter adapter;
                adapter.name = nameUtf8;
                adapter.id = nameUtf8;
                adapter.connected = connected;
                adapter.enabled = enabled;
                adapter.status = statusStr;
                adapters.push_back(adapter);
            }
        }
        _pclose(pipe);
    }

    // Pass 3: Filter out non-ethernet adapters
    // Remove WiFi, Bluetooth, VPN/VNIC adapters that slipped through GAA+netsh
    {
        std::vector<WiredAdapter> filtered;
        for (const auto& a : adapters) {
            std::string lower = a.name;
            for (auto& ch : lower) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            bool skip = (lower.find("wi-fi") != std::string::npos ||
                         lower.find("wifi") != std::string::npos ||
                         lower.find("wlan") != std::string::npos ||
                         lower.find("bluetooth") != std::string::npos ||
                         lower.find("\u84dd\u7259") != std::string::npos ||
                         lower.find("vnic") != std::string::npos ||
                         lower.find("vethernet") != std::string::npos ||
                         lower.find("hyper") != std::string::npos ||
                         lower.find("virtual") != std::string::npos ||
                         lower.find("loopback") != std::string::npos ||
                         lower.find("\u672c\u5730\u8fde\u63a5") != std::string::npos);
            if (!skip) filtered.push_back(a);
        }
        adapters = std::move(filtered);
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
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = L"netsh.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    result.ok = ShellExecuteExW(&sei) && sei.hProcess != nullptr;
    if (result.ok) {
        WaitForSingleObject(sei.hProcess, 30000);
        DWORD exitCode = 0;
        if (GetExitCodeProcess(sei.hProcess, &exitCode)) {
            result.ok = (exitCode == 0);
        }
        CloseHandle(sei.hProcess);
    }
    result.requested = true;
    result.elevated = result.ok;
    result.status = result.ok ? (enabled ? "enable_requested" : "disable_requested") : "failed";
    if (!result.ok) result.error = result.requested ? "netsh returned non-zero exit code" : "could not start elevated netsh";
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
