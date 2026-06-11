#include "core/wifi_detector.h"

#include "core/util.h"

#include <windows.h>
#include "core/dyn_wlan.h"
#include <wlanapi.h>  // type definitions only; no function calls linked

#include <memory>

namespace ww {

// Using dyn_wlan::WlanHandle

static std::string ssidToString(const DOT11_SSID& ssid) {
    if (ssid.uSSIDLength == 0) return "";
    return std::string(reinterpret_cast<const char*>(ssid.ucSSID), reinterpret_cast<const char*>(ssid.ucSSID) + ssid.uSSIDLength);
}

static std::wstring testSsidOverride() {
    std::wstring filePath = getEnvWide(L"WW_TEST_CURRENT_SSID_FILE");
    if (!filePath.empty()) {
        std::string ssid = readTextFileUtf8(filePath);
        while (!ssid.empty() && (ssid.back() == '\r' || ssid.back() == '\n')) {
            ssid.pop_back();
        }
        if (!ssid.empty()) return utf8ToWide(ssid);
    }
    return getEnvWide(L"WW_TEST_CURRENT_SSID");
}

WifiStatus getCurrentWifi() {
    WifiStatus status;
    std::wstring testSsid = testSsidOverride();
    if (!testSsid.empty()) {
        if (testSsid == L"<no-adapter>") {
            status.error = "No WiFi adapter";
            return status;
        }
        status.adapter_available = true;
        status.connected = testSsid != L"<disconnected>";
        status.ssid = status.connected ? wideToUtf8(testSsid) : "";
        status.interface_description = "WW_TEST adapter";
        return status;
    }

    if (!dyn_wlan::isAvailable()) {
        status.error = "wlanapi.dll not available";
        return status;
    }
    DWORD negotiated = 0;
    dyn_wlan::WlanHandle wlan;
    DWORD result = dyn_wlan::fn_WlanOpenHandle(2, nullptr, &negotiated, &wlan.handle);
    if (result != ERROR_SUCCESS) {
        status.error = "WlanOpenHandle failed";
        return status;
    }

    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    result = dyn_wlan::fn_WlanEnumInterfaces(wlan.handle, nullptr, &interfaces);
    if (result != ERROR_SUCCESS || !interfaces) {
        status.error = "WlanEnumInterfaces failed";
        return status;
    }
    std::unique_ptr<WLAN_INTERFACE_INFO_LIST, decltype(&dyn_wlan::freeMemory)> interfaceGuard(interfaces, dyn_wlan::freeMemory);
    status.adapter_available = interfaces->dwNumberOfItems > 0;

    for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
        const auto& item = interfaces->InterfaceInfo[i];
        if (item.isState != wlan_interface_state_connected) continue;

        status.interface_description = wideToUtf8(item.strInterfaceDescription);
        DWORD dataSize = 0;
        PWLAN_CONNECTION_ATTRIBUTES attrs = nullptr;
        WLAN_OPCODE_VALUE_TYPE opcode{};
        result = dyn_wlan::fn_WlanQueryInterface(
            wlan.handle,
            &item.InterfaceGuid,
            wlan_intf_opcode_current_connection,
            nullptr,
            &dataSize,
            reinterpret_cast<PVOID*>(&attrs),
            &opcode);

        if (result == ERROR_SUCCESS && attrs) {
            std::unique_ptr<WLAN_CONNECTION_ATTRIBUTES, decltype(&dyn_wlan::freeMemory)> attrsGuard(attrs, dyn_wlan::freeMemory);
            status.connected = true;
            status.ssid = ssidToString(attrs->wlanAssociationAttributes.dot11Ssid);
            return status;
        }
    }

    return status;
}

bool isNoWifiAdapter(const WifiStatus& status) {
    return !status.adapter_available && (status.error.empty() || status.error == "No WiFi adapter");
}

std::vector<std::string> getKnownWifiProfiles() {
    std::vector<std::string> profiles;
    DWORD negotiated = 0;
    dyn_wlan::WlanHandle wlan;
    if (!dyn_wlan::isAvailable()) return profiles;
    if (dyn_wlan::fn_WlanOpenHandle(2, nullptr, &negotiated, &wlan.handle) != ERROR_SUCCESS) return profiles;

    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    if (dyn_wlan::fn_WlanEnumInterfaces(wlan.handle, nullptr, &interfaces) != ERROR_SUCCESS || !interfaces) return profiles;
    std::unique_ptr<WLAN_INTERFACE_INFO_LIST, decltype(&dyn_wlan::freeMemory)> interfaceGuard(interfaces, dyn_wlan::freeMemory);

    for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
        PWLAN_PROFILE_INFO_LIST list = nullptr;
        if (dyn_wlan::fn_WlanGetProfileList(wlan.handle, &interfaces->InterfaceInfo[i].InterfaceGuid, nullptr, &list) != ERROR_SUCCESS || !list) continue;
        std::unique_ptr<WLAN_PROFILE_INFO_LIST, decltype(&dyn_wlan::freeMemory)> profileGuard(list, dyn_wlan::freeMemory);
        for (DWORD j = 0; j < list->dwNumberOfItems; ++j) {
            profiles.push_back(wideToUtf8(list->ProfileInfo[j].strProfileName));
        }
    }
    return profiles;
}

}
